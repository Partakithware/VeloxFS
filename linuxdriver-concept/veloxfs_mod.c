// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * veloxfs_mod.c  –  VeloxFS v6 Linux Kernel Driver
 *
 * Author : Maxwell Wingate
 * Target : Linux 5.15 LTS through 7.0+ (GCC / Clang, x86-64 or ARM64)
 * Tested and working on 7.0+ (None tested below)
 * Compatibility matrix (handled via version guards below):
 *   5.15 – 6.3  : user_namespace, i_atime direct access, mount_bdev, writepage
 *   6.4         : filemap_splice_read replaces generic_file_splice_read
 *   6.6         : inode timestamp accessors (inode_set_atime_to_ts etc)
 *   6.11        : writepage removed → writepages; block_write_full_folio;
 *                 write_begin/end take folio; generic_fillattr gets request_mask
 *   7.0         : mount_bdev removed → init_fs_context + get_tree_bdev
 *
 * VeloxFS on-disk layout (all little-endian, native struct packing):
 *
 *   Block 0          : superblock  (veloxfs_superblock, 80 bytes)
 *   fat_start ..     : FAT table   (array of u64, one entry per block)
 *   journal_start .. : journal     (veloxfs_journal_entry × 64, optional)
 *   inode_start ..   : inode table (veloxfs_inode × N, 144 bytes each)
 *   dir_start ..     : dirent table(veloxfs_dirent × M, 504 bytes each)
 *                      dir_start = inode_start + inode_blocks   (NOT in superblock)
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

/* =========================================================================
 * Kernel 7.0+: new mount API requires fs_context.h and mount.h
 * ====================================================================== */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#  include <linux/fs_context.h>
#  include <linux/mount.h>
#endif

/* =========================================================================
 * Compatibility shims — ordered by introduction version
 * ====================================================================== */

/* --- 6.3+: mnt_idmap replaces user_namespace in ownership/attr calls --- */
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

/* --- alloc_inode_sb() added in 5.18 --- */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
#  define alloc_inode_sb(sb, cache, gfp) kmem_cache_alloc((cache), (gfp))
#endif

/* --- 6.4+: filemap_splice_read replaced generic_file_splice_read --- */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#  define VLX_SPLICE_READ  filemap_splice_read
#else
#  define VLX_SPLICE_READ  generic_file_splice_read
#endif

/* --- 6.6+: inode timestamp accessors (i_atime/mtime/ctime moved) --- */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#  define vlx_inode_atime(inode)  inode_get_atime(inode)
#  define vlx_inode_mtime(inode)  inode_get_mtime(inode)
#  define vlx_inode_ctime(inode)  inode_get_ctime(inode)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#  define VLX_INODE_IS_NEW(inode) ((inode)->i_state.__state & I_NEW)
#else
#  define VLX_INODE_IS_NEW(inode) ((inode)->i_state & I_NEW)
#endif

static inline void veloxfs_set_inode_times(struct inode *inode,
                                            struct timespec64 ts)
{
    inode_set_atime_to_ts(inode, ts);
    inode_set_mtime_to_ts(inode, ts);
    inode_set_ctime_to_ts(inode, ts);
}
static inline void veloxfs_set_inode_ctime(struct inode *inode,
                                            struct timespec64 ts)
{
    inode_set_ctime_to_ts(inode, ts);
}
static inline void veloxfs_set_inode_mtime(struct inode *inode,
                                            struct timespec64 ts)
{
    inode_set_mtime_to_ts(inode, ts);
}
#else
#  define vlx_inode_atime(inode)  ((inode)->i_atime)
#  define vlx_inode_mtime(inode)  ((inode)->i_mtime)
#  define vlx_inode_ctime(inode)  ((inode)->i_ctime)

static inline void veloxfs_set_inode_times(struct inode *inode,
                                            struct timespec64 ts)
{
    inode->i_atime = inode->i_mtime = inode->i_ctime = ts;
}
static inline void veloxfs_set_inode_ctime(struct inode *inode,
                                            struct timespec64 ts)
{
    inode->i_ctime = ts;
}
static inline void veloxfs_set_inode_mtime(struct inode *inode,
                                            struct timespec64 ts)
{
    inode->i_mtime = ts;
}
#endif

/* --- 6.11+: generic_fillattr requires request_mask argument --- */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
#  define VLX_FILLATTR(idmap, inode, stat) \
        generic_fillattr((idmap), STATX_BASIC_STATS, (inode), (stat))
#else
#  define VLX_FILLATTR(ns, inode, stat) \
        generic_fillattr((ns), (inode), (stat))
#endif

/* =========================================================================
 * On-disk structure definitions
 * ====================================================================== */

#define VELOXFS_MAGIC_V6        0x564C5836UL
#define VELOXFS_MAGIC           VELOXFS_MAGIC_V6
#define VELOXFS_VERSION         6

#define VELOXFS_BS_MIN          4096
#define VELOXFS_BS_MAX          (1U << 20)

#define VELOXFS_INLINE_EXTENTS  4

#define VELOXFS_INODE_F_EXTENTS 0x01
#define VELOXFS_INODE_F_DIR     0x02

#define VELOXFS_MAX_PATH        480
#define VELOXFS_JOURNAL_SIZE    64

#define VELOXFS_FAT_FREE  0x0000000000000000ULL
#define VELOXFS_FAT_EOF   0xFFFFFFFFFFFFFFFFULL
#define VELOXFS_FAT_BAD   0xFFFFFFFFFFFFFFFEULL

struct veloxfs_superblock {
    __u32 magic;
    __u32 version;
    __u32 block_size;
    __u32 journal_enabled;
    __u32 storage_hint;
    __u32 _pad0;
    __u64 block_count;
    __u64 fat_start;
    __u64 fat_blocks;
    __u64 journal_start;
    __u64 journal_blocks;
    __u64 inode_start;
    __u64 inode_blocks;
    __u64 data_start;
    __u64 reserved[4];
} __attribute__((packed));

struct veloxfs_extent {
    __u64 start_block;
    __u64 block_count;
} __attribute__((packed));

struct veloxfs_disk_inode {
    __u64 inode_num;
    __u64 size;
    __u32 uid;
    __u32 gid;
    __u32 mode;
    __u32 inode_flags;
    __u64 ctime;
    __u64 mtime;
    __u64 atime;
    __u64 fat_head;
    struct veloxfs_extent extents[VELOXFS_INLINE_EXTENTS];
    __u64 extent_count;
    __u64 _ipad;
} __attribute__((packed));

static_assert(sizeof(struct veloxfs_disk_inode) == 144,
              "veloxfs_disk_inode must be 144 bytes");

struct veloxfs_disk_dirent {
    char  path[VELOXFS_MAX_PATH];
    __u64 inode_num;
    __u64 reserved[2];
} __attribute__((packed));

/* =========================================================================
 * VFS inode number mapping
 * ====================================================================== */
#define VELOXFS_ROOT_INO    1UL
#define VLX_TO_VFS_INO(n)  ((unsigned long)((n) + 1))
#define VFS_TO_VLX_INO(i)  ((__u64)((i) - 1))

/* =========================================================================
 * In-memory superblock private data  (sb->s_fs_info)
 * ====================================================================== */
struct veloxfs_sb_info {
    struct veloxfs_superblock  super;
    __u64                     *fat;
    struct veloxfs_disk_inode *inodes;
    struct veloxfs_disk_dirent*directory;
    __u64                      num_inodes;
    __u64                      num_dirents;
    __u64                      dir_start_blk;
    __u64                      dir_blocks;
    __u64                      last_alloc_hint;
    struct mutex               lock;
    int                        dirty_fat;
    int                        dirty_inodes;
    int                        dirty_dir;
};

/* =========================================================================
 * In-memory inode info
 * ====================================================================== */
struct veloxfs_inode_info {
    struct veloxfs_disk_inode  vi;
    struct inode               vfs_inode;
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
 * Layout helpers
 * ====================================================================== */
static __u64 vlx_calc_dir_blocks(__u64 block_count)
{
    __u64 n = block_count / 100;
    return n ? n : 1;
}

/* =========================================================================
 * Block flush helper
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
        sector_t phys   = (sector_t)(start_vlx_block * ratio + j);
        size_t   offset = (size_t)(j * (__u64)sb->s_blocksize);
        size_t   copy_len = min_t(size_t, (size_t)sb->s_blocksize,
                                  total_bytes - offset);

        bh = sb_getblk(sb, phys);
        if (!bh)
            return -EIO;

        lock_buffer(bh);
        memset(bh->b_data, 0, sb->s_blocksize);
        memcpy(bh->b_data, (const u8 *)data + offset, copy_len);
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
    size_t fat_bytes = (size_t)(sbi->super.block_count * sizeof(__u64));

    if (!sbi->dirty_fat)
        return 0;
    if (vlx_flush_region(sb, sbi->fat, fat_bytes, sbi->super.fat_start) < 0)
        return -EIO;
    sbi->dirty_fat = 0;
    return 0;
}

static int vlx_flush_inodes(struct super_block *sb)
{
    struct veloxfs_sb_info *sbi = VELOXFS_SB(sb);
    size_t inode_bytes = (size_t)(sbi->num_inodes *
                                  sizeof(struct veloxfs_disk_inode));
    if (!sbi->dirty_inodes)
        return 0;
    if (vlx_flush_region(sb, sbi->inodes, inode_bytes,
                          sbi->super.inode_start) < 0)
        return -EIO;
    sbi->dirty_inodes = 0;
    return 0;
}

static int vlx_flush_dir(struct super_block *sb)
{
    struct veloxfs_sb_info *sbi = VELOXFS_SB(sb);
    size_t dir_bytes = (size_t)(sbi->num_dirents *
                                sizeof(struct veloxfs_disk_dirent));
    if (!sbi->dirty_dir)
        return 0;
    if (vlx_flush_region(sb, sbi->directory, dir_bytes,
                          sbi->dir_start_blk) < 0)
        return -EIO;
    sbi->dirty_dir = 0;
    return 0;
}

/* =========================================================================
 * In-memory FAT operations
 * ====================================================================== */
static __u64 vlx_inode_total_blocks(struct veloxfs_sb_info *sbi,
                                     struct veloxfs_disk_inode *vi)
{
    __u64 total = 0, i, b, safety = 0;

    if (vi->inode_flags & VELOXFS_INODE_F_EXTENTS) {
        for (i = 0; i < vi->extent_count; i++)
            total += vi->extents[i].block_count;
    }
    b = vi->fat_head;
    while (b != 0 && b < sbi->super.block_count) {
        __u64 next = sbi->fat[b];
        total++;
        if (next == VELOXFS_FAT_EOF || next == VELOXFS_FAT_FREE ||
            next == VELOXFS_FAT_BAD)
            break;
        b = next;
        if (++safety > sbi->super.block_count)
            break;
    }
    return total;
}

static __u64 vlx_inode_get_phys(struct veloxfs_sb_info *sbi,
                                  struct veloxfs_disk_inode *vi, __u64 n)
{
    if (vi->inode_flags & VELOXFS_INODE_F_EXTENTS) {
        __u64 skipped = 0, i;

        for (i = 0; i < vi->extent_count; i++) {
            struct veloxfs_extent *e = &vi->extents[i];
            if (n < skipped + e->block_count)
                return e->start_block + (n - skipped);
            skipped += e->block_count;
        }
        if (vi->fat_head != 0) {
            __u64 b = vi->fat_head;
            __u64 rel = n - skipped, safety = 0;
            for (; rel > 0 && b != 0 && b < sbi->super.block_count; rel--) {
                __u64 next = sbi->fat[b];
                if (next == VELOXFS_FAT_EOF || next == VELOXFS_FAT_FREE ||
                    next == VELOXFS_FAT_BAD)
                    return 0;
                b = next;
                if (++safety > sbi->super.block_count)
                    return 0;
            }
            return (rel == 0) ? b : 0;
        }
        return 0;
    }
    {
        __u64 b = vi->fat_head, safety = 0;
        for (; n > 0 && b != 0 && b < sbi->super.block_count; n--) {
            __u64 next = sbi->fat[b];
            if (next == VELOXFS_FAT_EOF || next == VELOXFS_FAT_FREE ||
                next == VELOXFS_FAT_BAD)
                return 0;
            b = next;
            if (++safety > sbi->super.block_count)
                return 0;
        }
        return b;
    }
}

static __u64 vlx_alloc_contiguous(struct veloxfs_sb_info *sbi,
                                    __u64 wanted, __u64 *out_start)
{
    __u64 data_start  = sbi->super.data_start;
    __u64 block_count = sbi->super.block_count;
    __u64 best_start = 0, best_len = 0;
    __u64 run_start = 0, run_len = 0;
    __u64 hint = sbi->last_alloc_hint;
    int pass;

    if (hint < data_start)
        hint = data_start;

    for (pass = 0; pass < 2 && best_len < wanted; pass++) {
        __u64 scan_start = (pass == 0) ? hint        : data_start;
        __u64 scan_end   = (pass == 0) ? block_count : hint;
        __u64 i;

        run_len = 0;
        for (i = scan_start; i < scan_end; i++) {
            if (sbi->fat[i] == VELOXFS_FAT_FREE) {
                if (run_len == 0)
                    run_start = i;
                run_len++;
                if (run_len >= wanted) {
                    best_start = run_start;
                    best_len   = wanted;
                    goto found;
                }
            } else {
                if (run_len > best_len) {
                    best_len   = run_len;
                    best_start = run_start;
                }
                run_len = 0;
            }
        }
        if (run_len > best_len) {
            best_len   = run_len;
            best_start = run_start;
        }
    }

found:
    if (best_len == 0) {
        *out_start = 0;
        return 0;
    }
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
                              struct veloxfs_disk_inode *vi, __u64 extra_blocks)
{
    __u64 remaining = extra_blocks;

    if ((vi->inode_flags & VELOXFS_INODE_F_EXTENTS) && vi->extent_count > 0) {
        struct veloxfs_extent *last = &vi->extents[vi->extent_count - 1];
        __u64 next_phys = last->start_block + last->block_count;
        __u64 grown = 0;

        while (grown < remaining &&
               next_phys + grown < sbi->super.block_count &&
               sbi->fat[next_phys + grown] == VELOXFS_FAT_FREE) {
            sbi->fat[next_phys + grown] = VELOXFS_FAT_EOF;
            grown++;
        }
        if (grown > 0) {
            last->block_count += grown;
            remaining         -= grown;
            sbi->dirty_fat    = 1;
            sbi->dirty_inodes = 1;
        }
    }

    while (remaining > 0) {
        __u64 start = 0;
        __u64 got = vlx_alloc_contiguous(sbi, remaining, &start);
        if (got == 0)
            return -ENOSPC;

        if ((vi->inode_flags & VELOXFS_INODE_F_EXTENTS) &&
             vi->extent_count < VELOXFS_INLINE_EXTENTS) {
            vi->extents[vi->extent_count].start_block = start;
            vi->extents[vi->extent_count].block_count = got;
            vi->extent_count++;
            sbi->dirty_inodes = 1;
        } else {
            __u64 i;
            for (i = 0; i + 1 < got; i++)
                sbi->fat[start + i] = start + i + 1;
            sbi->fat[start + got - 1] = VELOXFS_FAT_EOF;

            if (vi->fat_head == 0) {
                vi->fat_head = start;
            } else {
                __u64 tail = vi->fat_head;
                while (sbi->fat[tail] != VELOXFS_FAT_EOF &&
                       sbi->fat[tail] != VELOXFS_FAT_FREE &&
                       sbi->fat[tail] < sbi->super.block_count)
                    tail = sbi->fat[tail];
                sbi->fat[tail] = start;
            }
            sbi->dirty_fat    = 1;
            sbi->dirty_inodes = 1;
        }
        remaining -= got;
    }
    return 0;
}

static void vlx_inode_free_all(struct veloxfs_sb_info *sbi,
                                struct veloxfs_disk_inode *vi)
{
    __u64 i, b, safety = 0;

    if (vi->inode_flags & VELOXFS_INODE_F_EXTENTS) {
        for (i = 0; i < vi->extent_count; i++) {
            struct veloxfs_extent *e = &vi->extents[i];
            __u64 b2;
            for (b2 = 0; b2 < e->block_count; b2++) {
                __u64 blk = e->start_block + b2;
                if (blk < sbi->super.block_count)
                    sbi->fat[blk] = VELOXFS_FAT_FREE;
            }
        }
        vi->extent_count = 0;
        sbi->dirty_fat = 1;
    }

    b = vi->fat_head;
    while (b != 0 && b < sbi->super.block_count) {
        __u64 next = sbi->fat[b];
        sbi->fat[b] = VELOXFS_FAT_FREE;
        sbi->dirty_fat = 1;
        if (next == VELOXFS_FAT_EOF || next == VELOXFS_FAT_FREE ||
            next == VELOXFS_FAT_BAD)
            break;
        b = next;
        if (++safety > sbi->super.block_count)
            break;
    }
    vi->fat_head      = 0;
    sbi->dirty_inodes = 1;
}

/* =========================================================================
 * Directory table helpers
 * ====================================================================== */
static bool vlx_is_direct_child(const char *parent, const char *path)
{
    size_t plen = strlen(parent);

    if (strcmp(parent, "/") == 0)
        return path[0] == '/' && path[1] != '\0' &&
               strchr(path + 1, '/') == NULL;
    if (strncmp(path, parent, plen) != 0 || path[plen] != '/')
        return false;
    return strchr(path + plen + 1, '/') == NULL;
}

static struct veloxfs_disk_dirent *vlx_find_dirent(struct veloxfs_sb_info *sbi,
                                                     const char *path)
{
    __u64 i;
    for (i = 0; i < sbi->num_dirents; i++) {
        if (sbi->directory[i].inode_num != 0 &&
            strcmp(sbi->directory[i].path, path) == 0)
            return &sbi->directory[i];
    }
    return NULL;
}

static struct veloxfs_disk_inode *vlx_get_disk_inode(struct veloxfs_sb_info *sbi,
                                                       __u64 inode_num)
{
    if (inode_num == 0 || inode_num > sbi->num_inodes)
        return NULL;
    if (sbi->inodes[inode_num - 1].inode_num == 0)
        return NULL;
    return &sbi->inodes[inode_num - 1];
}

/* =========================================================================
 * VFS inode cache
 * ====================================================================== */
static struct inode *veloxfs_alloc_inode(struct super_block *sb)
{
    struct veloxfs_inode_info *vi;
    vi = alloc_inode_sb(sb, veloxfs_inode_cachep, GFP_KERNEL);
    if (!vi)
        return NULL;
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
 * veloxfs_fill_inode — copy disk inode fields into VFS inode.
 *
 * Timestamp handling: kernels 6.6+ moved i_atime/mtime/ctime into an
 * internal struct; we must use inode_set_*_to_ts() instead of direct
 * struct member writes.
 */
static void veloxfs_fill_inode(struct inode *inode,
                                const struct veloxfs_disk_inode *disk)
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

    /* Use version-agnostic accessors for timestamps */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    inode_set_atime_to_ts(inode, atime);
    inode_set_mtime_to_ts(inode, mtime);
    inode_set_ctime_to_ts(inode, ctime);
#else
    inode->i_atime = atime;
    inode->i_mtime = mtime;
    inode->i_ctime = ctime;
#endif

    inode->i_blocks = vlx_inode_total_blocks(sbi,
                        (struct veloxfs_disk_inode *)disk) *
                      (sbi->super.block_size >> 9);
}

/* =========================================================================
 * veloxfs_iget
 * ====================================================================== */
static struct inode *veloxfs_iget(struct super_block *sb, __u64 vlx_ino)
{
    struct veloxfs_sb_info    *sbi  = VELOXFS_SB(sb);
    struct inode              *inode;
    struct veloxfs_disk_inode *disk_vi;

    inode = iget_locked(sb, VLX_TO_VFS_INO(vlx_ino));
    if (!inode)
        return ERR_PTR(-ENOMEM);
    if (!VLX_INODE_IS_NEW(inode))
        return inode;

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
 * get_block: maps VFS logical block → physical device sector
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
    sector_t phys_vfs;
    int err = 0;

    mutex_lock(&sbi->lock);

    phys_vlx = vlx_inode_get_phys(sbi, &vi_info->vi, vlx_blk);

    if (phys_vlx == 0) {
        if (!create)
            goto out;
        {
            __u64 have = vlx_inode_total_blocks(sbi, &vi_info->vi);
            __u64 need = vlx_blk + 1;
            if (need > have) {
                err = vlx_inode_extend(sbi, &vi_info->vi, need - have);
                if (err)
                    goto out;
                sbi->inodes[vi_info->vi.inode_num - 1] = vi_info->vi;
            }
        }
        phys_vlx = vlx_inode_get_phys(sbi, &vi_info->vi, vlx_blk);
        if (phys_vlx == 0) {
            err = -EIO;
            goto out;
        }
        set_buffer_new(bh_result);
    }

    phys_vfs = (sector_t)(phys_vlx * ratio + off_in);
    map_bh(bh_result, inode->i_sb, phys_vfs);

out:
    mutex_unlock(&sbi->lock);
    return err;
}

/* =========================================================================
 * Address space operations
 *
 * Kernel 6.11 removed .writepage and block_write_full_page.
 * We use .writepages + mpage_writepages for 6.11+, the classic
 * .writepage + block_write_full_page for older kernels.
 *
 * write_begin/write_end: kernel 6.11 switched struct page ** → struct folio **.
 * ====================================================================== */

static int veloxfs_read_folio(struct file *file, struct folio *folio)
{
    return mpage_read_folio(folio, veloxfs_get_block);
}

/* --- Page Writeback Operations --- */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
/* 6.11+ and 7.0+: writepages replaces writepage */
static int veloxfs_writepages(struct address_space *mapping,
                               struct writeback_control *wbc)
{
    return mpage_writepages(mapping, wbc, veloxfs_get_block);
}
#else /* < 6.11 */
static int veloxfs_writepage(struct page *page, struct writeback_control *wbc)
{
    return block_write_full_page(page, veloxfs_get_block, wbc);
}
#endif

/* --- Write Begin / End Operations --- */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)

/* 7.0+: write_begin/end use struct kiocb instead of struct file */
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
    struct inode              *inode   = mapping->host;
    struct veloxfs_sb_info    *sbi     = VELOXFS_SB(inode->i_sb);
    struct veloxfs_inode_info *vi_info = VELOXFS_I(inode);
    int ret;

    ret = generic_write_end(iocb, mapping, pos, len, copied, folio, fsdata);
    if (ret > 0) {
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
    return ret;
}

#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)

/* 6.11 to 6.14: write_begin/end use struct file and struct folio */
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
    struct inode              *inode   = mapping->host;
    struct veloxfs_sb_info    *sbi     = VELOXFS_SB(inode->i_sb);
    struct veloxfs_inode_info *vi_info = VELOXFS_I(inode);
    int ret;

    ret = generic_write_end(file, mapping, pos, len, copied, folio, fsdata);
    if (ret > 0) {
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
    return ret;
}

#else /* < 6.11 */

/* Pre-6.11: write_begin/end use struct file and struct page */
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
    struct inode              *inode   = mapping->host;
    struct veloxfs_sb_info    *sbi     = VELOXFS_SB(inode->i_sb);
    struct veloxfs_inode_info *vi_info = VELOXFS_I(inode);
    int ret;

    ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
    if (ret > 0) {
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
    int err;

    err = file_write_and_wait_range(file, start, end);
    if (err)
        return err;

    mutex_lock(&sbi->lock);
    vlx_flush_fat(inode->i_sb);
    vlx_flush_inodes(inode->i_sb);
    vlx_flush_dir(inode->i_sb);
    mutex_unlock(&sbi->lock);

    return sync_blockdev(inode->i_sb->s_bdev);
}

/* =========================================================================
 * File inode operations
 *
 * veloxfs_getattr: use VLX_FILLATTR which selects the correct overload
 * for the running kernel (3-arg for <6.11, 4-arg with request_mask for 6.11+).
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
    int err;

    err = setattr_prepare_compat(mnt_ns, dentry, attr);
    if (err)
        return err;

    if (attr->ia_valid & ATTR_SIZE) {
        loff_t new_size = attr->ia_size;
        __u64  vlx_bs   = sbi->super.block_size;
        __u64  new_blks = ((__u64)new_size + vlx_bs - 1) / vlx_bs;
        __u64  have_blks;

        mutex_lock(&sbi->lock);
        have_blks = vlx_inode_total_blocks(sbi, &vi_info->vi);

        if (new_blks < have_blks) {
            vlx_inode_free_all(sbi, &vi_info->vi);
            if (new_blks > 0)
                vlx_inode_extend(sbi, &vi_info->vi, new_blks);
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
    .splice_read = VLX_SPLICE_READ,   /* filemap_splice_read (6.4+) or generic */
};

/* =========================================================================
 * Directory operations
 * ====================================================================== */
static int vlx_dir_path(struct veloxfs_sb_info *sbi, struct inode *dir,
                         char *buf, size_t bufsz)
{
    if (dir->i_ino == VELOXFS_ROOT_INO) {
        strncpy(buf, "/", bufsz);
        buf[bufsz - 1] = '\0';
        return 0;
    }
    {
        __u64 vlx_ino = VFS_TO_VLX_INO(dir->i_ino);
        __u64 i;
        for (i = 0; i < sbi->num_dirents; i++) {
            if (sbi->directory[i].inode_num == vlx_ino) {
                strncpy(buf, sbi->directory[i].path, bufsz - 1);
                buf[bufsz - 1] = '\0';
                return 0;
            }
        }
    }
    return -ENOENT;
}

static int veloxfs_iterate(struct file *file, struct dir_context *ctx)
{
    struct inode           *dir = file_inode(file);
    struct veloxfs_sb_info *sbi = VELOXFS_SB(dir->i_sb);
    char    dir_path[VELOXFS_MAX_PATH];
    __u64   emitted = 0;
    loff_t  skip    = ctx->pos - 2;
    __u64   i;

    if (!dir_emit_dots(file, ctx))
        return 0;

    mutex_lock(&sbi->lock);

    if (vlx_dir_path(sbi, dir, dir_path, sizeof(dir_path)) < 0) {
        mutex_unlock(&sbi->lock);
        return -ENOENT;
    }

    for (i = 0; i < sbi->num_dirents; i++) {
        struct veloxfs_disk_dirent *de = &sbi->directory[i];
        struct veloxfs_disk_inode  *vi;
        const char                 *name;
        unsigned int                name_len, dtype;

        if (de->inode_num == 0)
            continue;
        if (!vlx_is_direct_child(dir_path, de->path))
            continue;

        if ((loff_t)emitted < skip) {
            emitted++;
            continue;
        }

        name = strrchr(de->path, '/');
        if (!name || name[1] == '\0')
            continue;
        name++;
        name_len = (unsigned int)strlen(name);

        vi    = vlx_get_disk_inode(sbi, de->inode_num);
        dtype = (vi && (vi->inode_flags & VELOXFS_INODE_F_DIR)) ?
                DT_DIR : DT_REG;

        mutex_unlock(&sbi->lock);
        if (!dir_emit(ctx, name, name_len,
                      VLX_TO_VFS_INO(de->inode_num), dtype))
            return 0;
        mutex_lock(&sbi->lock);

        ctx->pos++;
        emitted++;
    }

    mutex_unlock(&sbi->lock);
    return 0;
}

static struct dentry *veloxfs_lookup(struct inode *dir, struct dentry *dentry,
                                      unsigned int flags)
{
    struct veloxfs_sb_info     *sbi = VELOXFS_SB(dir->i_sb);
    struct veloxfs_disk_dirent *de;
    struct inode               *inode = NULL;
    char dir_path[VELOXFS_MAX_PATH];
    char full_path[VELOXFS_MAX_PATH];

    mutex_lock(&sbi->lock);

    if (vlx_dir_path(sbi, dir, dir_path, sizeof(dir_path)) < 0) {
        mutex_unlock(&sbi->lock);
        return ERR_PTR(-ENOENT);
    }

    if (strcmp(dir_path, "/") == 0)
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
        if (IS_ERR(inode))
            return ERR_CAST(inode);
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
    struct super_block         *sb      = dir->i_sb;
    struct veloxfs_sb_info     *sbi     = VELOXFS_SB(sb);
    struct inode               *inode;
    struct veloxfs_inode_info  *vi_info;
    struct veloxfs_disk_inode  *disk_vi = NULL;
    struct veloxfs_disk_dirent *de;
    char   dir_path[VELOXFS_MAX_PATH];
    char   full_path[VELOXFS_MAX_PATH];
    __u64  new_ino = 0, i;
    u64    now = (u64)ktime_get_real_seconds();

    mutex_lock(&sbi->lock);

    if (vlx_dir_path(sbi, dir, dir_path, sizeof(dir_path)) < 0) {
        mutex_unlock(&sbi->lock);
        return -ENOENT;
    }

    if (strcmp(dir_path, "/") == 0)
        snprintf(full_path, sizeof(full_path), "/%.*s",
                 (int)dentry->d_name.len, dentry->d_name.name);
    else
        snprintf(full_path, sizeof(full_path), "%s/%.*s",
                 dir_path, (int)dentry->d_name.len, dentry->d_name.name);

    if (vlx_find_dirent(sbi, full_path)) {
        mutex_unlock(&sbi->lock);
        return -EEXIST;
    }

    for (i = 0; i < sbi->num_inodes; i++) {
        if (sbi->inodes[i].inode_num == 0) {
            disk_vi = &sbi->inodes[i];
            new_ino = i + 1;
            break;
        }
    }
    if (!disk_vi) {
        mutex_unlock(&sbi->lock);
        return -ENOSPC;
    }

    de = NULL;
    for (i = 0; i < sbi->num_dirents; i++) {
        if (sbi->directory[i].inode_num == 0) {
            de = &sbi->directory[i];
            break;
        }
    }
    if (!de) {
        mutex_unlock(&sbi->lock);
        return -ENOSPC;
    }

    memset(disk_vi, 0, sizeof(*disk_vi));
    disk_vi->inode_num   = new_ino;
    disk_vi->mode        = (__u32)mode;
    disk_vi->uid         = (__u32)from_kuid(&init_user_ns, current_fsuid());
    disk_vi->gid         = (__u32)from_kgid(&init_user_ns, current_fsgid());
    disk_vi->ctime       = now;
    disk_vi->mtime       = now;
    disk_vi->atime       = now;
    disk_vi->size        = 0;
    disk_vi->inode_flags = VELOXFS_INODE_F_EXTENTS;
    if (is_dir)
        disk_vi->inode_flags |= VELOXFS_INODE_F_DIR;

    strncpy(de->path, full_path, VELOXFS_MAX_PATH - 1);
    de->path[VELOXFS_MAX_PATH - 1] = '\0';
    de->inode_num = new_ino;

    sbi->dirty_inodes = 1;
    sbi->dirty_dir    = 1;
    mutex_unlock(&sbi->lock);

    inode = new_inode(sb);
    if (!inode)
        return -ENOMEM;

    vi_info = VELOXFS_I(inode);
    mutex_lock(&sbi->lock);
    memcpy(&vi_info->vi, disk_vi, sizeof(*disk_vi));
    mutex_unlock(&sbi->lock);

    inode->i_ino    = VLX_TO_VFS_INO(new_ino);
    inode->i_mode   = mode;
    inode->i_blocks = 0;

    /* veloxfs_set_inode_times() selects the correct API per kernel version */
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

static int veloxfs_create(VLX_NS *mnt_ns, struct inode *dir,
                           struct dentry *dentry, umode_t mode, bool excl)
{
    return veloxfs_mknod(mnt_ns, dir, dentry, mode | S_IFREG, false);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
static struct dentry *veloxfs_mkdir(VLX_NS *mnt_ns, struct inode *dir,
                                    struct dentry *dentry, umode_t mode)
{
    int err = veloxfs_mknod(mnt_ns, dir, dentry, mode | S_IFDIR, true);
    if (!err) {
        inc_nlink(dir);
        return NULL; /* Success */
    }
    return ERR_PTR(err); /* Failure */
}
#else
static int veloxfs_mkdir(VLX_NS *mnt_ns, struct inode *dir,
                          struct dentry *dentry, umode_t mode)
{
    int err = veloxfs_mknod(mnt_ns, dir, dentry, mode | S_IFDIR, true);
    if (!err)
        inc_nlink(dir);
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
    __u64 vlx_ino = vi_info->vi.inode_num;
    __u64 i;

    if (!vlx_ino)
        return -ENOENT;

    mutex_lock(&sbi->lock);
    vlx_inode_free_all(sbi, &sbi->inodes[vlx_ino - 1]);
    memset(&sbi->inodes[vlx_ino - 1], 0, sizeof(struct veloxfs_disk_inode));
    sbi->dirty_inodes = 1;

    for (i = 0; i < sbi->num_dirents; i++) {
        if (sbi->directory[i].inode_num == vlx_ino) {
            memset(&sbi->directory[i], 0,
                   sizeof(struct veloxfs_disk_dirent));
            sbi->dirty_dir = 1;
            break;
        }
    }
    mutex_unlock(&sbi->lock);

    /*
     * Update timestamps — use the accessor helpers so this compiles on
     * both old kernels (direct member) and 6.6+ (accessor functions).
     */
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
    __u64 vlx_ino = vi_info->vi.inode_num;
    __u64 i;
    int err;

    mutex_lock(&sbi->lock);

    dir_path[0] = '\0';
    for (i = 0; i < sbi->num_dirents; i++) {
        if (sbi->directory[i].inode_num == vlx_ino) {
            strncpy(dir_path, sbi->directory[i].path, sizeof(dir_path) - 1);
            dir_path[sizeof(dir_path) - 1] = '\0';
            break;
        }
    }

    for (i = 0; i < sbi->num_dirents; i++) {
        if (sbi->directory[i].inode_num == 0)
            continue;
        if (vlx_is_direct_child(dir_path, sbi->directory[i].path)) {
            mutex_unlock(&sbi->lock);
            return -ENOTEMPTY;
        }
    }
    mutex_unlock(&sbi->lock);

    err = veloxfs_unlink(dir, dentry);
    if (!err)
        drop_nlink(dir);
    return err;
}

static int veloxfs_rename(VLX_NS *mnt_ns,
                           struct inode *old_dir, struct dentry *old_dentry,
                           struct inode *new_dir, struct dentry *new_dentry,
                           unsigned int flags)
{
    struct veloxfs_sb_info    *sbi     = VELOXFS_SB(old_dir->i_sb);
    struct veloxfs_inode_info *vi_info = VELOXFS_I(d_inode(old_dentry));
    char   new_dir_path[VELOXFS_MAX_PATH];
    char   new_full[VELOXFS_MAX_PATH];
    __u64  vlx_ino = vi_info->vi.inode_num;
    __u64  i;

    if (flags & ~RENAME_NOREPLACE)
        return -EINVAL;

    mutex_lock(&sbi->lock);

    if (vlx_dir_path(sbi, new_dir, new_dir_path, sizeof(new_dir_path)) < 0) {
        mutex_unlock(&sbi->lock);
        return -ENOENT;
    }

    if (strcmp(new_dir_path, "/") == 0)
        snprintf(new_full, sizeof(new_full), "/%.*s",
                 (int)new_dentry->d_name.len, new_dentry->d_name.name);
    else
        snprintf(new_full, sizeof(new_full), "%s/%.*s",
                 new_dir_path,
                 (int)new_dentry->d_name.len, new_dentry->d_name.name);

    if ((flags & RENAME_NOREPLACE) && vlx_find_dirent(sbi, new_full)) {
        mutex_unlock(&sbi->lock);
        return -EEXIST;
    }

    {
        struct veloxfs_disk_dirent *existing = vlx_find_dirent(sbi, new_full);
        if (existing) {
            __u64 old_ino = existing->inode_num;
            vlx_inode_free_all(sbi, &sbi->inodes[old_ino - 1]);
            memset(&sbi->inodes[old_ino - 1], 0,
                   sizeof(struct veloxfs_disk_inode));
            memset(existing, 0, sizeof(*existing));
            sbi->dirty_inodes = 1;
        }
    }

    for (i = 0; i < sbi->num_dirents; i++) {
        if (sbi->directory[i].inode_num == vlx_ino) {
            strncpy(sbi->directory[i].path, new_full, VELOXFS_MAX_PATH - 1);
            sbi->directory[i].path[VELOXFS_MAX_PATH - 1] = '\0';
            sbi->dirty_dir = 1;
            break;
        }
    }

    mutex_unlock(&sbi->lock);
    return 0;
}

static const struct inode_operations veloxfs_dir_inode_ops = {
    .lookup  = veloxfs_lookup,
    .create  = veloxfs_create,
    .mkdir   = veloxfs_mkdir,
    .unlink  = veloxfs_unlink,
    .rmdir   = veloxfs_rmdir,
    .rename  = veloxfs_rename,
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

    if (inode->i_ino == VELOXFS_ROOT_INO)
        return 0;

    vlx_ino = VFS_TO_VLX_INO(inode->i_ino);
    if (vlx_ino == 0 || vlx_ino > sbi->num_inodes)
        return -EIO;

    mutex_lock(&sbi->lock);

    vi_info->vi.mode  = (__u32)inode->i_mode;
    vi_info->vi.uid   = (__u32)i_uid_read(inode);
    vi_info->vi.gid   = (__u32)i_gid_read(inode);
    vi_info->vi.size  = (__u64)inode->i_size;

    /*
     * Read timestamps through the version-agnostic accessor macros.
     * On <6.6: vlx_inode_atime(inode) expands to inode->i_atime.
     * On 6.6+: expands to inode_get_atime(inode).
     */
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

    if (inode->i_nlink == 0 && inode->i_ino != VELOXFS_ROOT_INO) {
        __u64 vlx_ino = vi_info->vi.inode_num;
        if (vlx_ino != 0 && vlx_ino <= sbi->num_inodes) {
            mutex_lock(&sbi->lock);
            vlx_inode_free_all(sbi, &sbi->inodes[vlx_ino - 1]);
            memset(&sbi->inodes[vlx_ino - 1], 0,
                   sizeof(struct veloxfs_disk_inode));
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

    if (!sbi)
        return;

    mutex_lock(&sbi->lock);
    vlx_flush_fat(sb);
    vlx_flush_inodes(sb);
    vlx_flush_dir(sb);
    mutex_unlock(&sbi->lock);

    vfree(sbi->fat);
    vfree(sbi->inodes);
    vfree(sbi->directory);
    kfree(sbi);
    sb->s_fs_info = NULL;
}

static int veloxfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    struct veloxfs_sb_info *sbi = VELOXFS_SB(dentry->d_sb);
    __u64 used = 0, free_blks, data_blks, i;

    mutex_lock(&sbi->lock);
    for (i = sbi->super.data_start; i < sbi->super.block_count; i++)
        if (sbi->fat[i] != VELOXFS_FAT_FREE)
            used++;
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
    .alloc_inode   = veloxfs_alloc_inode,
    .free_inode    = veloxfs_free_inode,
    .write_inode   = veloxfs_write_inode,
    .evict_inode   = veloxfs_evict_inode,
    .put_super     = veloxfs_put_super,
    .statfs        = veloxfs_statfs,
};

/* =========================================================================
 * fill_super: shared between old mount_bdev API and new fs_context API.
 *
 * For kernel 7.0+, mount_bdev was removed.  We provide two versions:
 *   veloxfs_fill_super_legacy(sb, data, silent) — used by mount_bdev (<7.0)
 *   veloxfs_fill_super_fc(sb, fc)               — used by get_tree_bdev (7.0+)
 *
 * Both delegate to the common veloxfs_fill_super_impl() so there is no
 * code duplication.
 * ====================================================================== */
static int veloxfs_fill_super_impl(struct super_block *sb, int silent)
{
    struct veloxfs_sb_info     *sbi = NULL;
    struct buffer_head         *bh;
    struct veloxfs_superblock  *disk_super;
    struct inode               *root_inode;
    struct dentry              *root_dentry;
    u32    vlx_bs, vfs_bs;
    __u64  fat_bytes, inode_bytes, dir_bytes;
    __u64  ratio, i;
    int    err = -EINVAL;

    sb_set_blocksize(sb, 1024);
    bh = sb_bread(sb, 0);
    if (!bh) {
        if (!silent)
            pr_err("veloxfs: cannot read superblock from device\n");
        return -EIO;
    }

    disk_super = (struct veloxfs_superblock *)bh->b_data;

    if (disk_super->magic != VELOXFS_MAGIC_V6) {
        if (!silent)
            pr_err("veloxfs: bad magic 0x%08x (expected 0x%08lx)\n",
                   disk_super->magic, (unsigned long)VELOXFS_MAGIC_V6);
        brelse(bh);
        return -EINVAL;
    }

    vlx_bs = disk_super->block_size;
    if (vlx_bs < VELOXFS_BS_MIN || vlx_bs > VELOXFS_BS_MAX ||
        (vlx_bs & (vlx_bs - 1)) != 0) {
        if (!silent)
            pr_err("veloxfs: invalid block_size %u\n", vlx_bs);
        brelse(bh);
        return -EINVAL;
    }

    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi) { brelse(bh); return -ENOMEM; }

    memcpy(&sbi->super, disk_super, sizeof(sbi->super));
    brelse(bh);

    mutex_init(&sbi->lock);
    sbi->last_alloc_hint = sbi->super.data_start;

    vfs_bs = min_t(u32, vlx_bs, (u32)PAGE_SIZE);
    if (!sb_set_blocksize(sb, (int)vfs_bs)) {
        pr_err("veloxfs: device cannot support block size %u\n", vfs_bs);
        err = -EINVAL;
        goto err_sbi;
    }

    sb->s_magic    = VELOXFS_MAGIC;
    sb->s_op       = &veloxfs_sops;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_fs_info  = sbi;

    ratio = vlx_bs / vfs_bs;

    fat_bytes = sbi->super.block_count * sizeof(__u64);
    sbi->fat  = vmalloc(fat_bytes);
    if (!sbi->fat) { err = -ENOMEM; goto err_sbi; }

    for (i = 0; i < sbi->super.fat_blocks; i++) {
        __u64 j;
        u8 *dst = (u8 *)sbi->fat + i * vlx_bs;
        for (j = 0; j < ratio; j++) {
            sector_t phys = (sector_t)((sbi->super.fat_start + i) * ratio + j);
            bh = sb_bread(sb, phys);
            if (!bh) { err = -EIO; goto err_fat; }
            memcpy(dst + j * vfs_bs, bh->b_data, vfs_bs);
            brelse(bh);
        }
    }

    inode_bytes     = sbi->super.inode_blocks * (__u64)vlx_bs;
    sbi->inodes     = vmalloc(inode_bytes);
    sbi->num_inodes = inode_bytes / sizeof(struct veloxfs_disk_inode);
    if (!sbi->inodes) { err = -ENOMEM; goto err_fat; }

    for (i = 0; i < sbi->super.inode_blocks; i++) {
        __u64 j;
        u8 *dst = (u8 *)sbi->inodes + i * vlx_bs;
        for (j = 0; j < ratio; j++) {
            sector_t phys = (sector_t)((sbi->super.inode_start + i) * ratio + j);
            bh = sb_bread(sb, phys);
            if (!bh) { err = -EIO; goto err_inodes; }
            memcpy(dst + j * vfs_bs, bh->b_data, vfs_bs);
            brelse(bh);
        }
    }

    sbi->dir_start_blk = sbi->super.inode_start + sbi->super.inode_blocks;
    sbi->dir_blocks    = vlx_calc_dir_blocks(sbi->super.block_count);
    dir_bytes          = sbi->dir_blocks * (__u64)vlx_bs;
    sbi->directory     = vmalloc(dir_bytes);
    sbi->num_dirents   = dir_bytes / sizeof(struct veloxfs_disk_dirent);
    if (!sbi->directory) { err = -ENOMEM; goto err_inodes; }

    for (i = 0; i < sbi->dir_blocks; i++) {
        __u64 j;
        u8 *dst = (u8 *)sbi->directory + i * vlx_bs;
        for (j = 0; j < ratio; j++) {
            sector_t phys = (sector_t)((sbi->dir_start_blk + i) * ratio + j);
            bh = sb_bread(sb, phys);
            if (!bh) { err = -EIO; goto err_dir; }
            memcpy(dst + j * vfs_bs, bh->b_data, vfs_bs);
            brelse(bh);
        }
    }

    root_inode = new_inode(sb);
    if (!root_inode) { err = -ENOMEM; goto err_dir; }

    root_inode->i_ino  = VELOXFS_ROOT_INO;
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_op   = &veloxfs_dir_inode_ops;
    root_inode->i_fop  = &veloxfs_dir_ops;
    /* Use the portable helper for root inode timestamps */
    veloxfs_set_inode_times(root_inode, current_time(root_inode));
    set_nlink(root_inode, 2);

    root_dentry = d_make_root(root_inode);
    if (!root_dentry) { err = -ENOMEM; goto err_dir; }
    sb->s_root = root_dentry;

    pr_info("veloxfs: mounted — vlx_bs=%u vfs_bs=%u blocks=%llu inodes=%llu\n",
            vlx_bs, vfs_bs,
            (unsigned long long)sbi->super.block_count,
            (unsigned long long)sbi->num_inodes);
    return 0;

err_dir:    vfree(sbi->directory);
err_inodes: vfree(sbi->inodes);
err_fat:    vfree(sbi->fat);
err_sbi:    kfree(sbi);
    sb->s_fs_info = NULL;
    return err;
}

/* =========================================================================
 * Mount API — two paths based on kernel version
 *
 *   < 7.0 : classic .mount = veloxfs_mount → mount_bdev
 *   7.0+  : .init_fs_context → get_tree_bdev (mount_bdev was removed)
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

#else /* < 7.0: classic mount_bdev API */

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

#endif /* mount API */

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
                          sizeof(struct veloxfs_inode_info),
                          0,
                          SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
                          veloxfs_init_once);
    if (!veloxfs_inode_cachep)
        return -ENOMEM;

    err = register_filesystem(&veloxfs_fs_type);
    if (err) {
        kmem_cache_destroy(veloxfs_inode_cachep);
        return err;
    }

    pr_info("VeloxFS v6: driver loaded (kernel %d.%d)\n",
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
