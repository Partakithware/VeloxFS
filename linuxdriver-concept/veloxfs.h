/* veloxfs.h - v6.0 - Maxwell Wingate - https://github.com/Partakithware/VeloxFS/
 *
 * Storage-Optimized Edition: Flash / NAND / HDD
 *
 * STANDALONE DESIGN
 * =================
 * This header has zero hard dependencies on any system library.  All system
 * calls (malloc, memcpy, printf, time …) go through overridable macros.
 * Define your replacements BEFORE including this header:
 *
 *   // Kernel module example:
 *   #include <linux/slab.h>
 *   #include <linux/string.h>
 *   #include <linux/printk.h>
 *   #define veloxfs_MALLOC(sz)         kmalloc(sz, GFP_KERNEL)
 *   #define veloxfs_CALLOC(n, sz)      kzalloc((n)*(sz), GFP_KERNEL)
 *   #define veloxfs_FREE(p)            kfree(p)
 *   #define veloxfs_MEMSET(d,c,n)      memset(d,c,n)
 *   #define veloxfs_MEMCPY(d,s,n)      memcpy(d,s,n)
 *   #define veloxfs_MEMMOVE(d,s,n)     memmove(d,s,n)
 *   #define veloxfs_STRLEN(s)          strlen(s)
 *   #define veloxfs_STRCMP(a,b)        strcmp(a,b)
 *   #define veloxfs_STRNCMP(a,b,n)     strncmp(a,b,n)
 *   #define veloxfs_STRCHR(s,c)        strchr(s,c)
 *   #define veloxfs_STRNCPY(d,s,n)     strncpy(d,s,n)
 *   #define veloxfs_SNPRINTF(d,n,...)  snprintf(d,n,__VA_ARGS__)
 *   #define veloxfs_LOG(fmt,...)       printk(KERN_INFO fmt, ##__VA_ARGS__)
 *   #define veloxfs_TIME()             ktime_get_real_seconds()
 *   #define veloxfs_IMPLEMENTATION
 *   #include "veloxfs.h"
 *
 *   // Embedded bare-metal example (you supply my_malloc etc):
 *   #define veloxfs_MALLOC(sz)         my_malloc(sz)
 *   #define veloxfs_FREE(p)            my_free(p)
 *   ... etc ...
 *
 * For normal userspace usage: just #define veloxfs_IMPLEMENTATION and
 * #include "veloxfs.h" — the stdlib defaults are picked up automatically.
 *
 * =========================================================================
 * v6.0 — WHY v5 IS SLOW ON HDD/FLASH/NAND
 * =========================================================================
 * v5 was designed around NVMe assumptions:
 *   - 4 KB block size  → one seek per block on HDD (5 ms × 256K blocks = 21 min/GB)
 *   - Zero-on-alloc    → EVERY allocated block gets an extra write before data
 *   - Single-block I/O → pread()/pwrite() called once per 4 KB, even for sequential data
 *   - Next-fit alloc   → blocks scatter randomly; kills HDD seek patterns and NAND WAF
 *
 * v6.0 CHANGES
 * ============
 *  1. Configurable block size at format time (default 64 KB HDD, 128 KB NAND)
 *  2. Contiguous-first block allocator
 *  3. Zero-on-alloc removed
 *  4. 4-extent inline inode map (same 144-byte inode struct as v5)
 *  5. Large single I/O per extent
 *  6. Heap-allocated block scratch buffer (fs->block_buf)
 *  7. veloxfs_format_ex() — explicit block_size + storage_hint
 *
 * USAGE
 *   #define veloxfs_IMPLEMENTATION
 *   #include "veloxfs.h"
 *
 * LICENSE: Public Domain or MIT (see end of file)
 */

#ifndef veloxfs_H
#define veloxfs_H

/* ============================================================================
 * Standalone portable types
 *
 * When included in a kernel module, linux/types.h provides these already
 * (as u8, u16 etc).  For userspace, stdint.h is used.  Since this header
 * must work in both contexts we conditionally pull in stdint/stddef only
 * when they haven't been superseded.
 * ========================================================================= */
#if !defined(__KERNEL__)
#  include <stdint.h>
#  include <stddef.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Overridable hook macros — define BEFORE including this header to customise.
 * Defaults: standard C library (suitable for userspace / POSIX targets).
 * ========================================================================= */

/* --- Memory allocation --- */
#ifndef veloxfs_MALLOC
#  include <stdlib.h>
#  define veloxfs_MALLOC(sz)           malloc(sz)
#  define veloxfs_CALLOC(n, sz)        calloc((n), (sz))
#  define veloxfs_FREE(p)              free(p)
#endif

/* --- String / memory operations --- */
#ifndef veloxfs_MEMSET
#  include <string.h>
#  define veloxfs_MEMSET(d, c, n)      memset((d), (c), (n))
#  define veloxfs_MEMCPY(d, s, n)      memcpy((d), (s), (n))
#  define veloxfs_MEMMOVE(d, s, n)     memmove((d), (s), (n))
#  define veloxfs_STRLEN(s)            strlen(s)
#  define veloxfs_STRCMP(a, b)         strcmp((a), (b))
#  define veloxfs_STRNCMP(a, b, n)     strncmp((a), (b), (n))
#  define veloxfs_STRCHR(s, c)         strchr((s), (c))
#  define veloxfs_STRNCPY(d, s, n)     strncpy((d), (s), (n))
#  define veloxfs_SNPRINTF(d, n, ...)  snprintf((d), (n), __VA_ARGS__)
#endif

/* --- Logging --- */
#ifndef veloxfs_LOG
#  include <stdio.h>
#  define veloxfs_LOG(fmt, ...)        printf(fmt, ##__VA_ARGS__)
#endif

/* --- Timestamp --- */
#ifndef veloxfs_TIME
#  include <time.h>
#  define veloxfs_TIME()               time(NULL)
#endif

/* ============================================================================
 * Cross-platform struct packing
 * ========================================================================= */
#if defined(_MSC_VER)
#  define veloxfs_PACKED_START __pragma(pack(push,1))
#  define veloxfs_PACKED_END   __pragma(pack(pop))
#  define veloxfs_ATTR_PACKED
#else
#  define veloxfs_PACKED_START
#  define veloxfs_PACKED_END
#  define veloxfs_ATTR_PACKED  __attribute__((packed))
#endif

/* ============================================================================
 * Configuration & magic
 * ========================================================================= */

#define veloxfs_MAGIC_V6        0x564C5836UL   /* "VLX6" -- incompatible with v5 */
#define veloxfs_MAGIC           veloxfs_MAGIC_V6
#define veloxfs_VERSION         6

#define veloxfs_BS_NVME         4096
#define veloxfs_BS_HDD          65536
#define veloxfs_BS_FLASH        65536
#define veloxfs_BS_NAND         131072
#define veloxfs_BS_DEFAULT      veloxfs_BS_HDD

#define veloxfs_BS_MIN          4096
#define veloxfs_BS_MAX          (1U << 20)

#define veloxfs_BS(fs)          ((fs)->super.block_size)

#define veloxfs_STORAGE_AUTO    0
#define veloxfs_STORAGE_HDD     1
#define veloxfs_STORAGE_FLASH   2
#define veloxfs_STORAGE_NAND    3

#define veloxfs_INLINE_EXTENTS  4

#define veloxfs_INODE_F_EXTENTS 0x01
#define veloxfs_INODE_F_DIR     0x02

#define veloxfs_MAX_PATH        480
#define veloxfs_JOURNAL_SIZE    64

/* ============================================================================
 * Error codes
 * ========================================================================= */
typedef enum {
    veloxfs_OK             =  0,
    veloxfs_ERR_IO         = -1,
    veloxfs_ERR_CORRUPT    = -2,
    veloxfs_ERR_NOT_FOUND  = -3,
    veloxfs_ERR_EXISTS     = -4,
    veloxfs_ERR_NO_SPACE   = -5,
    veloxfs_ERR_INVALID    = -6,
    veloxfs_ERR_TOO_LARGE  = -7,
    veloxfs_ERR_TOO_MANY_FILES = -8,
    veloxfs_ERR_PERMISSION = -9,
} veloxfs_error;

/* ============================================================================
 * Permission bits
 * ========================================================================= */
#define veloxfs_S_IRUSR  0400
#define veloxfs_S_IWUSR  0200
#define veloxfs_S_IXUSR  0100
#define veloxfs_S_IRGRP  0040
#define veloxfs_S_IWGRP  0020
#define veloxfs_S_IXGRP  0010
#define veloxfs_S_IROTH  0004
#define veloxfs_S_IWOTH  0002
#define veloxfs_S_IXOTH  0001
#define veloxfs_S_IRWXU  0700
#define veloxfs_S_IRWXG  0070
#define veloxfs_S_IRWXO  0007

/* ============================================================================
 * FAT sentinel values
 * ========================================================================= */
#define veloxfs_FAT_FREE  0x0000000000000000ULL
#define veloxfs_FAT_EOF   0xFFFFFFFFFFFFFFFFULL
#define veloxfs_FAT_BAD   0xFFFFFFFFFFFFFFFEULL

/* ============================================================================
 * Journal operation types
 * ========================================================================= */
typedef enum {
    veloxfs_JOP_NONE     = 0,
    veloxfs_JOP_CREATE   = 1,
    veloxfs_JOP_DELETE   = 2,
    veloxfs_JOP_WRITE    = 3,
    veloxfs_JOP_EXTEND   = 4,
    veloxfs_JOP_TRUNCATE = 5,
    veloxfs_JOP_CHMOD    = 6,
    veloxfs_JOP_CHOWN    = 7,
} veloxfs_journal_op;

/* ============================================================================
 * I/O callbacks
 * ========================================================================= */
typedef int (*veloxfs_read_fn) (void *user, uint64_t offset, void       *buf, uint32_t size);
typedef int (*veloxfs_write_fn)(void *user, uint64_t offset, const void *buf, uint32_t size);

typedef struct {
    veloxfs_read_fn  read;
    veloxfs_write_fn write;
    void            *user_data;
} veloxfs_io;

/* ============================================================================
 * On-disk structures
 * ========================================================================= */

veloxfs_PACKED_START
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t journal_enabled;
    uint32_t storage_hint;
    uint32_t _pad0;
    uint64_t block_count;
    uint64_t fat_start;
    uint64_t fat_blocks;
    uint64_t journal_start;
    uint64_t journal_blocks;
    uint64_t inode_start;
    uint64_t inode_blocks;
    uint64_t data_start;
    uint64_t reserved[4];
} veloxfs_ATTR_PACKED veloxfs_superblock;
veloxfs_PACKED_END

veloxfs_PACKED_START
typedef struct {
    uint64_t start_block;
    uint64_t block_count;
} veloxfs_ATTR_PACKED veloxfs_extent;
veloxfs_PACKED_END

veloxfs_PACKED_START
typedef struct {
    uint64_t inode_num;
    uint64_t size;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint32_t inode_flags;
    uint64_t ctime;
    uint64_t mtime;
    uint64_t atime;
    uint64_t fat_head;
    veloxfs_extent extents[veloxfs_INLINE_EXTENTS];
    uint64_t extent_count;
    uint64_t _ipad;
    /* Add this padding to reach 256 bytes */
    uint64_t _reserved_padding[14]; 
} veloxfs_ATTR_PACKED veloxfs_inode;
veloxfs_PACKED_END

typedef char veloxfs_inode_size_check[
    (sizeof(veloxfs_inode) == 256) ? 1 : -1];

veloxfs_PACKED_START
typedef struct {
    char     path[veloxfs_MAX_PATH];
    uint64_t inode_num;
    uint64_t reserved[2];
} veloxfs_ATTR_PACKED veloxfs_dirent;
veloxfs_PACKED_END

veloxfs_PACKED_START
typedef struct {
    uint32_t sequence;
    uint32_t op_type;
    uint64_t inode_num;
    uint64_t block_addr;
    uint64_t old_value;
    uint64_t new_value;
    uint32_t checksum;
    uint32_t committed;
} veloxfs_ATTR_PACKED veloxfs_journal_entry;
veloxfs_PACKED_END

/* ============================================================================
 * Runtime handle
 * ========================================================================= */
typedef struct {
    veloxfs_io         io;
    veloxfs_superblock super;
    uint64_t          *fat;
    veloxfs_inode     *inodes;
    veloxfs_dirent    *directory;
    veloxfs_journal_entry *journal;
    uint8_t           *block_buf;
    uint64_t           num_inodes;
    uint64_t           num_dirents;
    uint32_t           next_journal_seq;
    uint32_t           current_uid;
    uint32_t           current_gid;
    int                dirty_fat;
    int                dirty_inodes;
    int                dirty_dir;
    int                dirty_journal;
    uint64_t          *dir_hash_table;
    uint64_t           dir_hash_size;
    uint64_t           last_alloc_hint;
} veloxfs_handle;

/* ============================================================================
 * File handle
 * ========================================================================= */
typedef struct {
    veloxfs_handle *fs;
    veloxfs_inode  *inode;
    uint64_t        position;
    uint64_t        cur_extent_idx;
    uint64_t        cur_extent_logical_start;
    uint64_t        fat_block;
    uint64_t        fat_block_idx;
    int             is_open;
    int             modified;
    int             can_read;
    int             can_write;
} veloxfs_file;

/* ============================================================================
 * Stat structures
 * ========================================================================= */
typedef struct {
    uint64_t size;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint64_t block_count;
    uint64_t ctime;
    uint64_t mtime;
    uint64_t atime;
} veloxfs_stat_t;

typedef struct {
    uint64_t total_blocks;
    uint64_t used_blocks;
    uint64_t free_blocks;
    uint64_t extent_files;
    uint64_t fat_overflow;
    float    avg_extents;
} veloxfs_alloc_stats;

/* ============================================================================
 * Public API
 * ========================================================================= */

int veloxfs_format(veloxfs_io io, uint64_t block_count, int enable_journal);
int veloxfs_format_ex(veloxfs_io io, uint64_t block_count, int enable_journal,
                      uint32_t block_size, uint32_t storage_hint);

int veloxfs_mount  (veloxfs_handle *fs, veloxfs_io io);
int veloxfs_unmount(veloxfs_handle *fs);
int veloxfs_sync   (veloxfs_handle *fs);
int veloxfs_fsck   (veloxfs_handle *fs);

void veloxfs_set_user(veloxfs_handle *fs, uint32_t uid, uint32_t gid);
void veloxfs_get_user(veloxfs_handle *fs, uint32_t *uid, uint32_t *gid);

int veloxfs_create(veloxfs_handle *fs, const char *path, uint32_t mode);
int veloxfs_delete(veloxfs_handle *fs, const char *path);
int veloxfs_rename(veloxfs_handle *fs, const char *old_path, const char *new_path);
int veloxfs_chmod (veloxfs_handle *fs, const char *path, uint32_t mode);
int veloxfs_chown (veloxfs_handle *fs, const char *path, uint32_t uid, uint32_t gid);

int veloxfs_write_file(veloxfs_handle *fs, const char *path, const void *data, uint64_t size);
int veloxfs_read_file (veloxfs_handle *fs, const char *path, void *out,
                       uint64_t max_size, uint64_t *out_size);

#define veloxfs_O_RDONLY 0x01
#define veloxfs_O_WRONLY 0x02
#define veloxfs_O_RDWR   0x03

int      veloxfs_open           (veloxfs_handle *fs, const char *path, int flags, veloxfs_file *file);
int      veloxfs_close          (veloxfs_file *file);
int      veloxfs_read           (veloxfs_file *file, void *buf, uint64_t count, uint64_t *bytes_read);
int      veloxfs_write          (veloxfs_file *file, const void *buf, uint64_t count);
int      veloxfs_seek           (veloxfs_file *file, int64_t offset, int whence);
uint64_t veloxfs_tell           (veloxfs_file *file);
int      veloxfs_truncate_handle(veloxfs_file *file, uint64_t new_size);

int veloxfs_mkdir(veloxfs_handle *fs, const char *path, uint32_t mode);

typedef void (*veloxfs_list_callback)(const char *path, const veloxfs_stat_t *stat,
                                      int is_dir, void *user_data);
int veloxfs_list(veloxfs_handle *fs, const char *path,
                 veloxfs_list_callback callback, void *user_data);

int veloxfs_stat       (veloxfs_handle *fs, const char *path, veloxfs_stat_t *stat);
int veloxfs_statfs     (veloxfs_handle *fs, uint64_t *total, uint64_t *used, uint64_t *free_blocks);
int veloxfs_alloc_stats_get(veloxfs_handle *fs, veloxfs_alloc_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* veloxfs_H */

/* ============================================================================
 * IMPLEMENTATION
 * ========================================================================= */
#ifdef veloxfs_IMPLEMENTATION

/* ============================================================================
 * CRC32 (self-contained — no system library needed)
 * ========================================================================= */
static uint32_t g_crc32_table[256];
static int      g_crc32_ready = 0;

static void crc32_init(void) {
    if (g_crc32_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ ((c & 1) ? 0xEDB88320U : 0);
        g_crc32_table[i] = c;
    }
    g_crc32_ready = 1;
}

static uint32_t crc32_compute(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++)
        crc = g_crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ============================================================================
 * Raw block I/O
 * ========================================================================= */
static int veloxfs_read_block(veloxfs_handle *fs, uint64_t block_idx, void *buf) {
    uint64_t off = block_idx * veloxfs_BS(fs);
    return fs->io.read(fs->io.user_data, off, buf, (uint32_t)veloxfs_BS(fs));
}

static int veloxfs_write_block(veloxfs_handle *fs, uint64_t block_idx, const void *buf) {
    uint64_t off = block_idx * veloxfs_BS(fs);
    return fs->io.write(fs->io.user_data, off, buf, (uint32_t)veloxfs_BS(fs));
}

static int veloxfs_read_blocks(veloxfs_handle *fs, uint64_t block_start,
                                void *buf, uint64_t count) {
    uint64_t off  = block_start * veloxfs_BS(fs);
    uint64_t size = count       * veloxfs_BS(fs);
    return fs->io.read(fs->io.user_data, off, buf, (uint32_t)size);
}

static int veloxfs_write_blocks(veloxfs_handle *fs, uint64_t block_start,
                                 const void *buf, uint64_t count) {
    uint64_t off  = block_start * veloxfs_BS(fs);
    uint64_t size = count       * veloxfs_BS(fs);
    return fs->io.write(fs->io.user_data, off, buf, (uint32_t)size);
}

/* ============================================================================
 * Layout calculators
 * ========================================================================= */
static uint64_t calc_fat_blocks(uint64_t block_count, uint32_t block_size) {
    uint64_t entries_per = block_size / sizeof(uint64_t);
    return (block_count + entries_per - 1) / entries_per;
}

static uint64_t calc_inode_blocks(uint64_t block_count, uint32_t block_size) {
    (void)block_size;
    uint64_t n = block_count / 50;
    return n ? n : 1;
}

static uint64_t calc_dir_blocks(uint64_t block_count, uint32_t block_size) {
    (void)block_size;
    uint64_t n = block_count / 100;
    return n ? n : 1;
}

/* ============================================================================
 * Path normalisation — uses veloxfs_SNPRINTF / veloxfs_STRNCPY / veloxfs_STRLEN
 * ========================================================================= */
static void normalize_path(const char *in, char *out, size_t out_size) {
    if (in[0] != '/')
        veloxfs_SNPRINTF(out, out_size, "/%s", in);
    else {
        veloxfs_STRNCPY(out, in, out_size - 1);
        out[out_size - 1] = '\0';
    }
    size_t len = veloxfs_STRLEN(out);
    if (len > 1 && out[len - 1] == '/')
        out[len - 1] = '\0';
}

/* ============================================================================
 * Directory hash table
 * ========================================================================= */
static uint64_t hash_path(const char *path) {
    uint64_t h = 14695981039346656037ULL;
    while (*path) { h ^= (uint8_t)(*path++); h *= 1099511628211ULL; }
    return h;
}

static void veloxfs_build_hash_table(veloxfs_handle *fs) {
    fs->dir_hash_size  = fs->num_dirents + (fs->num_dirents / 2) + 16;
    fs->dir_hash_table = (uint64_t*)veloxfs_CALLOC(fs->dir_hash_size, sizeof(uint64_t));
    if (!fs->dir_hash_table) { fs->dir_hash_size = 0; return; }

    for (uint64_t i = 0; i < fs->dir_hash_size; i++)
        fs->dir_hash_table[i] = UINT64_MAX;

    for (uint64_t i = 0; i < fs->num_dirents; i++) {
        if (fs->directory[i].inode_num == 0) continue;
        uint64_t slot = hash_path(fs->directory[i].path) % fs->dir_hash_size;
        while (fs->dir_hash_table[slot] != UINT64_MAX)
            slot = (slot + 1) % fs->dir_hash_size;
        fs->dir_hash_table[slot] = i;
    }
}

static void veloxfs_hash_insert(veloxfs_handle *fs, uint64_t idx) {
    if (!fs->dir_hash_table) return;
    uint64_t slot = hash_path(fs->directory[idx].path) % fs->dir_hash_size;
    while (fs->dir_hash_table[slot] != UINT64_MAX)
        slot = (slot + 1) % fs->dir_hash_size;
    fs->dir_hash_table[slot] = idx;
}

static void veloxfs_hash_remove(veloxfs_handle *fs, const char *path) {
    if (!fs->dir_hash_table) return;
    uint64_t slot = hash_path(path) % fs->dir_hash_size;
    for (uint64_t p = 0; p < fs->dir_hash_size; p++) {
        uint64_t idx = fs->dir_hash_table[slot];
        if (idx == UINT64_MAX) return;
        if (fs->directory[idx].inode_num != 0 &&
            veloxfs_STRCMP(fs->directory[idx].path, path) == 0) {
            fs->dir_hash_table[slot] = UINT64_MAX;
            return;
        }
        slot = (slot + 1) % fs->dir_hash_size;
    }
}

static void veloxfs_hash_update(veloxfs_handle *fs, const char *old_path, uint64_t idx) {
    veloxfs_hash_remove(fs, old_path);
    veloxfs_hash_insert(fs, idx);
}

/* ============================================================================
 * Contiguous block allocator
 * ========================================================================= */
static uint64_t veloxfs_alloc_contiguous(veloxfs_handle *fs, uint64_t wanted,
                                          uint64_t *out_start) {
    uint64_t data_start  = fs->super.data_start;
    uint64_t block_count = fs->super.block_count;
    uint64_t best_start = 0, best_len = 0;
    uint64_t run_start  = 0, run_len  = 0;
    uint64_t hint = fs->last_alloc_hint;
    if (hint < data_start) hint = data_start;

    for (int pass = 0; pass < 2 && best_len < wanted; pass++) {
        uint64_t scan_start = (pass == 0) ? hint        : data_start;
        uint64_t scan_end   = (pass == 0) ? block_count : hint;
        run_len = 0;
        for (uint64_t i = scan_start; i < scan_end; i++) {
            if (fs->fat[i] == veloxfs_FAT_FREE) {
                if (run_len == 0) run_start = i;
                run_len++;
                if (run_len >= wanted) {
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
    if (best_len == 0) { *out_start = 0; return 0; }

    uint64_t alloc = (best_len < wanted) ? best_len : wanted;
    for (uint64_t i = 0; i < alloc; i++)
        fs->fat[best_start + i] = veloxfs_FAT_EOF;
    fs->dirty_fat       = 1;
    fs->last_alloc_hint = best_start + alloc;
    if (fs->last_alloc_hint >= block_count) fs->last_alloc_hint = data_start;
    *out_start = best_start;
    return alloc;
}

/* ============================================================================
 * FAT chain helpers
 * ========================================================================= */
static void veloxfs_free_fat_chain(veloxfs_handle *fs, uint64_t head) {
    while (head != 0 && head < fs->super.block_count) {
        uint64_t next = fs->fat[head];
        fs->fat[head] = veloxfs_FAT_FREE;
        fs->dirty_fat = 1;
        if (next == veloxfs_FAT_EOF || next == veloxfs_FAT_FREE ||
            next == veloxfs_FAT_BAD) break;
        head = next;
    }
}

static uint64_t veloxfs_count_fat_chain(veloxfs_handle *fs, uint64_t head) {
    uint64_t count = 0, limit = fs->super.block_count + 1;
    while (head != 0 && head < fs->super.block_count) {
        count++;
        uint64_t next = fs->fat[head];
        if (next == veloxfs_FAT_EOF || next == veloxfs_FAT_FREE || next == veloxfs_FAT_BAD) break;
        head = next;
        if (count > limit) break;
    }
    return count;
}

static uint64_t veloxfs_fat_get_nth(veloxfs_handle *fs, uint64_t head, uint64_t n) {
    uint64_t b = head;
    for (uint64_t i = 0; i < n; i++) {
        if (b == 0 || b >= fs->super.block_count) return 0;
        uint64_t next = fs->fat[b];
        if (next == veloxfs_FAT_EOF || next == veloxfs_FAT_FREE || next == veloxfs_FAT_BAD) return 0;
        b = next;
    }
    return b;
}

/* ============================================================================
 * Inode block management
 * ========================================================================= */
static uint64_t veloxfs_inode_total_blocks(veloxfs_handle *fs, veloxfs_inode *inode) {
    uint64_t total = 0;
    if (inode->inode_flags & veloxfs_INODE_F_EXTENTS) {
        for (uint64_t i = 0; i < inode->extent_count; i++)
            total += inode->extents[i].block_count;
    }
    if (inode->fat_head != 0)
        total += veloxfs_count_fat_chain(fs, inode->fat_head);
    return total;
}

static uint64_t veloxfs_inode_get_phys(veloxfs_handle *fs, veloxfs_inode *inode,
                                        uint64_t logical_n) {
    if (inode->inode_flags & veloxfs_INODE_F_EXTENTS) {
        uint64_t skipped = 0;
        for (uint64_t i = 0; i < inode->extent_count; i++) {
            veloxfs_extent *e = &inode->extents[i];
            if (logical_n < skipped + e->block_count)
                return e->start_block + (logical_n - skipped);
            skipped += e->block_count;
        }
        if (inode->fat_head != 0)
            return veloxfs_fat_get_nth(fs, inode->fat_head, logical_n - skipped);
        return 0;
    }
    return veloxfs_fat_get_nth(fs, inode->fat_head, logical_n);
}

static int veloxfs_inode_extend(veloxfs_handle *fs, veloxfs_inode *inode,
                                 uint64_t extra_blocks) {
    uint64_t remaining = extra_blocks;

    if ((inode->inode_flags & veloxfs_INODE_F_EXTENTS) && inode->extent_count > 0) {
        veloxfs_extent *last = &inode->extents[inode->extent_count - 1];
        uint64_t next_phys = last->start_block + last->block_count;
        uint64_t grown = 0;
        while (grown < remaining && next_phys + grown < fs->super.block_count &&
               fs->fat[next_phys + grown] == veloxfs_FAT_FREE) {
            fs->fat[next_phys + grown] = veloxfs_FAT_EOF;
            grown++;
        }
        if (grown > 0) {
            last->block_count += grown; remaining -= grown;
            fs->dirty_fat = 1; fs->dirty_inodes = 1;
        }
    }

    while (remaining > 0) {
        uint64_t start = 0;
        uint64_t got   = veloxfs_alloc_contiguous(fs, remaining, &start);
        if (got == 0) return veloxfs_ERR_NO_SPACE;

        if ((inode->inode_flags & veloxfs_INODE_F_EXTENTS) &&
             inode->extent_count < veloxfs_INLINE_EXTENTS) {
            inode->extents[inode->extent_count].start_block = start;
            inode->extents[inode->extent_count].block_count = got;
            inode->extent_count++;
            fs->dirty_inodes = 1;
        } else {
            for (uint64_t i = 0; i + 1 < got; i++)
                fs->fat[start + i] = start + i + 1;
            fs->fat[start + got - 1] = veloxfs_FAT_EOF;
            if (inode->fat_head == 0) {
                inode->fat_head = start;
            } else {
                uint64_t tail = inode->fat_head;
                while (fs->fat[tail] != veloxfs_FAT_EOF &&
                       fs->fat[tail] != veloxfs_FAT_FREE &&
                       fs->fat[tail] < fs->super.block_count)
                    tail = fs->fat[tail];
                fs->fat[tail] = start;
            }
            fs->dirty_fat = 1; fs->dirty_inodes = 1;
        }
        remaining -= got;
    }
    return veloxfs_OK;
}

static void veloxfs_inode_free_all(veloxfs_handle *fs, veloxfs_inode *inode) {
    if (inode->inode_flags & veloxfs_INODE_F_EXTENTS) {
        for (uint64_t i = 0; i < inode->extent_count; i++) {
            veloxfs_extent *e = &inode->extents[i];
            for (uint64_t b = 0; b < e->block_count; b++) {
                uint64_t blk = e->start_block + b;
                if (blk < fs->super.block_count) fs->fat[blk] = veloxfs_FAT_FREE;
            }
        }
        inode->extent_count = 0;
        fs->dirty_fat = 1; fs->dirty_inodes = 1;
    }
    if (inode->fat_head != 0) {
        veloxfs_free_fat_chain(fs, inode->fat_head);
        inode->fat_head = 0; fs->dirty_inodes = 1;
    }
}

static int veloxfs_inode_truncate(veloxfs_handle *fs, veloxfs_inode *inode,
                                   uint64_t keep_blocks) {
    if (keep_blocks == 0) { veloxfs_inode_free_all(fs, inode); return veloxfs_OK; }

    if (inode->inode_flags & veloxfs_INODE_F_EXTENTS) {
        uint64_t left = keep_blocks;
        for (uint64_t i = 0; i < inode->extent_count; ) {
            veloxfs_extent *e = &inode->extents[i];
            if (left == 0) {
                for (uint64_t b = 0; b < e->block_count; b++) {
                    uint64_t blk = e->start_block + b;
                    if (blk < fs->super.block_count) fs->fat[blk] = veloxfs_FAT_FREE;
                }
                for (uint64_t j = i; j + 1 < inode->extent_count; j++)
                    inode->extents[j] = inode->extents[j + 1];
                inode->extent_count--;
            } else if (left < e->block_count) {
                for (uint64_t b = left; b < e->block_count; b++) {
                    uint64_t blk = e->start_block + b;
                    if (blk < fs->super.block_count) fs->fat[blk] = veloxfs_FAT_FREE;
                }
                e->block_count = left; left = 0; i++;
            } else { left -= e->block_count; i++; }
        }
        if (left == 0 && inode->fat_head != 0) {
            veloxfs_free_fat_chain(fs, inode->fat_head); inode->fat_head = 0;
        } else if (left > 0 && inode->fat_head != 0) {
            if (left == 1) {
                veloxfs_free_fat_chain(fs, fs->fat[inode->fat_head]);
                fs->fat[inode->fat_head] = veloxfs_FAT_EOF;
            } else {
                uint64_t block = inode->fat_head;
                for (uint64_t k = 1; k < left && block < fs->super.block_count; k++)
                    block = fs->fat[block];
                if (block < fs->super.block_count) {
                    veloxfs_free_fat_chain(fs, fs->fat[block]);
                    fs->fat[block] = veloxfs_FAT_EOF;
                }
            }
        }
        fs->dirty_fat = 1; fs->dirty_inodes = 1;
    } else {
        uint64_t block = inode->fat_head;
        for (uint64_t i = 1; i < keep_blocks && block < fs->super.block_count; i++) {
            uint64_t next = fs->fat[block];
            if (next == veloxfs_FAT_EOF || next == veloxfs_FAT_FREE) break;
            block = next;
        }
        if (block < fs->super.block_count) {
            veloxfs_free_fat_chain(fs, fs->fat[block]);
            fs->fat[block] = veloxfs_FAT_EOF;
            fs->dirty_fat = 1; fs->dirty_inodes = 1;
        }
    }
    return veloxfs_OK;
}

/* ============================================================================
 * Journal
 * ========================================================================= */
static int veloxfs_journal_log(veloxfs_handle *fs, uint32_t op_type, uint64_t inode_num,
                                uint64_t block_addr, uint64_t old_val, uint64_t new_val) {
    if (!fs->super.journal_enabled) return veloxfs_OK;
    uint32_t idx = fs->next_journal_seq % veloxfs_JOURNAL_SIZE;
    veloxfs_journal_entry *e = &fs->journal[idx];
    e->sequence  = fs->next_journal_seq++;
    e->op_type   = op_type;
    e->inode_num = inode_num;
    e->block_addr = block_addr;
    e->old_value = old_val;
    e->new_value = new_val;
    e->committed = 0;
    e->checksum  = crc32_compute(e, offsetof(veloxfs_journal_entry, checksum));
    veloxfs_MEMSET(fs->block_buf, 0, veloxfs_BS(fs));
    veloxfs_MEMCPY(fs->block_buf, e, sizeof(*e));
    veloxfs_write_block(fs, fs->super.journal_start + idx, fs->block_buf);
    return veloxfs_OK;
}

static int veloxfs_journal_commit(veloxfs_handle *fs) {
    if (!fs->super.journal_enabled) return veloxfs_OK;
    for (uint32_t i = 0; i < veloxfs_JOURNAL_SIZE; i++) {
        if (fs->journal[i].op_type != veloxfs_JOP_NONE && !fs->journal[i].committed) {
            fs->journal[i].committed = 1;
            veloxfs_MEMSET(fs->block_buf, 0, veloxfs_BS(fs));
            veloxfs_MEMCPY(fs->block_buf, &fs->journal[i], sizeof(fs->journal[i]));
            veloxfs_write_block(fs, fs->super.journal_start + i, fs->block_buf);
        }
    }
    return veloxfs_OK;
}

static veloxfs_inode *veloxfs_get_inode(veloxfs_handle *fs, uint64_t inode_num);
static int            veloxfs_flush_inodes(veloxfs_handle *fs);

static int veloxfs_journal_replay(veloxfs_handle *fs) {
    if (!fs->super.journal_enabled) return veloxfs_OK;
    int replayed = 0;
    for (uint32_t i = 0; i < veloxfs_JOURNAL_SIZE; i++) {
        veloxfs_journal_entry *e = &fs->journal[i];
        if (e->op_type == veloxfs_JOP_NONE || e->committed) continue;
        uint32_t expected = e->checksum;
        uint32_t actual   = crc32_compute(e, offsetof(veloxfs_journal_entry, checksum));
        if (expected != actual) continue;
        if (e->op_type == veloxfs_JOP_WRITE) {
            veloxfs_inode *inode = veloxfs_get_inode(fs, e->inode_num);
            if (inode) { inode->size = e->old_value; fs->dirty_inodes = 1; replayed++; }
        }
        veloxfs_MEMSET(e, 0, sizeof(*e));
    }
    if (replayed > 0) {
        veloxfs_LOG("Journal: rolled back %d uncommitted operations\n", replayed);
        veloxfs_flush_inodes(fs);
    }
    return veloxfs_OK;
}

/* ============================================================================
 * Permission checking
 * ========================================================================= */
static int veloxfs_check_permission(veloxfs_handle *fs, veloxfs_inode *inode, int need_write) {
    if (fs->current_uid == 0) return 1;
    uint32_t m = inode->mode;
    int can_r, can_w;
    if (inode->uid == fs->current_uid) {
        can_r = (m & veloxfs_S_IRUSR) != 0; can_w = (m & veloxfs_S_IWUSR) != 0;
    } else if (inode->gid == fs->current_gid) {
        can_r = (m & veloxfs_S_IRGRP) != 0; can_w = (m & veloxfs_S_IWGRP) != 0;
    } else {
        can_r = (m & veloxfs_S_IROTH) != 0; can_w = (m & veloxfs_S_IWOTH) != 0;
    }
    return need_write ? can_w : can_r;
}

/* ============================================================================
 * Inode table
 * ========================================================================= */
static veloxfs_inode *veloxfs_alloc_inode(veloxfs_handle *fs) {
    for (uint64_t i = 0; i < fs->num_inodes; i++) {
        if (fs->inodes[i].inode_num == 0) {
            veloxfs_MEMSET(&fs->inodes[i], 0, sizeof(veloxfs_inode));
            fs->inodes[i].inode_num   = i + 1;
            fs->inodes[i].inode_flags = veloxfs_INODE_F_EXTENTS;
            fs->dirty_inodes = 1;
            return &fs->inodes[i];
        }
    }
    return NULL;
}

static veloxfs_inode *veloxfs_get_inode(veloxfs_handle *fs, uint64_t inode_num) {
    if (inode_num == 0 || inode_num > fs->num_inodes) return NULL;
    veloxfs_inode *in = &fs->inodes[inode_num - 1];
    if (in->inode_num == 0) return NULL;
    return in;
}

static void veloxfs_free_inode(veloxfs_handle *fs, uint64_t inode_num) {
    if (inode_num == 0 || inode_num > fs->num_inodes) return;
    veloxfs_inode *in = &fs->inodes[inode_num - 1];
    veloxfs_inode_free_all(fs, in);
    veloxfs_MEMSET(in, 0, sizeof(*in));
    fs->dirty_inodes = 1;
}

/* ============================================================================
 * Directory
 * ========================================================================= */
static veloxfs_dirent *veloxfs_find_dirent(veloxfs_handle *fs, const char *path) {
    if (fs->dir_hash_table) {
        uint64_t slot = hash_path(path) % fs->dir_hash_size;
        for (uint64_t p = 0; p < fs->dir_hash_size; p++) {
            uint64_t idx = fs->dir_hash_table[slot];
            if (idx == UINT64_MAX) return NULL;
            if (fs->directory[idx].inode_num != 0 &&
                veloxfs_STRCMP(fs->directory[idx].path, path) == 0)
                return &fs->directory[idx];
            slot = (slot + 1) % fs->dir_hash_size;
        }
        return NULL;
    }
    for (uint64_t i = 0; i < fs->num_dirents; i++)
        if (fs->directory[i].inode_num != 0 &&
            veloxfs_STRCMP(fs->directory[i].path, path) == 0)
            return &fs->directory[i];
    return NULL;
}

static veloxfs_dirent *veloxfs_find_free_dirent(veloxfs_handle *fs) {
    for (uint64_t i = 0; i < fs->num_dirents; i++)
        if (fs->directory[i].inode_num == 0) return &fs->directory[i];
    return NULL;
}

/* ============================================================================
 * Flush helpers
 * ========================================================================= */
static int veloxfs_flush_fat(veloxfs_handle *fs) {
    if (!fs->dirty_fat) return veloxfs_OK;
    uint64_t bs = veloxfs_BS(fs);
    uint64_t entries_per = bs / sizeof(uint64_t);
    for (uint64_t i = 0; i < fs->super.fat_blocks; i++) {
        veloxfs_MEMSET(fs->block_buf, 0, bs);
        uint64_t off = i * entries_per;
        uint64_t cnt = entries_per;
        if (off + cnt > fs->super.block_count) cnt = fs->super.block_count - off;
        veloxfs_MEMCPY(fs->block_buf, &fs->fat[off], cnt * sizeof(uint64_t));
        if (veloxfs_write_block(fs, fs->super.fat_start + i, fs->block_buf) != 0)
            return veloxfs_ERR_IO;
    }
    fs->dirty_fat = 0;
    return veloxfs_OK;
}

static int veloxfs_flush_inodes(veloxfs_handle *fs) {
    if (!fs->dirty_inodes) return veloxfs_OK;
    uint64_t bs  = veloxfs_BS(fs);
    uint64_t ipp = bs / sizeof(veloxfs_inode);
    for (uint64_t i = 0; i < fs->super.inode_blocks; i++) {
        uint64_t off = i * ipp, cnt = ipp;
        if (off + cnt > fs->num_inodes) cnt = fs->num_inodes - off;
        veloxfs_MEMSET(fs->block_buf, 0, bs);
        veloxfs_MEMCPY(fs->block_buf, &fs->inodes[off], cnt * sizeof(veloxfs_inode));
        if (veloxfs_write_block(fs, fs->super.inode_start + i, fs->block_buf) != 0)
            return veloxfs_ERR_IO;
    }
    fs->dirty_inodes = 0;
    return veloxfs_OK;
}

static int veloxfs_flush_dir(veloxfs_handle *fs) {
    if (!fs->dirty_dir) return veloxfs_OK;
    uint64_t bs  = veloxfs_BS(fs);
    uint64_t dpp = bs / sizeof(veloxfs_dirent);
    uint64_t dir_start  = fs->super.inode_start + fs->super.inode_blocks;
    uint64_t dir_blocks = calc_dir_blocks(fs->super.block_count, (uint32_t)bs);
    for (uint64_t i = 0; i < dir_blocks; i++) {
        uint64_t off = i * dpp, cnt = dpp;
        if (off + cnt > fs->num_dirents) cnt = fs->num_dirents - off;
        veloxfs_MEMSET(fs->block_buf, 0, bs);
        veloxfs_MEMCPY(fs->block_buf, &fs->directory[off], cnt * sizeof(veloxfs_dirent));
        if (veloxfs_write_block(fs, dir_start + i, fs->block_buf) != 0)
            return veloxfs_ERR_IO;
    }
    fs->dirty_dir = 0;
    return veloxfs_OK;
}

/* ============================================================================
 * FORMAT
 * ========================================================================= */
int veloxfs_format_ex(veloxfs_io io, uint64_t block_count, int enable_journal,
                       uint32_t block_size, uint32_t storage_hint) {
    crc32_init();
    if (block_size < veloxfs_BS_MIN || block_size > veloxfs_BS_MAX) return veloxfs_ERR_INVALID;
    if ((block_size & (block_size - 1)) != 0) return veloxfs_ERR_INVALID;
    if (block_count < 64) return veloxfs_ERR_INVALID;

    veloxfs_superblock super;
    veloxfs_MEMSET(&super, 0, sizeof(super));
    super.magic           = veloxfs_MAGIC_V6;
    super.version         = veloxfs_VERSION;
    super.block_size      = block_size;
    super.block_count     = block_count;
    super.journal_enabled = (uint32_t)enable_journal;
    super.storage_hint    = storage_hint;

    uint64_t cur = 1;
    super.fat_start   = cur;
    super.fat_blocks  = calc_fat_blocks(block_count, block_size);
    cur += super.fat_blocks;
    super.journal_start  = cur;
    super.journal_blocks = enable_journal ? veloxfs_JOURNAL_SIZE : 0;
    cur += super.journal_blocks;
    super.inode_start  = cur;
    super.inode_blocks = calc_inode_blocks(block_count, block_size);
    cur += super.inode_blocks;
    uint64_t dir_blocks = calc_dir_blocks(block_count, block_size);
    cur += dir_blocks;
    super.data_start = cur;

    if (io.write(io.user_data, 0, &super, sizeof(super)) != 0) return veloxfs_ERR_IO;

    uint8_t *buf = (uint8_t*)veloxfs_CALLOC(1, block_size);
    if (!buf) return veloxfs_ERR_IO;

    uint64_t *fat = (uint64_t*)veloxfs_CALLOC(block_count, sizeof(uint64_t));
    if (!fat) { veloxfs_FREE(buf); return veloxfs_ERR_IO; }
    for (uint64_t i = 0; i < super.data_start; i++) fat[i] = veloxfs_FAT_BAD;

    uint64_t epp = block_size / sizeof(uint64_t);
    for (uint64_t i = 0; i < super.fat_blocks; i++) {
        veloxfs_MEMSET(buf, 0, block_size);
        uint64_t off = i * epp, cnt = epp;
        if (off + cnt > block_count) cnt = block_count - off;
        veloxfs_MEMCPY(buf, &fat[off], cnt * sizeof(uint64_t));
        uint64_t byte_off = (super.fat_start + i) * (uint64_t)block_size;
        io.write(io.user_data, byte_off, buf, block_size);
    }
    veloxfs_FREE(fat);

    veloxfs_MEMSET(buf, 0, block_size);
    for (uint64_t i = 0; i < super.inode_blocks; i++) {
        uint64_t byte_off = (super.inode_start + i) * (uint64_t)block_size;
        io.write(io.user_data, byte_off, buf, block_size);
    }
    uint64_t dir_start_blk = super.inode_start + super.inode_blocks;
    for (uint64_t i = 0; i < dir_blocks; i++) {
        uint64_t byte_off = (dir_start_blk + i) * (uint64_t)block_size;
        io.write(io.user_data, byte_off, buf, block_size);
    }
    veloxfs_FREE(buf);
    return veloxfs_OK;
}

int veloxfs_format(veloxfs_io io, uint64_t block_count, int enable_journal) {
    return veloxfs_format_ex(io, block_count, enable_journal,
                              veloxfs_BS_DEFAULT, veloxfs_STORAGE_AUTO);
}

/* ============================================================================
 * MOUNT / UNMOUNT / SYNC
 * ========================================================================= */
int veloxfs_mount(veloxfs_handle *fs, veloxfs_io io) {
    crc32_init();
    veloxfs_MEMSET(fs, 0, sizeof(*fs));
    fs->io = io;

    if (io.read(io.user_data, 0, &fs->super, sizeof(fs->super)) != 0)
        return veloxfs_ERR_IO;
    if (fs->super.magic != veloxfs_MAGIC_V6) return veloxfs_ERR_CORRUPT;
    if (fs->super.block_size < veloxfs_BS_MIN || fs->super.block_size > veloxfs_BS_MAX)
        return veloxfs_ERR_CORRUPT;

    uint32_t bs = fs->super.block_size;

    fs->block_buf = (uint8_t*)veloxfs_MALLOC(bs);
    if (!fs->block_buf) return veloxfs_ERR_IO;

    uint64_t fat_bytes = fs->super.block_count * sizeof(uint64_t);
    fs->fat = (uint64_t*)veloxfs_MALLOC(fat_bytes);
    if (!fs->fat) { veloxfs_FREE(fs->block_buf); return veloxfs_ERR_IO; }

    uint64_t epp = bs / sizeof(uint64_t);
    for (uint64_t i = 0; i < fs->super.fat_blocks; i++) {
        if (veloxfs_read_block(fs, fs->super.fat_start + i, fs->block_buf) != 0)
            goto err_fat;
        uint64_t off = i * epp, cnt = epp;
        if (off + cnt > fs->super.block_count) cnt = fs->super.block_count - off;
        veloxfs_MEMCPY(&fs->fat[off], fs->block_buf, cnt * sizeof(uint64_t));
    }

    {
        uint64_t ipp = bs / sizeof(veloxfs_inode);
        uint64_t inode_bytes = fs->super.inode_blocks * (uint64_t)bs;
        fs->inodes = (veloxfs_inode*)veloxfs_MALLOC(inode_bytes);
        if (!fs->inodes) goto err_fat;
        for (uint64_t i = 0; i < fs->super.inode_blocks; i++) {
            if (veloxfs_read_block(fs, fs->super.inode_start + i, fs->block_buf) != 0)
                goto err_inodes;
            uint64_t off = i * ipp;
            uint64_t cnt = (off + ipp > inode_bytes / sizeof(veloxfs_inode)) ?
                            inode_bytes / sizeof(veloxfs_inode) - off : ipp;
            veloxfs_MEMCPY(&fs->inodes[off], fs->block_buf, cnt * sizeof(veloxfs_inode));
        }
        fs->num_inodes = inode_bytes / sizeof(veloxfs_inode);
    }

    {
        uint64_t dir_blocks = calc_dir_blocks(fs->super.block_count, bs);
        uint64_t dir_bytes  = dir_blocks * (uint64_t)bs;
        fs->directory = (veloxfs_dirent*)veloxfs_MALLOC(dir_bytes);
        if (!fs->directory) goto err_inodes;
        uint64_t dir_start = fs->super.inode_start + fs->super.inode_blocks;
        uint64_t dpp = bs / sizeof(veloxfs_dirent);
        for (uint64_t i = 0; i < dir_blocks; i++) {
            if (veloxfs_read_block(fs, dir_start + i, fs->block_buf) != 0)
                goto err_dir;
            uint64_t off = i * dpp;
            veloxfs_MEMCPY(&fs->directory[off], fs->block_buf, (size_t)bs);
        }
        fs->num_dirents = dir_bytes / sizeof(veloxfs_dirent);
    }

    veloxfs_build_hash_table(fs);

    if (fs->super.journal_enabled) {
        uint64_t jbytes = veloxfs_JOURNAL_SIZE * sizeof(veloxfs_journal_entry);
        fs->journal = (veloxfs_journal_entry*)veloxfs_MALLOC(jbytes);
        if (!fs->journal) goto err_hash;
        for (uint32_t i = 0; i < veloxfs_JOURNAL_SIZE; i++) {
            if (veloxfs_read_block(fs, fs->super.journal_start + i, fs->block_buf) != 0)
                goto err_journal;
            veloxfs_MEMCPY(&fs->journal[i], fs->block_buf, sizeof(veloxfs_journal_entry));
        }
        veloxfs_journal_replay(fs);
    }

    fs->last_alloc_hint = fs->super.data_start;
    return veloxfs_OK;

err_journal: veloxfs_FREE(fs->journal);
err_hash:    if (fs->dir_hash_table) veloxfs_FREE(fs->dir_hash_table);
err_dir:     veloxfs_FREE(fs->directory);
err_inodes:  veloxfs_FREE(fs->inodes);
err_fat:     veloxfs_FREE(fs->fat); veloxfs_FREE(fs->block_buf);
    return veloxfs_ERR_IO;
}

int veloxfs_unmount(veloxfs_handle *fs) {
    if (!fs) return veloxfs_OK;
    veloxfs_sync(fs);
    if (fs->fat)           { veloxfs_FREE(fs->fat);           fs->fat           = NULL; }
    if (fs->inodes)        { veloxfs_FREE(fs->inodes);        fs->inodes        = NULL; }
    if (fs->directory)     { veloxfs_FREE(fs->directory);     fs->directory     = NULL; }
    if (fs->dir_hash_table){ veloxfs_FREE(fs->dir_hash_table);fs->dir_hash_table= NULL; }
    if (fs->journal)       { veloxfs_FREE(fs->journal);       fs->journal       = NULL; }
    if (fs->block_buf)     { veloxfs_FREE(fs->block_buf);     fs->block_buf     = NULL; }
    return veloxfs_OK;
}

int veloxfs_sync(veloxfs_handle *fs) {
    if (!fs) return veloxfs_ERR_INVALID;
    int ret = veloxfs_OK;
    if (veloxfs_flush_fat(fs)    != veloxfs_OK) ret = veloxfs_ERR_IO;
    if (veloxfs_flush_inodes(fs) != veloxfs_OK) ret = veloxfs_ERR_IO;
    if (veloxfs_flush_dir(fs)    != veloxfs_OK) ret = veloxfs_ERR_IO;
    if (fs->super.journal_enabled) veloxfs_journal_commit(fs);
    return ret;
}

void veloxfs_set_user(veloxfs_handle *fs, uint32_t uid, uint32_t gid)
    { fs->current_uid = uid; fs->current_gid = gid; }
void veloxfs_get_user(veloxfs_handle *fs, uint32_t *uid, uint32_t *gid)
    { if (uid) *uid = fs->current_uid; if (gid) *gid = fs->current_gid; }

/* ============================================================================
 * FILE METADATA OPERATIONS
 * ========================================================================= */
int veloxfs_create(veloxfs_handle *fs, const char *path, uint32_t mode) {
    char norm[veloxfs_MAX_PATH];
    normalize_path(path, norm, sizeof(norm));
    if (veloxfs_find_dirent(fs, norm)) return veloxfs_ERR_EXISTS;
    veloxfs_inode *inode = veloxfs_alloc_inode(fs);
    if (!inode) return veloxfs_ERR_TOO_MANY_FILES;
    inode->mode       = mode;
    inode->uid        = fs->current_uid;
    inode->gid        = fs->current_gid;
    inode->inode_flags = veloxfs_INODE_F_EXTENTS;
    inode->ctime = inode->mtime = inode->atime = (uint64_t)veloxfs_TIME();
    inode->size  = 0;
    veloxfs_dirent *d = veloxfs_find_free_dirent(fs);
    if (!d) { veloxfs_free_inode(fs, inode->inode_num); return veloxfs_ERR_TOO_MANY_FILES; }
    veloxfs_STRNCPY(d->path, norm, veloxfs_MAX_PATH - 1);
    d->path[veloxfs_MAX_PATH - 1] = '\0';
    d->inode_num = inode->inode_num;
    fs->dirty_dir = 1;
    for (uint64_t i = 0; i < fs->num_dirents; i++)
        if (&fs->directory[i] == d) { veloxfs_hash_insert(fs, i); break; }
    if (fs->super.journal_enabled)
        veloxfs_journal_log(fs, veloxfs_JOP_CREATE, inode->inode_num, 0, 0, 0);
    return veloxfs_OK;
}

int veloxfs_delete(veloxfs_handle *fs, const char *path) {
    char norm[veloxfs_MAX_PATH];
    normalize_path(path, norm, sizeof(norm));
    veloxfs_dirent *d = veloxfs_find_dirent(fs, norm);
    if (!d) return veloxfs_ERR_NOT_FOUND;
    veloxfs_inode *in = veloxfs_get_inode(fs, d->inode_num);
    if (!in) return veloxfs_ERR_CORRUPT;
    if (!veloxfs_check_permission(fs, in, 1)) return veloxfs_ERR_PERMISSION;
    if (fs->super.journal_enabled)
        veloxfs_journal_log(fs, veloxfs_JOP_DELETE, in->inode_num, 0, 0, 0);
    veloxfs_free_inode(fs, d->inode_num);
    veloxfs_hash_remove(fs, norm);
    veloxfs_MEMSET(d, 0, sizeof(*d));
    fs->dirty_dir = 1;
    if (fs->super.journal_enabled) veloxfs_journal_commit(fs);
    return veloxfs_OK;
}

int veloxfs_rename(veloxfs_handle *fs, const char *old_path, const char *new_path) {
    char old_n[veloxfs_MAX_PATH], new_n[veloxfs_MAX_PATH];
    normalize_path(old_path, old_n, sizeof(old_n));
    normalize_path(new_path, new_n, sizeof(new_n));
    veloxfs_dirent *d = veloxfs_find_dirent(fs, old_n);
    if (!d) return veloxfs_ERR_NOT_FOUND;
    if (veloxfs_find_dirent(fs, new_n)) return veloxfs_ERR_EXISTS;
    veloxfs_inode *in = veloxfs_get_inode(fs, d->inode_num);
    if (!in) return veloxfs_ERR_CORRUPT;
    if (!veloxfs_check_permission(fs, in, 1)) return veloxfs_ERR_PERMISSION;
    veloxfs_STRNCPY(d->path, new_n, veloxfs_MAX_PATH - 1);
    d->path[veloxfs_MAX_PATH - 1] = '\0';
    fs->dirty_dir = 1;
    for (uint64_t i = 0; i < fs->num_dirents; i++)
        if (&fs->directory[i] == d) { veloxfs_hash_update(fs, old_n, i); break; }
    return veloxfs_OK;
}

int veloxfs_chmod(veloxfs_handle *fs, const char *path, uint32_t mode) {
    char norm[veloxfs_MAX_PATH]; normalize_path(path, norm, sizeof(norm));
    veloxfs_dirent *d = veloxfs_find_dirent(fs, norm);
    if (!d) return veloxfs_ERR_NOT_FOUND;
    veloxfs_inode *in = veloxfs_get_inode(fs, d->inode_num);
    if (!in) return veloxfs_ERR_CORRUPT;
    if (fs->current_uid != 0 && fs->current_uid != in->uid) return veloxfs_ERR_PERMISSION;
    if (fs->super.journal_enabled)
        veloxfs_journal_log(fs, veloxfs_JOP_CHMOD, in->inode_num, 0, in->mode, mode);
    in->mode = mode; fs->dirty_inodes = 1;
    if (fs->super.journal_enabled) veloxfs_journal_commit(fs);
    return veloxfs_OK;
}

int veloxfs_chown(veloxfs_handle *fs, const char *path, uint32_t uid, uint32_t gid) {
    char norm[veloxfs_MAX_PATH]; normalize_path(path, norm, sizeof(norm));
    veloxfs_dirent *d = veloxfs_find_dirent(fs, norm);
    if (!d) return veloxfs_ERR_NOT_FOUND;
    veloxfs_inode *in = veloxfs_get_inode(fs, d->inode_num);
    if (!in) return veloxfs_ERR_CORRUPT;
    if (fs->current_uid != 0) return veloxfs_ERR_PERMISSION;
    if (fs->super.journal_enabled)
        veloxfs_journal_log(fs, veloxfs_JOP_CHOWN, in->inode_num, 0,
                            ((uint64_t)in->uid << 32) | in->gid,
                            ((uint64_t)uid     << 32) | gid);
    in->uid = uid; in->gid = gid; fs->dirty_inodes = 1;
    if (fs->super.journal_enabled) veloxfs_journal_commit(fs);
    return veloxfs_OK;
}

/* ============================================================================
 * BULK FILE I/O
 * ========================================================================= */
int veloxfs_write_file(veloxfs_handle *fs, const char *path,
                        const void *data, uint64_t size) {
    char norm[veloxfs_MAX_PATH]; normalize_path(path, norm, sizeof(norm));
    veloxfs_dirent *d = veloxfs_find_dirent(fs, norm);
    if (!d) return veloxfs_ERR_NOT_FOUND;
    veloxfs_inode *in = veloxfs_get_inode(fs, d->inode_num);
    if (!in) return veloxfs_ERR_CORRUPT;
    if (!veloxfs_check_permission(fs, in, 1)) return veloxfs_ERR_PERMISSION;

    uint64_t bs = veloxfs_BS(fs);
    uint64_t blocks_needed = (size + bs - 1) / bs;
    uint64_t blocks_have   = veloxfs_inode_total_blocks(fs, in);

    if (fs->super.journal_enabled)
        veloxfs_journal_log(fs, veloxfs_JOP_WRITE, in->inode_num, 0, in->size, size);

    if (blocks_needed > blocks_have) {
        int ret = veloxfs_inode_extend(fs, in, blocks_needed - blocks_have);
        if (ret != veloxfs_OK) return ret;
    } else if (blocks_needed < blocks_have) {
        veloxfs_inode_truncate(fs, in, blocks_needed);
    }

    const uint8_t *src = (const uint8_t*)data;
    uint64_t remaining = size;

    if (in->inode_flags & veloxfs_INODE_F_EXTENTS) {
        for (uint64_t ei = 0; ei < in->extent_count && remaining > 0; ei++) {
            veloxfs_extent *e = &in->extents[ei];
            uint64_t ext_bytes = e->block_count * bs;
            uint64_t to_write  = (ext_bytes < remaining) ? ext_bytes : remaining;
            uint64_t full_blocks = to_write / bs;
            if (full_blocks > 0) {
                if (veloxfs_write_blocks(fs, e->start_block, src, full_blocks) != 0)
                    return veloxfs_ERR_IO;
                src += full_blocks * bs; remaining -= full_blocks * bs;
            }
            if (remaining > 0 && remaining < bs && ei == in->extent_count - 1) {
                veloxfs_MEMSET(fs->block_buf, 0, bs);
                veloxfs_MEMCPY(fs->block_buf, src, remaining);
                if (veloxfs_write_block(fs, e->start_block + full_blocks, fs->block_buf) != 0)
                    return veloxfs_ERR_IO;
                remaining = 0;
            }
        }
    } else {
        uint64_t block = in->fat_head;
        while (remaining > 0 && block != 0 && block < fs->super.block_count) {
            uint64_t chunk = (remaining < bs) ? remaining : bs;
            veloxfs_MEMSET(fs->block_buf, 0, bs);
            veloxfs_MEMCPY(fs->block_buf, src, chunk);
            if (veloxfs_write_block(fs, block, fs->block_buf) != 0) return veloxfs_ERR_IO;
            src += chunk; remaining -= chunk;
            uint64_t next = fs->fat[block];
            if (next == veloxfs_FAT_EOF || next == veloxfs_FAT_FREE) break;
            block = next;
        }
    }

    in->size = size; in->mtime = (uint64_t)veloxfs_TIME(); fs->dirty_inodes = 1;
    if (fs->super.journal_enabled) veloxfs_journal_commit(fs);
    return veloxfs_OK;
}

int veloxfs_read_file(veloxfs_handle *fs, const char *path,
                       void *out, uint64_t max_size, uint64_t *out_size) {
    char norm[veloxfs_MAX_PATH]; normalize_path(path, norm, sizeof(norm));
    veloxfs_dirent *d = veloxfs_find_dirent(fs, norm);
    if (!d) return veloxfs_ERR_NOT_FOUND;
    veloxfs_inode *in = veloxfs_get_inode(fs, d->inode_num);
    if (!in) return veloxfs_ERR_CORRUPT;
    if (!veloxfs_check_permission(fs, in, 0)) return veloxfs_ERR_PERMISSION;

    uint64_t bs   = veloxfs_BS(fs);
    uint64_t size = (in->size < max_size) ? in->size : max_size;
    if (out_size) *out_size = size;
    if (size == 0) return veloxfs_OK;

    uint8_t *dest = (uint8_t*)out;
    uint64_t remaining = size;

    if (in->inode_flags & veloxfs_INODE_F_EXTENTS) {
        for (uint64_t ei = 0; ei < in->extent_count && remaining > 0; ei++) {
            veloxfs_extent *e = &in->extents[ei];
            uint64_t ext_bytes  = e->block_count * bs;
            uint64_t to_read    = (ext_bytes < remaining) ? ext_bytes : remaining;
            uint64_t full_blocks = to_read / bs;
            if (full_blocks > 0) {
                if (veloxfs_read_blocks(fs, e->start_block, dest, full_blocks) != 0)
                    return veloxfs_ERR_IO;
                dest += full_blocks * bs; remaining -= full_blocks * bs;
            }
            if (remaining > 0 && remaining < bs) {
                if (veloxfs_read_block(fs, e->start_block + full_blocks, fs->block_buf) != 0)
                    return veloxfs_ERR_IO;
                veloxfs_MEMCPY(dest, fs->block_buf, remaining);
                remaining = 0;
            }
        }
    } else {
        uint64_t block = in->fat_head;
        while (remaining > 0 && block != 0 && block < fs->super.block_count) {
            uint64_t chunk = (remaining < bs) ? remaining : bs;
            if (veloxfs_read_block(fs, block, fs->block_buf) != 0) return veloxfs_ERR_IO;
            veloxfs_MEMCPY(dest, fs->block_buf, chunk);
            dest += chunk; remaining -= chunk;
            uint64_t next = fs->fat[block];
            if (next == veloxfs_FAT_EOF || next == veloxfs_FAT_FREE) break;
            block = next;
        }
    }

    in->atime = (uint64_t)veloxfs_TIME(); fs->dirty_inodes = 1;
    return veloxfs_OK;
}

/* ============================================================================
 * STREAMING FILE HANDLE I/O  (open/close/read/write/seek/tell/truncate)
 * ========================================================================= */
int veloxfs_open(veloxfs_handle *fs, const char *path, int flags, veloxfs_file *file) {
    char norm[veloxfs_MAX_PATH]; normalize_path(path, norm, sizeof(norm));
    veloxfs_dirent *d = veloxfs_find_dirent(fs, norm);
    if (!d) return veloxfs_ERR_NOT_FOUND;
    veloxfs_inode *in = veloxfs_get_inode(fs, d->inode_num);
    if (!in) return veloxfs_ERR_CORRUPT;
    int need_write = (flags & veloxfs_O_WRONLY) || (flags & veloxfs_O_RDWR);
    if (!veloxfs_check_permission(fs, in, need_write)) return veloxfs_ERR_PERMISSION;
    veloxfs_MEMSET(file, 0, sizeof(*file));
    file->fs = fs; file->inode = in; file->is_open = 1;
    file->can_read  = (flags & veloxfs_O_RDONLY) || (flags & veloxfs_O_RDWR);
    file->can_write = need_write;
    return veloxfs_OK;
}

int veloxfs_close(veloxfs_file *file) {
    if (!file || !file->is_open) return veloxfs_ERR_INVALID;
    if (file->modified) { file->inode->mtime = (uint64_t)veloxfs_TIME(); file->fs->dirty_inodes = 1; }
    veloxfs_MEMSET(file, 0, sizeof(*file));
    return veloxfs_OK;
}

int veloxfs_read(veloxfs_file *file, void *buf, uint64_t count, uint64_t *bytes_read) {
    if (!file || !file->is_open || !file->can_read) return veloxfs_ERR_INVALID;
    veloxfs_inode  *in = file->inode;
    veloxfs_handle *fs = file->fs;
    uint64_t        bs = veloxfs_BS(fs);
    uint64_t avail = (file->position < in->size) ? in->size - file->position : 0;
    uint64_t to_read = (count < avail) ? count : avail;
    if (to_read == 0) { if (bytes_read) *bytes_read = 0; return veloxfs_OK; }

    uint8_t *dest = (uint8_t*)buf;
    uint64_t done = 0, pos = file->position;

    if (in->inode_flags & veloxfs_INODE_F_EXTENTS) {
        uint64_t logical_block = pos / bs, block_off = pos % bs, skipped = 0;
        uint64_t start_ei = 0;
        if (file->cur_extent_idx < in->extent_count &&
            file->cur_extent_logical_start <= logical_block) {
            veloxfs_extent *ce = &in->extents[file->cur_extent_idx];
            if (logical_block < file->cur_extent_logical_start + ce->block_count) {
                start_ei = file->cur_extent_idx;
                skipped  = file->cur_extent_logical_start;
            }
        }
        for (uint64_t ei = start_ei; ei < in->extent_count && done < to_read; ei++) {
            veloxfs_extent *e = &in->extents[ei];
            if (skipped + e->block_count <= logical_block) { skipped += e->block_count; continue; }
            file->cur_extent_idx = ei; file->cur_extent_logical_start = skipped;
            uint64_t ext_block_off = logical_block - skipped;
            uint64_t phys_block    = e->start_block + ext_block_off;
            uint64_t bytes_in_ext  = (e->block_count - ext_block_off) * bs - block_off;
            uint64_t chunk         = (bytes_in_ext < (to_read - done)) ? bytes_in_ext : (to_read - done);
            uint64_t phys_off = phys_block * bs + block_off;
            if (fs->io.read(fs->io.user_data, phys_off, dest + done, (uint32_t)chunk) != 0)
                return veloxfs_ERR_IO;
            done += chunk; logical_block = skipped + e->block_count; block_off = 0; skipped = logical_block;
        }
    } else {
        uint64_t target_idx = pos / bs;
        if (target_idx != file->fat_block_idx || (file->fat_block == 0 && in->fat_head != 0)) {
            file->fat_block = in->fat_head; file->fat_block_idx = 0;
            for (uint64_t i = 0; i < target_idx && file->fat_block != 0; i++) {
                uint64_t nx = fs->fat[file->fat_block];
                if (nx == veloxfs_FAT_EOF || nx == veloxfs_FAT_FREE) break;
                file->fat_block = nx; file->fat_block_idx++;
            }
        }
        while (done < to_read && file->fat_block != 0) {
            if (veloxfs_read_block(fs, file->fat_block, fs->block_buf) != 0) return veloxfs_ERR_IO;
            uint64_t off_in = (pos + done) % bs;
            uint64_t chunk  = bs - off_in;
            if (chunk > to_read - done) chunk = to_read - done;
            veloxfs_MEMCPY(dest + done, fs->block_buf + off_in, chunk);
            done += chunk;
            if (((pos + done) % bs) == 0) {
                uint64_t nx = fs->fat[file->fat_block];
                if (nx == veloxfs_FAT_EOF || nx == veloxfs_FAT_FREE) break;
                file->fat_block = nx; file->fat_block_idx++;
            }
        }
    }
    file->position += done;
    if (bytes_read) *bytes_read = done;
    in->atime = (uint64_t)veloxfs_TIME(); fs->dirty_inodes = 1;
    return veloxfs_OK;
}

int veloxfs_write(veloxfs_file *file, const void *buf, uint64_t count) {
    if (!file || !file->is_open || !file->can_write) return veloxfs_ERR_INVALID;
    veloxfs_inode  *in = file->inode;
    veloxfs_handle *fs = file->fs;
    uint64_t        bs = veloxfs_BS(fs);
    uint64_t end_pos = file->position + count;
    if (end_pos > in->size) {
        uint64_t blocks_needed = (end_pos + bs - 1) / bs;
        uint64_t blocks_have   = veloxfs_inode_total_blocks(fs, in);
        if (blocks_needed > blocks_have) {
            int ret = veloxfs_inode_extend(fs, in, blocks_needed - blocks_have);
            if (ret != veloxfs_OK) return ret;
        }
        if (fs->super.journal_enabled)
            veloxfs_journal_log(fs, veloxfs_JOP_WRITE, in->inode_num, 0, in->size, end_pos);
        in->size = end_pos;
    }
    const uint8_t *src = (const uint8_t*)buf;
    uint64_t done = 0, pos = file->position;

    if (in->inode_flags & veloxfs_INODE_F_EXTENTS) {
        uint64_t logical_block = pos / bs, block_off = pos % bs, skipped = 0;
        uint64_t start_ei = 0;
        if (file->cur_extent_idx < in->extent_count &&
            file->cur_extent_logical_start <= logical_block) {
            veloxfs_extent *ce = &in->extents[file->cur_extent_idx];
            if (logical_block < file->cur_extent_logical_start + ce->block_count) {
                start_ei = file->cur_extent_idx; skipped = file->cur_extent_logical_start;
            }
        }
        for (uint64_t ei = start_ei; ei < in->extent_count && done < count; ei++) {
            veloxfs_extent *e = &in->extents[ei];
            if (skipped + e->block_count <= logical_block) { skipped += e->block_count; continue; }
            file->cur_extent_idx = ei; file->cur_extent_logical_start = skipped;
            uint64_t ext_block_off = logical_block - skipped;
            uint64_t phys_block    = e->start_block + ext_block_off;
            uint64_t bytes_in_ext  = (e->block_count - ext_block_off) * bs - block_off;
            uint64_t chunk         = (bytes_in_ext < (count - done)) ? bytes_in_ext : (count - done);
            uint64_t written_in_chunk = 0;
            if (block_off != 0) {
                uint64_t first_chunk = bs - block_off;
                if (first_chunk > chunk) first_chunk = chunk;
                if (veloxfs_read_block(fs, phys_block, fs->block_buf) != 0) return veloxfs_ERR_IO;
                veloxfs_MEMCPY(fs->block_buf + block_off, src + done, first_chunk);
                if (veloxfs_write_block(fs, phys_block, fs->block_buf) != 0) return veloxfs_ERR_IO;
                written_in_chunk += first_chunk; phys_block++;
            }
            uint64_t full_remaining = chunk - written_in_chunk;
            uint64_t full_blocks    = full_remaining / bs;
            if (full_blocks > 0) {
                uint64_t phys_off = phys_block * bs;
                if (fs->io.write(fs->io.user_data, phys_off, src + done + written_in_chunk,
                                  (uint32_t)(full_blocks * bs)) != 0) return veloxfs_ERR_IO;
                written_in_chunk += full_blocks * bs; phys_block += full_blocks;
            }
            uint64_t tail = chunk - written_in_chunk;
            if (tail > 0) {
                if (veloxfs_read_block(fs, phys_block, fs->block_buf) != 0) return veloxfs_ERR_IO;
                veloxfs_MEMCPY(fs->block_buf, src + done + written_in_chunk, tail);
                if (veloxfs_write_block(fs, phys_block, fs->block_buf) != 0) return veloxfs_ERR_IO;
            }
            done += chunk; logical_block = skipped + e->block_count; block_off = 0; skipped = logical_block;
        }
    } else {
        uint64_t target_idx = pos / bs;
        if (target_idx != file->fat_block_idx || (file->fat_block == 0 && in->fat_head != 0)) {
            file->fat_block = in->fat_head; file->fat_block_idx = 0;
            for (uint64_t i = 0; i < target_idx && file->fat_block != 0; i++) {
                uint64_t nx = fs->fat[file->fat_block];
                if (nx == veloxfs_FAT_EOF || nx == veloxfs_FAT_FREE) break;
                file->fat_block = nx; file->fat_block_idx++;
            }
        }
        while (done < count && file->fat_block != 0 && file->fat_block < fs->super.block_count) {
            uint64_t off_in = (pos + done) % bs;
            uint64_t chunk  = bs - off_in;
            if (chunk > count - done) chunk = count - done;
            if (off_in != 0 || chunk < bs)
                if (veloxfs_read_block(fs, file->fat_block, fs->block_buf) != 0) return veloxfs_ERR_IO;
            veloxfs_MEMCPY(fs->block_buf + off_in, src + done, chunk);
            if (veloxfs_write_block(fs, file->fat_block, fs->block_buf) != 0) return veloxfs_ERR_IO;
            done += chunk;
            if (((pos + done) % bs) == 0) {
                uint64_t nx = fs->fat[file->fat_block];
                if (nx == veloxfs_FAT_EOF || nx == veloxfs_FAT_FREE) break;
                file->fat_block = nx; file->fat_block_idx++;
            }
        }
    }
    file->position += done; file->modified = 1;
    in->mtime = (uint64_t)veloxfs_TIME(); fs->dirty_inodes = 1;
    if (fs->super.journal_enabled) veloxfs_journal_commit(fs);
    return veloxfs_OK;
}

int veloxfs_seek(veloxfs_file *file, int64_t offset, int whence) {
    if (!file || !file->is_open) return veloxfs_ERR_INVALID;
    int64_t new_pos;
    switch (whence) {
        case 0: new_pos = offset; break;
        case 1: new_pos = (int64_t)file->position + offset; break;
        case 2: new_pos = (int64_t)file->inode->size + offset; break;
        default: return veloxfs_ERR_INVALID;
    }
    if (new_pos < 0) return veloxfs_ERR_INVALID;
    if ((uint64_t)new_pos > file->inode->size && !file->can_write) return veloxfs_ERR_INVALID;
    file->position = (uint64_t)new_pos;
    file->cur_extent_idx = 0; file->cur_extent_logical_start = 0;
    file->fat_block = 0; file->fat_block_idx = 0;
    return veloxfs_OK;
}

uint64_t veloxfs_tell(veloxfs_file *file) {
    if (!file || !file->is_open) return 0;
    return file->position;
}

int veloxfs_truncate_handle(veloxfs_file *file, uint64_t new_size) {
    if (!file || !file->is_open || !file->can_write) return veloxfs_ERR_INVALID;
    veloxfs_inode  *in = file->inode;
    veloxfs_handle *fs = file->fs;
    uint64_t bs = veloxfs_BS(fs);
    uint64_t blocks_needed = (new_size + bs - 1) / bs;
    uint64_t blocks_have   = veloxfs_inode_total_blocks(fs, in);
    if (blocks_needed > blocks_have) {
        int ret = veloxfs_inode_extend(fs, in, blocks_needed - blocks_have);
        if (ret != veloxfs_OK) return ret;
    } else if (blocks_needed < blocks_have) {
        veloxfs_inode_truncate(fs, in, blocks_needed);
    }
    in->size = new_size; file->modified = 1; fs->dirty_inodes = 1;
    if (file->position > new_size) file->position = new_size;
    file->cur_extent_idx = 0; file->cur_extent_logical_start = 0;
    file->fat_block = 0; file->fat_block_idx = 0;
    return veloxfs_OK;
}

/* ============================================================================
 * DIRECTORY OPERATIONS
 * ========================================================================= */
int veloxfs_mkdir(veloxfs_handle *fs, const char *path, uint32_t mode) {
    return veloxfs_create(fs, path, mode | veloxfs_S_IRWXU);
}

int veloxfs_list(veloxfs_handle *fs, const char *path,
                  veloxfs_list_callback callback, void *user_data) {
    char norm[veloxfs_MAX_PATH];
    normalize_path(path, norm, sizeof(norm));
    size_t plen = veloxfs_STRLEN(norm);
    if (plen > 0 && norm[plen - 1] != '/') {
        if (plen + 1 < sizeof(norm)) { norm[plen] = '/'; norm[plen+1] = '\0'; plen++; }
    }
    for (uint64_t i = 0; i < fs->num_dirents; i++) {
        if (fs->directory[i].inode_num == 0) continue;
        const char *ep = fs->directory[i].path;
        if (veloxfs_STRCMP(norm, "/") != 0 && veloxfs_STRNCMP(ep, norm, plen) != 0) continue;
        veloxfs_inode *in = veloxfs_get_inode(fs, fs->directory[i].inode_num);
        if (!in) continue;
        veloxfs_stat_t st = {
            .size        = in->size,
            .uid         = in->uid,
            .gid         = in->gid,
            .mode        = in->mode,
            .block_count = veloxfs_inode_total_blocks(fs, in),
            .ctime       = in->ctime,
            .mtime       = in->mtime,
            .atime       = in->atime,
        };
        callback(ep, &st, 0, user_data);
    }
    return veloxfs_OK;
}

/* ============================================================================
 * STAT / STATFS
 * ========================================================================= */
int veloxfs_stat(veloxfs_handle *fs, const char *path, veloxfs_stat_t *stat) {
    char norm[veloxfs_MAX_PATH]; normalize_path(path, norm, sizeof(norm));
    veloxfs_dirent *d = veloxfs_find_dirent(fs, norm);
    if (!d) return veloxfs_ERR_NOT_FOUND;
    veloxfs_inode *in = veloxfs_get_inode(fs, d->inode_num);
    if (!in) return veloxfs_ERR_CORRUPT;
    stat->size        = in->size;
    stat->uid         = in->uid;
    stat->gid         = in->gid;
    stat->mode        = in->mode;
    stat->block_count = veloxfs_inode_total_blocks(fs, in);
    stat->ctime       = in->ctime;
    stat->mtime       = in->mtime;
    stat->atime       = in->atime;
    return veloxfs_OK;
}

int veloxfs_statfs(veloxfs_handle *fs, uint64_t *total, uint64_t *used, uint64_t *free_blocks) {
    uint64_t u = 0;
    for (uint64_t i = 0; i < fs->super.block_count; i++)
        if (fs->fat[i] != veloxfs_FAT_FREE) u++;
    if (total)       *total       = fs->super.block_count;
    if (used)        *used        = u;
    if (free_blocks) *free_blocks = fs->super.block_count - u;
    return veloxfs_OK;
}

int veloxfs_alloc_stats_get(veloxfs_handle *fs, veloxfs_alloc_stats *stats) {
    veloxfs_MEMSET(stats, 0, sizeof(*stats));
    uint64_t u = 0;
    for (uint64_t i = 0; i < fs->super.block_count; i++)
        if (fs->fat[i] != veloxfs_FAT_FREE) u++;
    stats->total_blocks = fs->super.block_count;
    stats->used_blocks  = u;
    stats->free_blocks  = fs->super.block_count - u;
    uint64_t ext_count = 0, overflow_count = 0, total_extents = 0;
    for (uint64_t i = 0; i < fs->num_inodes; i++) {
        veloxfs_inode *in = &fs->inodes[i];
        if (in->inode_num == 0) continue;
        if (in->inode_flags & veloxfs_INODE_F_EXTENTS) {
            ext_count++; total_extents += in->extent_count;
            if (in->fat_head != 0) overflow_count++;
        }
    }
    stats->extent_files = ext_count;
    stats->fat_overflow = overflow_count;
    stats->avg_extents  = (ext_count > 0) ? (float)total_extents / ext_count : 0.0f;
    return veloxfs_OK;
}

/* ============================================================================
 * FSCK
 * ========================================================================= */
int veloxfs_fsck(veloxfs_handle *fs) {
    int errors = 0;
    uint8_t *used = (uint8_t*)veloxfs_CALLOC(fs->super.block_count, 1);
    if (!used) return veloxfs_ERR_IO;
    for (uint64_t i = 0; i < fs->super.data_start; i++) used[i] = 1;

    for (uint64_t i = 0; i < fs->num_inodes; i++) {
        veloxfs_inode *in = &fs->inodes[i];
        if (in->inode_num == 0) continue;
        if (in->inode_flags & veloxfs_INODE_F_EXTENTS) {
            for (uint64_t ei = 0; ei < in->extent_count; ei++) {
                veloxfs_extent *e = &in->extents[ei];
                for (uint64_t b = 0; b < e->block_count; b++) {
                    uint64_t blk = e->start_block + b;
                    if (blk >= fs->super.block_count) {
                        veloxfs_LOG("fsck: inode %llu extent %llu block out of range\n",
                               (unsigned long long)in->inode_num, (unsigned long long)ei);
                        errors++; continue;
                    }
                    if (used[blk]) {
                        veloxfs_LOG("fsck: block %llu used by multiple files\n",
                               (unsigned long long)blk);
                        errors++;
                    }
                    used[blk] = 1;
                }
            }
        }
        uint64_t blk = in->fat_head, safety = 0;
        while (blk != 0 && blk < fs->super.block_count) {
            if (used[blk]) {
                veloxfs_LOG("fsck: block %llu shared in FAT chain\n", (unsigned long long)blk);
                errors++;
            }
            used[blk] = 1;
            uint64_t nx = fs->fat[blk];
            if (nx == veloxfs_FAT_EOF || nx == veloxfs_FAT_FREE || nx == veloxfs_FAT_BAD) break;
            blk = nx;
            if (++safety > fs->super.block_count) {
                veloxfs_LOG("fsck: FAT loop detected for inode %llu\n",
                       (unsigned long long)in->inode_num);
                errors++; break;
            }
        }
    }

    int orphaned = 0;
    for (uint64_t i = fs->super.data_start; i < fs->super.block_count; i++) {
        if (fs->fat[i] != veloxfs_FAT_FREE && !used[i]) {
            fs->fat[i] = veloxfs_FAT_FREE; orphaned++; fs->dirty_fat = 1;
        }
    }
    veloxfs_FREE(used);

    if (orphaned > 0) {
        veloxfs_LOG("fsck: freed %d orphaned blocks\n", orphaned);
        veloxfs_flush_fat(fs);
    }
    if (errors == 0 && orphaned == 0)
        veloxfs_LOG("fsck: filesystem is clean (v6 extent-mode)\n");

    return (errors == 0) ? veloxfs_OK : veloxfs_ERR_CORRUPT;
}

#endif /* veloxfs_IMPLEMENTATION */

/*
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2026 Maxwell Wingate

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
------------------------------------------------------------------------------
*/