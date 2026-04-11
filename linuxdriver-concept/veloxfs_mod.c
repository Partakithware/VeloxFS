// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * veloxfs_mod.c  —  VeloxFS v6 Linux Kernel Driver
 *
 * Author : Maxwell Wingate
 * Target : Linux 5.15 LTS through 7.0+ (GCC / Clang, x86-64 or ARM64)
 *
 * This driver uses veloxfs.h as the single authoritative source of truth
 * for all on-disk structure definitions, magic numbers, FAT sentinel values,
 * and layout constants.  veloxfs.h is included WITHOUT veloxfs_IMPLEMENTATION
 * so only the struct/constant/prototype section is compiled — the full
 * userspace I/O implementation is excluded.
 *
 * Kernel compatibility matrix (handled via version guards):
 *   5.15 – 6.3  : user_namespace, i_atime direct, mount_bdev, writepage
 *   5.18        : alloc_inode_sb() added
 *   6.3         : mnt_idmap replaces user_namespace
 *   6.4         : filemap_splice_read replaces generic_file_splice_read
 *   6.6         : inode timestamp moved to accessors
 *   6.11        : writepage removed; write_begin/end use folio; fillattr request_mask
 *   7.0         : mount_bdev removed → init_fs_context + get_tree_bdev;
 *                 write_begin/end use kiocb
 *
 * On-disk layout (see veloxfs.h for full documentation):
 *   Block 0          : veloxfs_superblock
 *   fat_start ..     : FAT  (u64 per block)
 *   journal_start .. : journal entries (optional)
 *   inode_start ..   : veloxfs_inode table
 *   dir_start ..     : veloxfs_dirent table  [= inode_start + inode_blocks]
 *   data_start ..    : file data
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/statfs.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/time.h>
#include <linux/pagemap.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#  include <linux/fs_context.h>
#  include <linux/mount.h>
#endif

/* =========================================================================
 * Kernel compatibility shims
 * ====================================================================== */

/* 6.3+: mnt_idmap replaces user_namespace */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
#  define VLX_NS  struct mnt_idmap
#  define inode_init_owner_compat(ns, inode, dir, mode) \
        inode_init_owner((ns), (inode), (dir), (mode))
#  define setattr_prepare_compat(ns, de, attr) \
        setattr_prepare((ns), (de), (attr))
#  define setattr_copy_compat(ns, inode, attr) \
        setattr_copy((ns), (inode), (attr))
#else
#  define VLX_NS  struct user_namespace
#  define inode_init_owner_compat(ns, inode, dir, mode) \
        inode_init_owner((ns), (inode), (dir), (mode))
#  define setattr_prepare_compat(ns, de, attr) \
        setattr_prepare((ns), (de), (attr))
#  define setattr_copy_compat(ns, inode, attr) \
        setattr_copy((ns), (inode), (attr))
#endif

/* 5.18+: alloc_inode_sb() */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
#  define alloc_inode_sb(sb, cache, gfp) kmem_cache_alloc((cache), (gfp))
#endif

/* 6.4+: filemap_splice_read */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#  define VLX_SPLICE_READ  filemap_splice_read
#else
#  define VLX_SPLICE_READ  generic_file_splice_read
#endif

/* 6.6+: inode timestamp accessors */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#  define vlx_inode_atime(i)  inode_get_atime(i)
#  define vlx_inode_mtime(i)  inode_get_mtime(i)
#  define vlx_inode_ctime(i)  inode_get_ctime(i)
static inline void veloxfs_set_inode_times(struct inode *inode, struct timespec64 ts)
{
    inode_set_atime_to_ts(inode, ts);
    inode_set_mtime_to_ts(inode, ts);
    inode_set_ctime_to_ts(inode, ts);
}
static inline void veloxfs_set_inode_ctime(struct inode *inode, struct timespec64 ts)
    { inode_set_ctime_to_ts(inode, ts); }
static inline void veloxfs_set_inode_mtime(struct inode *inode, struct timespec64 ts)
    { inode_set_mtime_to_ts(inode, ts); }
#else
#  define vlx_inode_atime(i)  ((i)->i_atime)
#  define vlx_inode_mtime(i)  ((i)->i_mtime)
#  define vlx_inode_ctime(i)  ((i)->i_ctime)
static inline void veloxfs_set_inode_times(struct inode *inode, struct timespec64 ts)
    { inode->i_atime = inode->i_mtime = inode->i_ctime = ts; }
static inline void veloxfs_set_inode_ctime(struct inode *inode, struct timespec64 ts)
    { inode->i_ctime = ts; }
static inline void veloxfs_set_inode_mtime(struct inode *inode, struct timespec64 ts)
    { inode->i_mtime = ts; }
#endif

/* 6.11+: generic_fillattr requires request_mask */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
#  define VLX_FILLATTR(ns, inode, stat) \
        generic_fillattr((ns), STATX_BASIC_STATS, (inode), (stat))
#else
#  define VLX_FILLATTR(ns, inode, stat) \
        generic_fillattr((ns), (inode), (stat))
#endif


#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
/* 7.0+ uses a struct for i_state; use the kernel's preferred accessor */
#define VLX_INODE_IS_NEW(i) (inode_state_read_once(i) & I_NEW)
#else
/* Legacy kernels use a simple unsigned long */
#define VLX_INODE_IS_NEW(i) ((i)->i_state & I_NEW)
#endif

/* =========================================================================
 * veloxfs.h kernel integration
 *
 * Define hook macros pointing at kernel primitives, then include veloxfs.h
 * WITHOUT veloxfs_IMPLEMENTATION.  Only the on-disk struct definitions,
 * constants, and public API prototypes are compiled into the module.
 *
 * C99 fixed-width types (uint8_t etc.) are not exported by default in kernel
 * context; alias them from the kernel's own __u* / __s* types first.
 * ====================================================================== */

typedef __u8   uint8_t;
typedef __u16  uint16_t;
typedef __u32  uint32_t;
typedef __u64  uint64_t;
typedef __s8   int8_t;
typedef __s16  int16_t;
typedef __s32  int32_t;
typedef __s64  int64_t;

#define veloxfs_MALLOC(sz)          kmalloc((sz), GFP_KERNEL)
#define veloxfs_CALLOC(n, sz)       kzalloc((size_t)(n) * (size_t)(sz), GFP_KERNEL)
#define veloxfs_FREE(p)             kfree(p)
#define veloxfs_MEMSET(d, c, n)     memset((d), (c), (n))
#define veloxfs_MEMCPY(d, s, n)     memcpy((d), (s), (n))
#define veloxfs_MEMMOVE(d, s, n)    memmove((d), (s), (n))
#define veloxfs_STRLEN(s)           strlen(s)
#define veloxfs_STRCMP(a, b)        strcmp((a), (b))
#define veloxfs_STRNCMP(a, b, n)    strncmp((a), (b), (n))
#define veloxfs_STRCHR(s, c)        strchr((s), (c))
#define veloxfs_STRNCPY(d, s, n)    strncpy((d), (s), (n))
#define veloxfs_SNPRINTF(d, n, ...) snprintf((d), (n), __VA_ARGS__)
#define veloxfs_LOG(fmt, ...)       pr_info(fmt, ##__VA_ARGS__)
#define veloxfs_TIME()              ((uint64_t)ktime_get_real_seconds())

/*
 * Include veloxfs.h for on-disk definitions only — no implementation.
 * veloxfs_IMPLEMENTATION is intentionally NOT defined.
 */
#include "veloxfs.h"

/* =========================================================================
 * Bridge: map VELOXFS_ screaming-snake constants to veloxfs.h names.
 * No values are re-defined; these are pure macro aliases.
 * ====================================================================== */
#define VELOXFS_MAGIC_V6        veloxfs_MAGIC_V6
#define VELOXFS_MAGIC           veloxfs_MAGIC
#define VELOXFS_VERSION         veloxfs_VERSION
#define VELOXFS_BS_MIN          veloxfs_BS_MIN
#define VELOXFS_BS_MAX          veloxfs_BS_MAX
#define VELOXFS_INLINE_EXTENTS  veloxfs_INLINE_EXTENTS
#define VELOXFS_INODE_F_EXTENTS veloxfs_INODE_F_EXTENTS
#define VELOXFS_INODE_F_DIR     veloxfs_INODE_F_DIR
#define VELOXFS_MAX_PATH        veloxfs_MAX_PATH
#define VELOXFS_JOURNAL_SIZE    veloxfs_JOURNAL_SIZE
#define VELOXFS_FAT_FREE        veloxfs_FAT_FREE
#define VELOXFS_FAT_EOF         veloxfs_FAT_EOF
#define VELOXFS_FAT_BAD         veloxfs_FAT_BAD

/*
 * veloxfs.h defines on-disk types as anonymous packed structs with typedef
 * names: veloxfs_superblock, veloxfs_inode, veloxfs_dirent, veloxfs_extent.
 * The driver uses these typedef names directly throughout — they are NOT
 * prefixed with "struct" because they are typedefs, not tagged structs.
 */
static_assert(sizeof(veloxfs_inode) == 256,
              "veloxfs_inode from veloxfs.h must be 256 bytes");

/* =========================================================================
 * VFS inode number mapping
 *   VFS ino 1           → synthetic root (no on-disk inode)
 *   VFS ino vlx_num + 1 → veloxfs_inode with inode_num == vlx_num
 * ====================================================================== */
#define VELOXFS_ROOT_INO    1UL
#define VLX_TO_VFS_INO(n)  ((unsigned long)((n) + 1))
#define VFS_TO_VLX_INO(i)  ((__u64)((i) - 1))

/* =========================================================================
 * In-memory superblock info  (sb->s_fs_info)
 *
 * sbi->super   is  veloxfs_superblock  (on-disk type from veloxfs.h)
 * sbi->inodes  is  veloxfs_inode *     (on-disk type from veloxfs.h)
 * sbi->directory is veloxfs_dirent *   (on-disk type from veloxfs.h)
 * ====================================================================== */
struct veloxfs_sb_info {
    veloxfs_superblock  super;
    __u64              *fat;
    veloxfs_inode      *inodes;
    veloxfs_dirent     *directory;
    __u64               num_inodes;
    __u64               num_dirents;
    __u64               dir_start_blk;
    __u64               dir_blocks;
    __u64               last_alloc_hint;
    struct mutex        lock;
    int                 dirty_fat;
    int                 dirty_inodes;
    int                 dirty_dir;
};

/* =========================================================================
 * In-memory inode info
 * vi is veloxfs_inode (on-disk type from veloxfs.h); vfs_inode is the VFS inode.
 * ====================================================================== */
struct veloxfs_inode_info {
    veloxfs_inode  vi;
    struct inode   vfs_inode;
};

static struct kmem_cache *veloxfs_inode_cachep;

static inline struct veloxfs_inode_info *VELOXFS_I(struct inode *inode)
{
    return container_of(inode, struct veloxfs_inode_info, vfs_inode);
}

static inline struct veloxfs_sb_info *VELOXFS_SB(struct super_block *sb)
{
    return (struct veloxfs_sb_info *)sb->s_fs_info;
}

/* =========================================================================
 * Forward declarations
 * ====================================================================== */
static const struct inode_operations veloxfs_dir_inode_ops;
static const struct file_operations  veloxfs_dir_ops;
static const struct inode_operations veloxfs_file_inode_ops;
static const struct file_operations  veloxfs_file_ops;
static const struct address_space_operations veloxfs_aops;

/* =========================================================================
 * Layout helper — mirrors calc_dir_blocks() in veloxfs.h
 * ====================================================================== */
static __u64 vlx_calc_dir_blocks(__u64 block_count)
{
    __u64 n = block_count / 100;
    return n ? n : 1;
}

/* =========================================================================
 * Block flush helpers
 * Caller must hold sbi->lock.
 * ====================================================================== */
static int vlx_flush_region(struct super_block *sb,
                              const void *data, size_t total_bytes,
                              __u64 start_vlx_block)
{
    struct veloxfs_sb_info *sbi = VELOXFS_SB(sb);
    __u64 ratio = sbi->super.block_size / (__u64)sb->s_blocksize;
    __u64 total_vfs_blocks = (total_bytes + sb->s_blocksize - 1) /
                              (__u64)sb->s_blocksize;
    __u64 j;

    for (j = 0; j < total_vfs_blocks; j++) {
        struct buffer_head *bh;
        sector_t phys  = (sector_t)(start_vlx_block * ratio + j);
        size_t   off   = (size_t)(j * (__u64)sb->s_blocksize);
        size_t   cplen = min_t(size_t, (size_t)sb->s_blocksize, total_bytes - off);

        bh = sb_getblk(sb, phys);
        if (!bh) return -EIO;
        lock_buffer(bh);
        memset(bh->b_data, 0, sb->s_blocksize);
        memcpy(bh->b_data, (const u8 *)data + off, cplen);
        set_buffer_uptodate(bh);
        mark_buffer_dirty(bh);
        unlock_buffer(bh);
        brelse(bh);
    }
    return 0;
}

static int vlx_flush_fat(struct super_block *sb)
{
    struct veloxfs_sb_info *sbi = VELOXFS_SB(sb);
    if (!sbi->dirty_fat) return 0;
    if (vlx_flush_region(sb, sbi->fat,
                          (size_t)(sbi->super.block_count * sizeof(__u64)),
                          sbi->super.fat_start) < 0)
        return -EIO;
    sbi->dirty_fat = 0;
    return 0;
}

static int vlx_flush_inodes(struct super_block *sb)
{
    struct veloxfs_sb_info *sbi = VELOXFS_SB(sb);
    if (!sbi->dirty_inodes) return 0;
    if (vlx_flush_region(sb, sbi->inodes,
                          (size_t)(sbi->num_inodes * sizeof(veloxfs_inode)),
                          sbi->super.inode_start) < 0)
        return -EIO;
    sbi->dirty_inodes = 0;
    return 0;
}

static int vlx_flush_dir(struct super_block *sb)
{
    struct veloxfs_sb_info *sbi = VELOXFS_SB(sb);
    if (!sbi->dirty_dir) return 0;
    if (vlx_flush_region(sb, sbi->directory,
                          (size_t)(sbi->num_dirents * sizeof(veloxfs_dirent)),
                          sbi->dir_start_blk) < 0)
        return -EIO;
    sbi->dirty_dir = 0;
    return 0;
}

/* =========================================================================
 * In-memory FAT operations
 * ====================================================================== */

static __u64 vlx_inode_total_blocks(struct veloxfs_sb_info *sbi,
                                     veloxfs_inode *vi)
{
    __u64 total = 0, i, b, safety = 0;
    if (vi->inode_flags & VELOXFS_INODE_F_EXTENTS)
        for (i = 0; i < vi->extent_count; i++)
            total += vi->extents[i].block_count;
    b = vi->fat_head;
    while (b && b < sbi->super.block_count) {
        __u64 next = sbi->fat[b];
        total++;
        if (next == VELOXFS_FAT_EOF || next == VELOXFS_FAT_FREE ||
            next == VELOXFS_FAT_BAD) break;
        b = next;
        if (++safety > sbi->super.block_count) break;
    }
    return total;
}

static __u64 vlx_inode_get_phys(struct veloxfs_sb_info *sbi,
                                  veloxfs_inode *vi, __u64 n)
{
    if (vi->inode_flags & VELOXFS_INODE_F_EXTENTS) {
        __u64 skipped = 0, i;
        for (i = 0; i < vi->extent_count; i++) {
            veloxfs_extent *e = &vi->extents[i];
            if (n < skipped + e->block_count)
                return e->start_block + (n - skipped);
            skipped += e->block_count;
        }
        if (vi->fat_head) {
            __u64 b = vi->fat_head, rel = n - skipped, safety = 0;
            for (; rel && b && b < sbi->super.block_count; rel--) {
                __u64 next = sbi->fat[b];
                if (next == VELOXFS_FAT_EOF || next == VELOXFS_FAT_FREE ||
                    next == VELOXFS_FAT_BAD) return 0;
                b = next;
                if (++safety > sbi->super.block_count) return 0;
            }
            return rel == 0 ? b : 0;
        }
        return 0;
    }
    {
        __u64 b = vi->fat_head, safety = 0;
        for (; n && b && b < sbi->super.block_count; n--) {
            __u64 next = sbi->fat[b];
            if (next == VELOXFS_FAT_EOF || next == VELOXFS_FAT_FREE ||
                next == VELOXFS_FAT_BAD) return 0;
            b = next;
            if (++safety > sbi->super.block_count) return 0;
        }
        return b;
    }
}

static __u64 vlx_alloc_contiguous(struct veloxfs_sb_info *sbi,
                                    __u64 wanted, __u64 *out_start)
{
    __u64 data_start  = sbi->super.data_start;
    __u64 block_count = sbi->super.block_count;
    __u64 best_start = 0, best_len = 0, run_start = 0, run_len = 0;
    __u64 hint = sbi->last_alloc_hint;
    int pass;

    if (hint < data_start) hint = data_start;

    for (pass = 0; pass < 2 && best_len < wanted; pass++) {
        __u64 scan_start = pass ? data_start : hint;
        __u64 scan_end   = pass ? hint       : block_count;
        __u64 i;
        run_len = 0;
        for (i = scan_start; i < scan_end; i++) {
            if (sbi->fat[i] == VELOXFS_FAT_FREE) {
                if (!run_len) run_start = i;
                if (++run_len >= wanted) {
                    best_start = run_start; best_len = wanted; goto found;
                }
            } else {
                if (run_len > best_len) { best_len = run_len; best_start = run_start; }
                run_len = 0;
            }
        }
        if (run_len > best_len) { best_len = run_len; best_start = run_start; }
    }
found:
    if (!best_len) { *out_start = 0; return 0; }
    {
        __u64 alloc = min(best_len, wanted), i;
        for (i = 0; i < alloc; i++)
            sbi->fat[best_start + i] = VELOXFS_FAT_EOF;
        sbi->dirty_fat       = 1;
        sbi->last_alloc_hint = best_start + alloc;
        if (sbi->last_alloc_hint >= block_count)
            sbi->last_alloc_hint = data_start;
        *out_start = best_start;
        return alloc;
    }
}

static int vlx_inode_extend(struct veloxfs_sb_info *sbi,
                              veloxfs_inode *vi, __u64 extra_blocks)
{
    __u64 remaining = extra_blocks;

    if ((vi->inode_flags & VELOXFS_INODE_F_EXTENTS) && vi->extent_count > 0) {
        veloxfs_extent *last = &vi->extents[vi->extent_count - 1];
        __u64 next_phys = last->start_block + last->block_count, grown = 0;
        while (grown < remaining && next_phys + grown < sbi->super.block_count &&
               sbi->fat[next_phys + grown] == VELOXFS_FAT_FREE) {
            sbi->fat[next_phys + grown] = VELOXFS_FAT_EOF; grown++;
        }
        if (grown) {
            last->block_count += grown; remaining -= grown;
            sbi->dirty_fat = sbi->dirty_inodes = 1;
        }
    }

    while (remaining > 0) {
        __u64 start = 0, got = vlx_alloc_contiguous(sbi, remaining, &start);
        if (!got) return -ENOSPC;

        if ((vi->inode_flags & VELOXFS_INODE_F_EXTENTS) &&
             vi->extent_count < VELOXFS_INLINE_EXTENTS) {
            vi->extents[vi->extent_count].start_block = start;
            vi->extents[vi->extent_count].block_count = got;
            vi->extent_count++;
            sbi->dirty_inodes = 1;
        } else {
            __u64 i;
            for (i = 0; i + 1 < got; i++) sbi->fat[start + i] = start + i + 1;
            sbi->fat[start + got - 1] = VELOXFS_FAT_EOF;
            if (!vi->fat_head) {
                vi->fat_head = start;
            } else {
                __u64 tail = vi->fat_head;
                while (sbi->fat[tail] != VELOXFS_FAT_EOF &&
                       sbi->fat[tail] != VELOXFS_FAT_FREE &&
                       sbi->fat[tail] < sbi->super.block_count)
                    tail = sbi->fat[tail];
                sbi->fat[tail] = start;
            }
            sbi->dirty_fat = sbi->dirty_inodes = 1;
        }
        remaining -= got;
    }
    return 0;
}

static void vlx_inode_free_all(struct veloxfs_sb_info *sbi, veloxfs_inode *vi)
{
    __u64 i, b, safety = 0;
    if (vi->inode_flags & VELOXFS_INODE_F_EXTENTS) {
        for (i = 0; i < vi->extent_count; i++) {
            veloxfs_extent *e = &vi->extents[i];
            __u64 b2;
            for (b2 = 0; b2 < e->block_count; b2++) {
                __u64 blk = e->start_block + b2;
                if (blk < sbi->super.block_count) sbi->fat[blk] = VELOXFS_FAT_FREE;
            }
        }
        vi->extent_count = 0; sbi->dirty_fat = 1;
    }
    b = vi->fat_head;
    while (b && b < sbi->super.block_count) {
        __u64 next = sbi->fat[b];
        sbi->fat[b] = VELOXFS_FAT_FREE; sbi->dirty_fat = 1;
        if (next == VELOXFS_FAT_EOF || next == VELOXFS_FAT_FREE ||
            next == VELOXFS_FAT_BAD) break;
        b = next;
        if (++safety > sbi->super.block_count) break;
    }
    vi->fat_head = 0; sbi->dirty_inodes = 1;
}

/* =========================================================================
 * Directory table helpers
 * ====================================================================== */
static bool vlx_is_direct_child(const char *parent, const char *path)
{
    size_t plen = strlen(parent);
    if (!strcmp(parent, "/"))
        return path[0] == '/' && path[1] && !strchr(path + 1, '/');
    if (strncmp(path, parent, plen) || path[plen] != '/') return false;
    return !strchr(path + plen + 1, '/');
}

static veloxfs_dirent *vlx_find_dirent(struct veloxfs_sb_info *sbi,
                                        const char *path)
{
    __u64 i;
    for (i = 0; i < sbi->num_dirents; i++)
        if (sbi->directory[i].inode_num &&
            !strcmp(sbi->directory[i].path, path))
            return &sbi->directory[i];
    return NULL;
}

static veloxfs_inode *vlx_get_disk_inode(struct veloxfs_sb_info *sbi,
                                           __u64 inode_num)
{
    if (!inode_num || inode_num > sbi->num_inodes) return NULL;
    if (!sbi->inodes[inode_num - 1].inode_num) return NULL;
    return &sbi->inodes[inode_num - 1];
}

/* =========================================================================
 * VFS inode cache
 * ====================================================================== */
static struct inode *veloxfs_alloc_inode(struct super_block *sb)
{
    struct veloxfs_inode_info *vi;
    vi = alloc_inode_sb(sb, veloxfs_inode_cachep, GFP_KERNEL);
    if (!vi) return NULL;
    memset(&vi->vi, 0, sizeof(vi->vi));
    return &vi->vfs_inode;
}

static void veloxfs_free_inode(struct inode *inode)
{
    kmem_cache_free(veloxfs_inode_cachep, VELOXFS_I(inode));
}

static void veloxfs_init_once(void *obj)
{
    struct veloxfs_inode_info *vi = (struct veloxfs_inode_info *)obj;
    inode_init_once(&vi->vfs_inode);
}

/*
 * Copy a veloxfs_inode (from veloxfs.h) into a VFS inode.
 * veloxfs_inode field names are the canonical on-disk names from veloxfs.h.
 */
static void veloxfs_fill_inode(struct inode *inode, const veloxfs_inode *disk)
{
    struct veloxfs_inode_info *vi_info = VELOXFS_I(inode);
    struct veloxfs_sb_info    *sbi     = VELOXFS_SB(inode->i_sb);
    struct timespec64 atime = { .tv_sec = (time64_t)disk->atime, .tv_nsec = 0 };
    struct timespec64 mtime = { .tv_sec = (time64_t)disk->mtime, .tv_nsec = 0 };
    struct timespec64 ctime = { .tv_sec = (time64_t)disk->ctime, .tv_nsec = 0 };

    memcpy(&vi_info->vi, disk, sizeof(*disk));

    inode->i_ino  = VLX_TO_VFS_INO(disk->inode_num);
    inode->i_mode = (umode_t)disk->mode;
    i_uid_write(inode, (uid_t)disk->uid);
    i_gid_write(inode, (gid_t)disk->gid);
    inode->i_size = (loff_t)disk->size;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    inode_set_atime_to_ts(inode, atime);
    inode_set_mtime_to_ts(inode, mtime);
    inode_set_ctime_to_ts(inode, ctime);
#else
    inode->i_atime = atime;
    inode->i_mtime = mtime;
    inode->i_ctime = ctime;
#endif

    inode->i_blocks = vlx_inode_total_blocks(sbi, (veloxfs_inode *)disk) *
                      (sbi->super.block_size >> 9);
}

static struct inode *veloxfs_iget(struct super_block *sb, __u64 vlx_ino)
{
    struct veloxfs_sb_info *sbi = VELOXFS_SB(sb);
    struct inode           *inode;
    veloxfs_inode          *disk_vi;

    inode = iget_locked(sb, VLX_TO_VFS_INO(vlx_ino));
    if (!inode) return ERR_PTR(-ENOMEM);
    if (!VLX_INODE_IS_NEW(inode)) return inode;

    mutex_lock(&sbi->lock);
    disk_vi = vlx_get_disk_inode(sbi, vlx_ino);
    if (!disk_vi) {
        mutex_unlock(&sbi->lock);
        iget_failed(inode);
        return ERR_PTR(-ENOENT);
    }
    veloxfs_fill_inode(inode, disk_vi);
    mutex_unlock(&sbi->lock);

    if (S_ISDIR(inode->i_mode)) {
        inode->i_op  = &veloxfs_dir_inode_ops;
        inode->i_fop = &veloxfs_dir_ops;
        set_nlink(inode, 2);
    } else {
        inode->i_op             = &veloxfs_file_inode_ops;
        inode->i_fop            = &veloxfs_file_ops;
        inode->i_mapping->a_ops = &veloxfs_aops;
        set_nlink(inode, 1);
    }
    unlock_new_inode(inode);
    return inode;
}

/* =========================================================================
 * get_block — maps VFS logical block → physical device sector
 * ====================================================================== */
static int veloxfs_get_block(struct inode *inode, sector_t iblock,
                              struct buffer_head *bh_result, int create)
{
    struct veloxfs_inode_info *vi_info = VELOXFS_I(inode);
    struct veloxfs_sb_info    *sbi     = VELOXFS_SB(inode->i_sb);
    __u64 vlx_bs  = sbi->super.block_size;
    __u64 vfs_bs  = (unsigned long)inode->i_sb->s_blocksize;
    __u64 ratio   = vlx_bs / vfs_bs;
    __u64 vlx_blk = (__u64)iblock / ratio;
    __u64 off_in  = (__u64)iblock % ratio;
    __u64 phys_vlx;
    int   err = 0;

    mutex_lock(&sbi->lock);
    phys_vlx = vlx_inode_get_phys(sbi, &vi_info->vi, vlx_blk);

    if (!phys_vlx) {
        if (!create) goto out;
        {
            __u64 have = vlx_inode_total_blocks(sbi, &vi_info->vi);
            __u64 need = vlx_blk + 1;
            if (need > have) {
                err = vlx_inode_extend(sbi, &vi_info->vi, need - have);
                if (err) goto out;
                sbi->inodes[vi_info->vi.inode_num - 1] = vi_info->vi;
            }
        }
        phys_vlx = vlx_inode_get_phys(sbi, &vi_info->vi, vlx_blk);
        if (!phys_vlx) { err = -EIO; goto out; }
        set_buffer_new(bh_result);
    }
    map_bh(bh_result, inode->i_sb, (sector_t)(phys_vlx * ratio + off_in));
out:
    mutex_unlock(&sbi->lock);
    return err;
}

/* =========================================================================
 * Address space operations
 * ====================================================================== */
static int veloxfs_read_folio(struct file *file, struct folio *folio)
{
    return mpage_read_folio(folio, veloxfs_get_block);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static int veloxfs_writepages(struct address_space *mapping,
                               struct writeback_control *wbc)
{
    return mpage_writepages(mapping, wbc, veloxfs_get_block);
}
#else
static int veloxfs_writepage(struct page *page, struct writeback_control *wbc)
{
    return block_write_full_page(page, veloxfs_get_block, wbc);
}
#endif

/* Helper to update size after write_end (shared across all variants) */
static void vlx_write_end_update_size(struct inode *inode, loff_t pos,
                                       unsigned copied)
{
    struct veloxfs_sb_info    *sbi     = VELOXFS_SB(inode->i_sb);
    struct veloxfs_inode_info *vi_info = VELOXFS_I(inode);
    loff_t new_end = pos + copied;
    mutex_lock(&sbi->lock);
    if ((__u64)new_end > vi_info->vi.size) {
        vi_info->vi.size                             = (__u64)new_end;
        sbi->inodes[vi_info->vi.inode_num - 1].size = (__u64)new_end;
        sbi->dirty_inodes = 1;
    }
    mutex_unlock(&sbi->lock);
    mark_inode_dirty(inode);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)

static int veloxfs_write_begin(const struct kiocb *iocb,
                                struct address_space *mapping,
                                loff_t pos, unsigned len,
                                struct folio **foliop, void **fsdata)
{
    return block_write_begin(mapping, pos, len, foliop, veloxfs_get_block);
}

static int veloxfs_write_end(const struct kiocb *iocb,
                              struct address_space *mapping,
                              loff_t pos, unsigned len, unsigned copied,
                              struct folio *folio, void *fsdata)
{
    int ret = generic_write_end(iocb, mapping, pos, len, copied, folio, fsdata);
    if (ret > 0) vlx_write_end_update_size(mapping->host, pos, copied);
    return ret;
}

#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)

static int veloxfs_write_begin(struct file *file,
                                struct address_space *mapping,
                                loff_t pos, unsigned len,
                                struct folio **foliop, void **fsdata)
{
    return block_write_begin(mapping, pos, len, foliop, veloxfs_get_block);
}

static int veloxfs_write_end(struct file *file,
                              struct address_space *mapping,
                              loff_t pos, unsigned len, unsigned copied,
                              struct folio *folio, void *fsdata)
{
    int ret = generic_write_end(file, mapping, pos, len, copied, folio, fsdata);
    if (ret > 0) vlx_write_end_update_size(mapping->host, pos, copied);
    return ret;
}

#else

static int veloxfs_write_begin(struct file *file,
                                struct address_space *mapping,
                                loff_t pos, unsigned len,
                                struct page **pagep, void **fsdata)
{
    return block_write_begin(mapping, pos, len, pagep, veloxfs_get_block);
}

static int veloxfs_write_end(struct file *file,
                              struct address_space *mapping,
                              loff_t pos, unsigned len, unsigned copied,
                              struct page *page, void *fsdata)
{
    int ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
    if (ret > 0) vlx_write_end_update_size(mapping->host, pos, copied);
    return ret;
}

#endif

static sector_t veloxfs_bmap(struct address_space *mapping, sector_t block)
{
    return generic_block_bmap(mapping, block, veloxfs_get_block);
}

static const struct address_space_operations veloxfs_aops = {
    .read_folio  = veloxfs_read_folio,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
    .writepages  = veloxfs_writepages,
#else
    .writepage   = veloxfs_writepage,
#endif
    .write_begin = veloxfs_write_begin,
    .write_end   = veloxfs_write_end,
    .bmap        = veloxfs_bmap,
};

/* =========================================================================
 * fsync
 * ====================================================================== */
static int veloxfs_fsync(struct file *file, loff_t start, loff_t end,
                          int datasync)
{
    struct inode           *inode = file_inode(file);
    struct veloxfs_sb_info *sbi   = VELOXFS_SB(inode->i_sb);
    int err = file_write_and_wait_range(file, start, end);
    if (err) return err;
    mutex_lock(&sbi->lock);
    vlx_flush_fat(inode->i_sb);
    vlx_flush_inodes(inode->i_sb);
    vlx_flush_dir(inode->i_sb);
    mutex_unlock(&sbi->lock);
    return sync_blockdev(inode->i_sb->s_bdev);
}

/* =========================================================================
 * File inode operations
 * ====================================================================== */
static int veloxfs_getattr(VLX_NS *mnt_ns, const struct path *path,
                            struct kstat *stat, u32 request_mask,
                            unsigned int query_flags)
{
    VLX_FILLATTR(mnt_ns, d_inode(path->dentry), stat);
    return 0;
}

static int veloxfs_setattr(VLX_NS *mnt_ns, struct dentry *dentry,
                            struct iattr *attr)
{
    struct inode              *inode   = d_inode(dentry);
    struct veloxfs_sb_info    *sbi     = VELOXFS_SB(inode->i_sb);
    struct veloxfs_inode_info *vi_info = VELOXFS_I(inode);
    int err = setattr_prepare_compat(mnt_ns, dentry, attr);
    if (err) return err;

    if (attr->ia_valid & ATTR_SIZE) {
        loff_t new_size = attr->ia_size;
        __u64  vlx_bs   = sbi->super.block_size;
        __u64  new_blks = ((__u64)new_size + vlx_bs - 1) / vlx_bs;
        __u64  have_blks;

        mutex_lock(&sbi->lock);
        have_blks = vlx_inode_total_blocks(sbi, &vi_info->vi);
        if (new_blks < have_blks) {
            vlx_inode_free_all(sbi, &vi_info->vi);
            if (new_blks) vlx_inode_extend(sbi, &vi_info->vi, new_blks);
        } else if (new_blks > have_blks) {
            vlx_inode_extend(sbi, &vi_info->vi, new_blks - have_blks);
        }
        vi_info->vi.size = (__u64)new_size;
        sbi->inodes[vi_info->vi.inode_num - 1] = vi_info->vi;
        sbi->dirty_inodes = 1;
        mutex_unlock(&sbi->lock);
        truncate_setsize(inode, new_size);
    }

    setattr_copy_compat(mnt_ns, inode, attr);
    mark_inode_dirty(inode);
    return 0;
}

static const struct inode_operations veloxfs_file_inode_ops = {
    .setattr = veloxfs_setattr,
    .getattr = veloxfs_getattr,
};

static const struct file_operations veloxfs_file_ops = {
    .llseek      = generic_file_llseek,
    .read_iter   = generic_file_read_iter,
    .write_iter  = generic_file_write_iter,
    .mmap        = generic_file_mmap,
    .fsync       = veloxfs_fsync,
    .splice_read = VLX_SPLICE_READ,
};

/* =========================================================================
 * Directory operations
 * ====================================================================== */
static int vlx_dir_path(struct veloxfs_sb_info *sbi, struct inode *dir,
                         char *buf, size_t bufsz)
{
    if (dir->i_ino == VELOXFS_ROOT_INO) {
        strncpy(buf, "/", bufsz); buf[bufsz - 1] = '\0'; return 0;
    }
    {
        __u64 vlx_ino = VFS_TO_VLX_INO(dir->i_ino), i;
        for (i = 0; i < sbi->num_dirents; i++) {
            if (sbi->directory[i].inode_num == vlx_ino) {
                strncpy(buf, sbi->directory[i].path, bufsz - 1);
                buf[bufsz - 1] = '\0'; return 0;
            }
        }
    }
    return -ENOENT;
}

static int veloxfs_iterate(struct file *file, struct dir_context *ctx)
{
    struct inode           *dir = file_inode(file);
    struct veloxfs_sb_info *sbi = VELOXFS_SB(dir->i_sb);
    char   dir_path[VELOXFS_MAX_PATH];
    __u64  emitted = 0, i;
    loff_t skip    = ctx->pos - 2;

    if (!dir_emit_dots(file, ctx)) return 0;

    mutex_lock(&sbi->lock);
    if (vlx_dir_path(sbi, dir, dir_path, sizeof(dir_path)) < 0) {
        mutex_unlock(&sbi->lock); return -ENOENT;
    }

    for (i = 0; i < sbi->num_dirents; i++) {
        veloxfs_dirent *de = &sbi->directory[i];
        veloxfs_inode  *vi;
        const char     *name;
        unsigned int    name_len, dtype;

        if (!de->inode_num || !vlx_is_direct_child(dir_path, de->path)) continue;
        if ((loff_t)emitted < skip) { emitted++; continue; }

        name = strrchr(de->path, '/');
        if (!name || !name[1]) continue;
        name++; name_len = (unsigned int)strlen(name);

        vi    = vlx_get_disk_inode(sbi, de->inode_num);
        dtype = (vi && (vi->inode_flags & VELOXFS_INODE_F_DIR)) ? DT_DIR : DT_REG;

        mutex_unlock(&sbi->lock);
        if (!dir_emit(ctx, name, name_len, VLX_TO_VFS_INO(de->inode_num), dtype))
            return 0;
        mutex_lock(&sbi->lock);
        ctx->pos++; emitted++;
    }
    mutex_unlock(&sbi->lock);
    return 0;
}

static struct dentry *veloxfs_lookup(struct inode *dir, struct dentry *dentry,
                                      unsigned int flags)
{
    struct veloxfs_sb_info *sbi = VELOXFS_SB(dir->i_sb);
    veloxfs_dirent         *de;
    struct inode           *inode = NULL;
    char dir_path[VELOXFS_MAX_PATH], full_path[VELOXFS_MAX_PATH];

    mutex_lock(&sbi->lock);
    if (vlx_dir_path(sbi, dir, dir_path, sizeof(dir_path)) < 0) {
        mutex_unlock(&sbi->lock); return ERR_PTR(-ENOENT);
    }
    if (!strcmp(dir_path, "/"))
        snprintf(full_path, sizeof(full_path), "/%.*s",
                 (int)dentry->d_name.len, dentry->d_name.name);
    else
        snprintf(full_path, sizeof(full_path), "%s/%.*s",
                 dir_path, (int)dentry->d_name.len, dentry->d_name.name);

    de = vlx_find_dirent(sbi, full_path);
    if (de) {
        __u64 vlx_ino = de->inode_num;
        mutex_unlock(&sbi->lock);
        inode = veloxfs_iget(dir->i_sb, vlx_ino);
        if (IS_ERR(inode)) return ERR_CAST(inode);
    } else {
        mutex_unlock(&sbi->lock);
    }
    return d_splice_alias(inode, dentry);
}

/* =========================================================================
 * Common create helper
 * ====================================================================== */
static int veloxfs_mknod(VLX_NS *mnt_ns, struct inode *dir,
                          struct dentry *dentry, umode_t mode, bool is_dir)
{
    struct super_block        *sb      = dir->i_sb;
    struct veloxfs_sb_info    *sbi     = VELOXFS_SB(sb);
    struct inode              *inode;
    struct veloxfs_inode_info *vi_info;
    veloxfs_inode             *disk_vi = NULL;
    veloxfs_dirent            *de;
    char  dir_path[VELOXFS_MAX_PATH], full_path[VELOXFS_MAX_PATH];
    __u64 new_ino = 0, i;
    u64   now     = (u64)ktime_get_real_seconds();

    mutex_lock(&sbi->lock);

    if (vlx_dir_path(sbi, dir, dir_path, sizeof(dir_path)) < 0) {
        mutex_unlock(&sbi->lock); return -ENOENT;
    }
    if (!strcmp(dir_path, "/"))
        snprintf(full_path, sizeof(full_path), "/%.*s",
                 (int)dentry->d_name.len, dentry->d_name.name);
    else
        snprintf(full_path, sizeof(full_path), "%s/%.*s",
                 dir_path, (int)dentry->d_name.len, dentry->d_name.name);

    if (vlx_find_dirent(sbi, full_path)) {
        mutex_unlock(&sbi->lock); return -EEXIST;
    }

    for (i = 0; i < sbi->num_inodes; i++) {
        if (!sbi->inodes[i].inode_num) {
            disk_vi = &sbi->inodes[i]; new_ino = i + 1; break;
        }
    }
    if (!disk_vi) { mutex_unlock(&sbi->lock); return -ENOSPC; }

    de = NULL;
    for (i = 0; i < sbi->num_dirents; i++) {
        if (!sbi->directory[i].inode_num) { de = &sbi->directory[i]; break; }
    }
    if (!de) { mutex_unlock(&sbi->lock); return -ENOSPC; }

    /* Initialise veloxfs_inode fields (names come from veloxfs.h) */
    memset(disk_vi, 0, sizeof(*disk_vi));
    disk_vi->inode_num   = new_ino;
    disk_vi->mode        = (__u32)mode;
    disk_vi->uid         = (__u32)from_kuid(&init_user_ns, current_fsuid());
    disk_vi->gid         = (__u32)from_kgid(&init_user_ns, current_fsgid());
    disk_vi->ctime = disk_vi->mtime = disk_vi->atime = now;
    disk_vi->size        = 0;
    disk_vi->inode_flags = VELOXFS_INODE_F_EXTENTS;
    if (is_dir) disk_vi->inode_flags |= VELOXFS_INODE_F_DIR;

    /* Initialise veloxfs_dirent fields (names come from veloxfs.h) */
    strncpy(de->path, full_path, VELOXFS_MAX_PATH - 1);
    de->path[VELOXFS_MAX_PATH - 1] = '\0';
    de->inode_num = new_ino;
    sbi->dirty_inodes = sbi->dirty_dir = 1;
    mutex_unlock(&sbi->lock);

    inode = new_inode(sb);
    if (!inode) return -ENOMEM;

    vi_info = VELOXFS_I(inode);
    mutex_lock(&sbi->lock);
    memcpy(&vi_info->vi, disk_vi, sizeof(*disk_vi));
    mutex_unlock(&sbi->lock);

    inode->i_ino = VLX_TO_VFS_INO(new_ino);
    inode->i_mode   = mode;
    inode->i_blocks = 0;
    veloxfs_set_inode_times(inode, current_time(inode));
    inode_init_owner_compat(mnt_ns, inode, dir, mode);

    if (is_dir) {
        inode->i_op  = &veloxfs_dir_inode_ops;
        inode->i_fop = &veloxfs_dir_ops;
        set_nlink(inode, 2);
    } else {
        inode->i_op             = &veloxfs_file_inode_ops;
        inode->i_fop            = &veloxfs_file_ops;
        inode->i_mapping->a_ops = &veloxfs_aops;
        set_nlink(inode, 1);
    }
    insert_inode_hash(inode);
    mark_inode_dirty(inode);
    d_instantiate(dentry, inode);
    return 0;
}

static int veloxfs_vfs_create(VLX_NS *mnt_ns, struct inode *dir,
                           struct dentry *dentry, umode_t mode, bool excl)
{
    return veloxfs_mknod(mnt_ns, dir, dentry, mode | S_IFREG, false);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
static struct dentry *veloxfs_vfs_mkdir(VLX_NS *mnt_ns, struct inode *dir,
                                     struct dentry *dentry, umode_t mode)
{
    int err = veloxfs_mknod(mnt_ns, dir, dentry, mode | S_IFDIR, true);
    if (!err) { inc_nlink(dir); return NULL; }
    return ERR_PTR(err);
}
#else
static int veloxfs_vfs_mkdir(VLX_NS *mnt_ns, struct inode *dir,
                          struct dentry *dentry, umode_t mode)
{
    int err = veloxfs_mknod(mnt_ns, dir, dentry, mode | S_IFDIR, true);
    if (!err) inc_nlink(dir);
    return err;
}
#endif

/* =========================================================================
 * Unlink / rmdir / rename
 * ====================================================================== */
static int veloxfs_unlink(struct inode *dir, struct dentry *dentry)
{
    struct inode              *inode   = d_inode(dentry);
    struct veloxfs_sb_info    *sbi     = VELOXFS_SB(inode->i_sb);
    struct veloxfs_inode_info *vi_info = VELOXFS_I(inode);
    struct timespec64          now;
    __u64 vlx_ino = vi_info->vi.inode_num, i;

    if (!vlx_ino) return -ENOENT;

    mutex_lock(&sbi->lock);
    vlx_inode_free_all(sbi, &sbi->inodes[vlx_ino - 1]);
    memset(&sbi->inodes[vlx_ino - 1], 0, sizeof(veloxfs_inode));
    sbi->dirty_inodes = 1;
    for (i = 0; i < sbi->num_dirents; i++) {
        if (sbi->directory[i].inode_num == vlx_ino) {
            memset(&sbi->directory[i], 0, sizeof(veloxfs_dirent));
            sbi->dirty_dir = 1; break;
        }
    }
    mutex_unlock(&sbi->lock);

    now = current_time(inode);
    veloxfs_set_inode_ctime(inode, now);
    veloxfs_set_inode_ctime(dir,   now);
    veloxfs_set_inode_mtime(dir,   now);
    drop_nlink(inode);
    mark_inode_dirty(inode);
    mark_inode_dirty(dir);
    return 0;
}

static int veloxfs_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct inode              *inode   = d_inode(dentry);
    struct veloxfs_sb_info    *sbi     = VELOXFS_SB(inode->i_sb);
    struct veloxfs_inode_info *vi_info = VELOXFS_I(inode);
    char dir_path[VELOXFS_MAX_PATH];
    __u64 vlx_ino = vi_info->vi.inode_num, i;
    int   err;

    mutex_lock(&sbi->lock);
    dir_path[0] = '\0';
    for (i = 0; i < sbi->num_dirents; i++) {
        if (sbi->directory[i].inode_num == vlx_ino) {
            strncpy(dir_path, sbi->directory[i].path, sizeof(dir_path) - 1);
            dir_path[sizeof(dir_path) - 1] = '\0'; break;
        }
    }
    for (i = 0; i < sbi->num_dirents; i++) {
        if (!sbi->directory[i].inode_num) continue;
        if (vlx_is_direct_child(dir_path, sbi->directory[i].path)) {
            mutex_unlock(&sbi->lock); return -ENOTEMPTY;
        }
    }
    mutex_unlock(&sbi->lock);

    err = veloxfs_unlink(dir, dentry);
    if (!err) drop_nlink(dir);
    return err;
}

static int veloxfs_vfs_rename(VLX_NS *mnt_ns,
                           struct inode *old_dir, struct dentry *old_dentry,
                           struct inode *new_dir, struct dentry *new_dentry,
                           unsigned int flags)
{
    struct veloxfs_sb_info    *sbi     = VELOXFS_SB(old_dir->i_sb);
    struct veloxfs_inode_info *vi_info = VELOXFS_I(d_inode(old_dentry));
    char  new_dir_path[VELOXFS_MAX_PATH], new_full[VELOXFS_MAX_PATH];
    __u64 vlx_ino = vi_info->vi.inode_num, i;

    if (flags & ~RENAME_NOREPLACE) return -EINVAL;

    mutex_lock(&sbi->lock);
    if (vlx_dir_path(sbi, new_dir, new_dir_path, sizeof(new_dir_path)) < 0) {
        mutex_unlock(&sbi->lock); return -ENOENT;
    }
    if (!strcmp(new_dir_path, "/"))
        snprintf(new_full, sizeof(new_full), "/%.*s",
                 (int)new_dentry->d_name.len, new_dentry->d_name.name);
    else
        snprintf(new_full, sizeof(new_full), "%s/%.*s",
                 new_dir_path, (int)new_dentry->d_name.len, new_dentry->d_name.name);

    if ((flags & RENAME_NOREPLACE) && vlx_find_dirent(sbi, new_full)) {
        mutex_unlock(&sbi->lock); return -EEXIST;
    }
    {
        veloxfs_dirent *existing = vlx_find_dirent(sbi, new_full);
        if (existing) {
            __u64 old_ino = existing->inode_num;
            vlx_inode_free_all(sbi, &sbi->inodes[old_ino - 1]);
            memset(&sbi->inodes[old_ino - 1], 0, sizeof(veloxfs_inode));
            memset(existing, 0, sizeof(*existing));
            sbi->dirty_inodes = 1;
        }
    }
    for (i = 0; i < sbi->num_dirents; i++) {
        if (sbi->directory[i].inode_num == vlx_ino) {
            strncpy(sbi->directory[i].path, new_full, VELOXFS_MAX_PATH - 1);
            sbi->directory[i].path[VELOXFS_MAX_PATH - 1] = '\0';
            sbi->dirty_dir = 1; break;
        }
    }
    mutex_unlock(&sbi->lock);
    return 0;
}

static const struct inode_operations veloxfs_dir_inode_ops = {
    .lookup  = veloxfs_lookup,
    .create  = veloxfs_vfs_create,
    .mkdir   = veloxfs_vfs_mkdir,
    .unlink  = veloxfs_unlink,
    .rmdir   = veloxfs_rmdir,
    .rename  = veloxfs_vfs_rename,
    .getattr = veloxfs_getattr,
    .setattr = veloxfs_setattr,
};

static const struct file_operations veloxfs_dir_ops = {
    .iterate_shared = veloxfs_iterate,
    .llseek         = generic_file_llseek,
    .fsync          = veloxfs_fsync,
};

/* =========================================================================
 * Super block operations
 * ====================================================================== */
static int veloxfs_write_inode(struct inode *inode,
                                struct writeback_control *wbc)
{
    struct veloxfs_sb_info    *sbi     = VELOXFS_SB(inode->i_sb);
    struct veloxfs_inode_info *vi_info = VELOXFS_I(inode);
    __u64 vlx_ino;

    if (inode->i_ino == VELOXFS_ROOT_INO) return 0;

    vlx_ino = VFS_TO_VLX_INO(inode->i_ino);
    if (!vlx_ino || vlx_ino > sbi->num_inodes) return -EIO;

    mutex_lock(&sbi->lock);
    /* Write VFS fields into veloxfs_inode (field names from veloxfs.h) */
    vi_info->vi.mode  = (__u32)inode->i_mode;
    vi_info->vi.uid   = (__u32)i_uid_read(inode);
    vi_info->vi.gid   = (__u32)i_gid_read(inode);
    vi_info->vi.size  = (__u64)inode->i_size;
    vi_info->vi.atime = (__u64)vlx_inode_atime(inode).tv_sec;
    vi_info->vi.mtime = (__u64)vlx_inode_mtime(inode).tv_sec;
    vi_info->vi.ctime = (__u64)vlx_inode_ctime(inode).tv_sec;
    sbi->inodes[vlx_ino - 1] = vi_info->vi;
    sbi->dirty_inodes = 1;
    if (wbc->sync_mode == WB_SYNC_ALL) {
        vlx_flush_inodes(inode->i_sb);
        vlx_flush_fat(inode->i_sb);
        vlx_flush_dir(inode->i_sb);
    }
    mutex_unlock(&sbi->lock);
    return 0;
}

static void veloxfs_evict_inode(struct inode *inode)
{
    struct veloxfs_sb_info    *sbi     = VELOXFS_SB(inode->i_sb);
    struct veloxfs_inode_info *vi_info = VELOXFS_I(inode);

    truncate_inode_pages_final(&inode->i_data);

    if (!inode->i_nlink && inode->i_ino != VELOXFS_ROOT_INO) {
        __u64 vlx_ino = vi_info->vi.inode_num;
        if (vlx_ino && vlx_ino <= sbi->num_inodes) {
            mutex_lock(&sbi->lock);
            vlx_inode_free_all(sbi, &sbi->inodes[vlx_ino - 1]);
            memset(&sbi->inodes[vlx_ino - 1], 0, sizeof(veloxfs_inode));
            sbi->dirty_inodes = 1;
            mutex_unlock(&sbi->lock);
        }
    }
    invalidate_inode_buffers(inode);
    clear_inode(inode);
}

static void veloxfs_put_super(struct super_block *sb)
{
    struct veloxfs_sb_info *sbi = VELOXFS_SB(sb);
    if (!sbi) return;
    mutex_lock(&sbi->lock);
    vlx_flush_fat(sb); vlx_flush_inodes(sb); vlx_flush_dir(sb);
    mutex_unlock(&sbi->lock);
    vfree(sbi->fat); vfree(sbi->inodes); vfree(sbi->directory);
    kfree(sbi); sb->s_fs_info = NULL;
}

static int veloxfs_vfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    struct veloxfs_sb_info *sbi = VELOXFS_SB(dentry->d_sb);
    __u64 used = 0, i, data_blks, free_blks;

    mutex_lock(&sbi->lock);
    for (i = sbi->super.data_start; i < sbi->super.block_count; i++)
        if (sbi->fat[i] != VELOXFS_FAT_FREE) used++;
    mutex_unlock(&sbi->lock);

    data_blks = sbi->super.block_count - sbi->super.data_start;
    free_blks  = data_blks - used;
    buf->f_type    = VELOXFS_MAGIC;
    buf->f_bsize   = sbi->super.block_size;
    buf->f_blocks  = data_blks;
    buf->f_bfree   = free_blks;
    buf->f_bavail  = free_blks;
    buf->f_files   = sbi->num_inodes;
    buf->f_ffree   = 0;
    buf->f_namelen = VELOXFS_MAX_PATH - 1;
    return 0;
}

static const struct super_operations veloxfs_sops = {
    .alloc_inode = veloxfs_alloc_inode,
    .free_inode  = veloxfs_free_inode,
    .write_inode = veloxfs_write_inode,
    .evict_inode = veloxfs_evict_inode,
    .put_super   = veloxfs_put_super,
    .statfs      = veloxfs_vfs_statfs,
};

/* =========================================================================
 * fill_super — shared between both mount API paths
 *
 * Reads veloxfs_superblock from block 0 (the struct is defined in veloxfs.h),
 * validates it, loads the FAT, inode table, and dirent table.
 * ====================================================================== */
static int veloxfs_fill_super_impl(struct super_block *sb, int silent)
{
    struct veloxfs_sb_info *sbi = NULL;
    struct buffer_head     *bh;
    veloxfs_superblock     *disk_super;
    struct inode           *root_inode;
    struct dentry          *root_dentry;
    u32   vlx_bs, vfs_bs;
    __u64 fat_bytes, inode_bytes, dir_bytes, ratio, i;
    int   err = -EINVAL;

    sb_set_blocksize(sb, 1024);
    bh = sb_bread(sb, 0);
    if (!bh) {
        if (!silent) pr_err("veloxfs: cannot read superblock\n");
        return -EIO;
    }

    disk_super = (veloxfs_superblock *)bh->b_data;

    if (disk_super->magic != VELOXFS_MAGIC_V6) {
        if (!silent)
            pr_err("veloxfs: bad magic 0x%08x (expected 0x%08lx)\n",
                   disk_super->magic, (unsigned long)VELOXFS_MAGIC_V6);
        brelse(bh); return -EINVAL;
    }

    vlx_bs = disk_super->block_size;
    if (vlx_bs < VELOXFS_BS_MIN || vlx_bs > VELOXFS_BS_MAX ||
        (vlx_bs & (vlx_bs - 1))) {
        if (!silent) pr_err("veloxfs: invalid block_size %u\n", vlx_bs);
        brelse(bh); return -EINVAL;
    }

    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi) { brelse(bh); return -ENOMEM; }

    /* sbi->super is veloxfs_superblock from veloxfs.h */
    memcpy(&sbi->super, disk_super, sizeof(sbi->super));
    brelse(bh);

    mutex_init(&sbi->lock);
    sbi->last_alloc_hint = sbi->super.data_start;

    vfs_bs = min_t(u32, vlx_bs, (u32)PAGE_SIZE);
    if (!sb_set_blocksize(sb, (int)vfs_bs)) {
        pr_err("veloxfs: device cannot support block size %u\n", vfs_bs);
        err = -EINVAL; goto err_sbi;
    }

    sb->s_magic    = VELOXFS_MAGIC;
    sb->s_op       = &veloxfs_sops;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_fs_info  = sbi;
    ratio          = vlx_bs / vfs_bs;

    /* Load FAT */
    fat_bytes = sbi->super.fat_blocks * vlx_bs; // Allocate full blocks
    sbi->fat  = vmalloc(fat_bytes);
    //if (!sbi->fat) { ... }
    if (!sbi->fat) { err = -ENOMEM; goto err_sbi; }

    for (i = 0; i < sbi->super.fat_blocks; i++) {
        __u64 j; u8 *dst = (u8 *)sbi->fat + i * vlx_bs;
        for (j = 0; j < ratio; j++) {
            sector_t phys = (sector_t)((sbi->super.fat_start + i) * ratio + j);
            bh = sb_bread(sb, phys);
            if (!bh) { err = -EIO; goto err_fat; }
            memcpy(dst + j * vfs_bs, bh->b_data, vfs_bs); brelse(bh);
        }
    }

    /* Load inode table (veloxfs_inode from veloxfs.h) */
    inode_bytes     = sbi->super.inode_blocks * (__u64)vlx_bs;
    sbi->inodes     = vmalloc(inode_bytes);
    sbi->num_inodes = inode_bytes / sizeof(veloxfs_inode);
    if (!sbi->inodes) { err = -ENOMEM; goto err_fat; }

    for (i = 0; i < sbi->super.inode_blocks; i++) {
        __u64 j; u8 *dst = (u8 *)sbi->inodes + i * vlx_bs;
        for (j = 0; j < ratio; j++) {
            sector_t phys = (sector_t)((sbi->super.inode_start + i) * ratio + j);
            bh = sb_bread(sb, phys);
            if (!bh) { err = -EIO; goto err_inodes; }
            memcpy(dst + j * vfs_bs, bh->b_data, vfs_bs); brelse(bh);
        }
    }

    /* Load dirent table (veloxfs_dirent from veloxfs.h) */
    sbi->dir_start_blk = sbi->super.inode_start + sbi->super.inode_blocks;
    sbi->dir_blocks    = vlx_calc_dir_blocks(sbi->super.block_count);
    dir_bytes          = sbi->dir_blocks * (__u64)vlx_bs;
    sbi->directory     = vmalloc(dir_bytes);
    sbi->num_dirents   = dir_bytes / sizeof(veloxfs_dirent);
    if (!sbi->directory) { err = -ENOMEM; goto err_inodes; }

    for (i = 0; i < sbi->dir_blocks; i++) {
        __u64 j; u8 *dst = (u8 *)sbi->directory + i * vlx_bs;
        for (j = 0; j < ratio; j++) {
            sector_t phys = (sector_t)((sbi->dir_start_blk + i) * ratio + j);
            bh = sb_bread(sb, phys);
            if (!bh) { err = -EIO; goto err_dir; }
            memcpy(dst + j * vfs_bs, bh->b_data, vfs_bs); brelse(bh);
        }
    }

    /* Synthetic root inode */
    root_inode = new_inode(sb);
    if (!root_inode) { err = -ENOMEM; goto err_dir; }
    root_inode->i_ino  = VELOXFS_ROOT_INO;
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_op   = &veloxfs_dir_inode_ops;
    root_inode->i_fop  = &veloxfs_dir_ops;
    veloxfs_set_inode_times(root_inode, current_time(root_inode));
    set_nlink(root_inode, 2);

    root_dentry = d_make_root(root_inode);
    if (!root_dentry) { err = -ENOMEM; goto err_dir; }
    sb->s_root = root_dentry;

    pr_info("veloxfs: mounted — vlx_bs=%u vfs_bs=%u blocks=%llu "
            "inodes=%llu dirents=%llu (veloxfs.h v%d on-disk layout)\n",
            vlx_bs, vfs_bs,
            (unsigned long long)sbi->super.block_count,
            (unsigned long long)sbi->num_inodes,
            (unsigned long long)sbi->num_dirents,
            VELOXFS_VERSION);
    return 0;

err_dir:    vfree(sbi->directory);
err_inodes: vfree(sbi->inodes);
err_fat:    vfree(sbi->fat);
err_sbi:    kfree(sbi); sb->s_fs_info = NULL;
    return err;
}

/* =========================================================================
 * Mount API  (two paths: < 7.0 = mount_bdev,  7.0+ = init_fs_context)
 * ====================================================================== */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)

static int veloxfs_fill_super_fc(struct super_block *sb, struct fs_context *fc)
{
    return veloxfs_fill_super_impl(sb, fc->sb_flags & SB_SILENT ? 1 : 0);
}

static int veloxfs_get_tree(struct fs_context *fc)
{
    return get_tree_bdev(fc, veloxfs_fill_super_fc);
}

static const struct fs_context_operations veloxfs_context_ops = {
    .get_tree = veloxfs_get_tree,
};

static int veloxfs_init_fs_context(struct fs_context *fc)
{
    fc->ops = &veloxfs_context_ops;
    return 0;
}

#else

static int veloxfs_fill_super_legacy(struct super_block *sb, void *data,
                                      int silent)
{
    return veloxfs_fill_super_impl(sb, silent);
}

static struct dentry *veloxfs_mount(struct file_system_type *fs_type,
                                     int flags, const char *dev_name, void *data)
{
    return mount_bdev(fs_type, flags, dev_name, data, veloxfs_fill_super_legacy);
}

#endif

static struct file_system_type veloxfs_fs_type = {
    .owner    = THIS_MODULE,
    .name     = "veloxfs",
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
    .init_fs_context = veloxfs_init_fs_context,
#else
    .mount    = veloxfs_mount,
#endif
    .kill_sb  = kill_block_super,
    .fs_flags = FS_REQUIRES_DEV,
};

/* =========================================================================
 * Module lifecycle
 * ====================================================================== */
static int __init init_veloxfs_fs(void)
{
    int err;

    veloxfs_inode_cachep =
        kmem_cache_create("veloxfs_inode_cache",
                          sizeof(struct veloxfs_inode_info), 0,
                          SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
                          veloxfs_init_once);
    if (!veloxfs_inode_cachep)
        return -ENOMEM;

    err = register_filesystem(&veloxfs_fs_type);
    if (err) {
        kmem_cache_destroy(veloxfs_inode_cachep);
        return err;
    }

    pr_info("VeloxFS v6: driver loaded (kernel %d.%d) — "
            "on-disk types from veloxfs.h\n",
            LINUX_VERSION_MAJOR, LINUX_VERSION_PATCHLEVEL);
    return 0;
}

static void __exit exit_veloxfs_fs(void)
{
    unregister_filesystem(&veloxfs_fs_type);
    rcu_barrier();
    kmem_cache_destroy(veloxfs_inode_cachep);
    pr_info("VeloxFS v6: driver unloaded\n");
}

module_init(init_veloxfs_fs);
module_exit(exit_veloxfs_fs);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Maxwell Wingate");
MODULE_DESCRIPTION("VeloxFS v6 Storage-Optimised Linux Kernel Driver");
MODULE_ALIAS_FS("veloxfs");
