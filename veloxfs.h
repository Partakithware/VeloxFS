/* veloxfs.h - v5.0 - Maxwell Wingate - https://github.com/Partakithware/VeloxFS/
   
   A single-header FAT-style filesystem library with journaling.
   See end of file for license information (MIT or Public Domain).

 * v5.0 - Linked-list allocation for maximum robustness
 * 
 * MAJOR CHANGES FROM v4.0:
 * - Replaced extent-based allocation with FAT-style linked lists
 * - Safe file growth without relocation
 * - Fragmentation tolerance
 * - No more overwrite bugs from contiguous allocation
 * 
 * WHY THIS IS BETTER:
 * - Files can grow safely (no relocation needed)
 * - No overwrites from fragmentation
 * - Minimal structural changes (just inode.first_block + FAT chains)
 * - Proven approach (FAT survived for decades)
 * 
 * TRADE-OFFS:
 * - 8 bytes overhead per block vs extents
 * - Slightly slower traversal for large files
 * - BUT: Maximum correctness and safety
 * 
 * USAGE:
 *   #define veloxfs_IMPLEMENTATION
 *   #include "veloxfs.h"
 * 
 * LICENSE: Public domain or MIT
 */

#ifndef veloxfs_H
#define veloxfs_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cross-platform packing */
#if defined(_MSC_VER)
    #define veloxfs_PACKED_START __pragma(pack(push, 1))
    #define veloxfs_PACKED_END   __pragma(pack(pop))
    #define veloxfs_ATTR_PACKED
#else
    #define veloxfs_PACKED_START
    #define veloxfs_PACKED_END
    #define veloxfs_ATTR_PACKED __attribute__((packed))
#endif

/* Configuration */
#define veloxfs_BLOCK_SIZE       4096
#define veloxfs_MAX_PATH         480
#define veloxfs_MAGIC            0x564C5846  /* "VLXF" */
#define veloxfs_VERSION          5
#define veloxfs_JOURNAL_SIZE     64

/*
 * Timestamp hook -- override before including this header if your platform
 * has no RTC or no time() function (e.g. bare-metal, RTOS, WASM).
 *
 * Example for a system without a clock:
 *   #define veloxfs_TIME() 0
 *   #define veloxfs_IMPLEMENTATION
 *   #include "veloxfs.h"
 *
 * Example using a custom clock source:
 *   #define veloxfs_TIME() my_rtc_get_unix_seconds()
 *   #define veloxfs_IMPLEMENTATION
 *   #include "veloxfs.h"
 *
 * If not defined, defaults to the standard time(NULL).
 */
#ifndef veloxfs_TIME
#  include <time.h>
#  define veloxfs_TIME() time(NULL)
#endif

/* Error codes */
typedef enum {
    veloxfs_OK = 0,
    veloxfs_ERR_IO = -1,
    veloxfs_ERR_CORRUPT = -2,
    veloxfs_ERR_NOT_FOUND = -3,
    veloxfs_ERR_EXISTS = -4,
    veloxfs_ERR_NO_SPACE = -5,
    veloxfs_ERR_INVALID = -6,
    veloxfs_ERR_TOO_LARGE = -7,
    veloxfs_ERR_TOO_MANY_FILES = -8,
    veloxfs_ERR_PERMISSION = -9,
} veloxfs_error;

/* Permission bits */
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

/* FAT entry values - THE KEY CHANGE FROM v4 */
#define veloxfs_FAT_FREE         0x0000000000000000ULL  /* Block is free */
#define veloxfs_FAT_EOF          0xFFFFFFFFFFFFFFFFULL  /* End of file chain */
#define veloxfs_FAT_BAD          0xFFFFFFFFFFFFFFFEULL  /* Bad block */
/* All other values 0x00000001 - 0xFFFFFFFFFFFFFFFD are next block indices */

#define BATCH_BLOCKS 4096

/* Journal operation types */
typedef enum {
    veloxfs_JOP_NONE = 0,
    veloxfs_JOP_CREATE = 1,
    veloxfs_JOP_DELETE = 2,
    veloxfs_JOP_WRITE = 3,
    veloxfs_JOP_EXTEND = 4,
    veloxfs_JOP_TRUNCATE = 5,
    veloxfs_JOP_CHMOD = 6,
    veloxfs_JOP_CHOWN = 7,
} veloxfs_journal_op;

/* I/O callbacks */
typedef int (*veloxfs_read_fn)(void *user, uint64_t offset, void *buf, uint32_t size);
typedef int (*veloxfs_write_fn)(void *user, uint64_t offset, const void *buf, uint32_t size);

typedef struct {
    veloxfs_read_fn  read;
    veloxfs_write_fn write;
    void            *user_data;
} veloxfs_io;

/* On-disk structures */
veloxfs_PACKED_START
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t journal_enabled;
    uint64_t block_count;
    uint64_t fat_start;
    uint64_t fat_blocks;
    uint64_t journal_start;
    uint64_t journal_blocks;
    uint64_t inode_start;
    uint64_t inode_blocks;
    uint64_t data_start;         /* CRITICAL: Data region offset */
    uint64_t reserved[7];
} veloxfs_ATTR_PACKED veloxfs_superblock;
veloxfs_PACKED_END

/* Inode structure - SIMPLIFIED for linked-list */
veloxfs_PACKED_START
typedef struct {
    uint64_t inode_num;
    uint64_t size;              /* File size in bytes */
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint32_t reserved_was_extent_count;  /* Kept for binary compatibility */
    uint64_t ctime;
    uint64_t mtime;
    uint64_t atime;
    uint64_t first_block;       /* THE KEY FIELD: First block in chain */
    uint64_t reserved[10];      /* Reserved space (was extents + indirect_block) */
} veloxfs_ATTR_PACKED veloxfs_inode;
veloxfs_PACKED_END

/* Directory entry */
veloxfs_PACKED_START
typedef struct {
    char     path[veloxfs_MAX_PATH];
    uint64_t inode_num;
    uint64_t reserved[2];
} veloxfs_ATTR_PACKED veloxfs_dirent;
veloxfs_PACKED_END

/* Journal entry */
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

/* Runtime handle */
typedef struct {
    veloxfs_io         io;
    veloxfs_superblock super;
    uint64_t          *fat;           /* FAT array: fat[block] = next_block */
    veloxfs_inode     *inodes;
    veloxfs_dirent    *directory;
    veloxfs_journal_entry *journal;
    uint64_t          num_inodes;
    uint64_t          num_dirents;
    uint32_t          next_journal_seq;
    uint32_t          current_uid;
    uint32_t          current_gid;
    int               dirty_fat;
    int               dirty_inodes;
    int               dirty_dir;
    int               dirty_journal;
    
    /* Directory hash table for O(1) lookups */
    uint64_t         *dir_hash_table;
    uint64_t          dir_hash_size;
    uint64_t          last_alloc_idx;  /* Speed up allocation */
} veloxfs_handle;

/* File handle */
typedef struct {
    veloxfs_handle *fs;
    veloxfs_inode  *inode;
    uint64_t       position;
    uint64_t       current_block;      /* Cached for sequential access */
    uint64_t       current_block_idx;  /* Which block in file */
    int            is_open;
    int            modified;
    int            can_read;
    int            can_write;
} veloxfs_file;

/* File info structure */
typedef struct {
    uint64_t size;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint64_t block_count;  /* Number of blocks allocated */
    uint64_t ctime;
    uint64_t mtime;
    uint64_t atime;
} veloxfs_stat_t;

/* Allocation statistics */
typedef struct {
    uint64_t total_blocks;
    uint64_t used_blocks;
    uint64_t free_blocks;
    uint64_t longest_chain;       /* Longest file chain */
    float    avg_chain_length;    /* Average chain length */
} veloxfs_alloc_stats;

/* Core API - Full function list */

/* Filesystem management */
int veloxfs_format(veloxfs_io io, uint64_t block_count, int enable_journal);
int veloxfs_mount(veloxfs_handle *fs, veloxfs_io io);
int veloxfs_unmount(veloxfs_handle *fs);
int veloxfs_fsck(veloxfs_handle *fs);
int veloxfs_sync(veloxfs_handle *fs);

/* User context */
void veloxfs_set_user(veloxfs_handle *fs, uint32_t uid, uint32_t gid);
void veloxfs_get_user(veloxfs_handle *fs, uint32_t *uid, uint32_t *gid);

/* File operations */
int veloxfs_create(veloxfs_handle *fs, const char *path, uint32_t mode);
int veloxfs_delete(veloxfs_handle *fs, const char *path);
int veloxfs_rename(veloxfs_handle *fs, const char *old_path, const char *new_path);

/* Permission operations */
int veloxfs_chmod(veloxfs_handle *fs, const char *path, uint32_t mode);
int veloxfs_chown(veloxfs_handle *fs, const char *path, uint32_t uid, uint32_t gid);

/* I/O operations */
int veloxfs_write_file(veloxfs_handle *fs, const char *path, const void *data, uint64_t size);
int veloxfs_read_file(veloxfs_handle *fs, const char *path, void *out, uint64_t max_size, uint64_t *out_size);

/* File handle operations */
#define veloxfs_O_RDONLY  0x01
#define veloxfs_O_WRONLY  0x02
#define veloxfs_O_RDWR    0x03

int veloxfs_open(veloxfs_handle *fs, const char *path, int flags, veloxfs_file *file);
int veloxfs_close(veloxfs_file *file);
int veloxfs_read(veloxfs_file *file, void *buf, uint64_t count, uint64_t *bytes_read);
int veloxfs_write(veloxfs_file *file, const void *buf, uint64_t count);
int veloxfs_seek(veloxfs_file *file, int64_t offset, int whence);
uint64_t veloxfs_tell(veloxfs_file *file);
int veloxfs_truncate_handle(veloxfs_file *file, uint64_t new_size);

/* Directory operations */
int veloxfs_mkdir(veloxfs_handle *fs, const char *path, uint32_t mode);
typedef void (*veloxfs_list_callback)(const char *path, const veloxfs_stat_t *stat, int is_dir, void *user_data);
int veloxfs_list(veloxfs_handle *fs, const char *path, veloxfs_list_callback callback, void *user_data);

/* Stats */
int veloxfs_stat(veloxfs_handle *fs, const char *path, veloxfs_stat_t *stat);
int veloxfs_statfs(veloxfs_handle *fs, uint64_t *total, uint64_t *used, uint64_t *free_blocks);
int veloxfs_alloc_stats_get(veloxfs_handle *fs, veloxfs_alloc_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* veloxfs_V5_H */

/* ============================================================================
 * IMPLEMENTATION
 * ========================================================================= */

#ifdef veloxfs_IMPLEMENTATION

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* CRC32 for journal checksums */
static uint32_t crc32_table[256];
static int crc32_initialized = 0;

static void crc32_init(void) {
    if (crc32_initialized) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
        crc32_table[i] = crc;
    }
    crc32_initialized = 1;
}

static uint32_t crc32_compute(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

/* ========================================================================
 * HELPER FUNCTIONS
 * ====================================================================== */

static int veloxfs_read_block(veloxfs_handle *fs, uint64_t block_index, void *buf) {
    uint64_t offset = block_index * veloxfs_BLOCK_SIZE;
    return fs->io.read(fs->io.user_data, offset, buf, veloxfs_BLOCK_SIZE);
}

static int veloxfs_write_block(veloxfs_handle *fs, uint64_t block_index, const void *buf) {
    uint64_t offset = block_index * veloxfs_BLOCK_SIZE;
    return fs->io.write(fs->io.user_data, offset, buf, veloxfs_BLOCK_SIZE);
}

static int veloxfs_write_blocks(veloxfs_handle *fs, uint64_t block_start, const void *buf, uint64_t count) {
    uint64_t offset = block_start * veloxfs_BLOCK_SIZE;
    return fs->io.write(fs->io.user_data, offset, buf, count * veloxfs_BLOCK_SIZE);
}

static uint64_t veloxfs_calculate_fat_blocks(uint64_t block_count) {
    uint64_t entries_per_block = veloxfs_BLOCK_SIZE / sizeof(uint64_t);
    return (block_count + entries_per_block - 1) / entries_per_block;
}

static uint64_t veloxfs_calculate_inode_blocks(uint64_t block_count) {
    uint64_t inode_blocks = block_count / 50;
    return inode_blocks > 0 ? inode_blocks : 1;
}

static uint64_t veloxfs_calculate_dir_blocks(uint64_t block_count) {
    uint64_t dir_blocks = block_count / 100;
    return dir_blocks > 0 ? dir_blocks : 1;
}

static void normalize_path(const char *input, char *output, size_t output_size) {
    if (input[0] != '/') {
        snprintf(output, output_size, "/%s", input);
    } else {
        strncpy(output, input, output_size - 1);
        output[output_size - 1] = '\0';
    }
    
    size_t len = strlen(output);
    if (len > 1 && output[len - 1] == '/') {
        output[len - 1] = '\0';
    }
}

/* ========================================================================
 * HASH TABLE FOR O(1) DIRECTORY LOOKUPS
 * ====================================================================== */

static uint64_t hash_path(const char *path) {
    uint64_t hash = 14695981039346656037ULL;
    while (*path) {
        hash ^= (uint64_t)(unsigned char)(*path);
        hash *= 1099511628211ULL;
        path++;
    }
    return hash;
}

static void veloxfs_build_hash_table(veloxfs_handle *fs) {
    fs->dir_hash_size = fs->num_dirents + (fs->num_dirents / 2);
    fs->dir_hash_table = (uint64_t*)calloc(fs->dir_hash_size, sizeof(uint64_t));
    
    if (!fs->dir_hash_table) {
        fs->dir_hash_size = 0;
        return;
    }
    
    for (uint64_t i = 0; i < fs->dir_hash_size; i++) {
        fs->dir_hash_table[i] = UINT64_MAX;
    }
    
    for (uint64_t i = 0; i < fs->num_dirents; i++) {
        if (fs->directory[i].inode_num != 0) {
            uint64_t hash = hash_path(fs->directory[i].path);
            uint64_t slot = hash % fs->dir_hash_size;
            
            while (fs->dir_hash_table[slot] != UINT64_MAX) {
                slot = (slot + 1) % fs->dir_hash_size;
            }
            
            fs->dir_hash_table[slot] = i;
        }
    }
}

static void veloxfs_hash_insert(veloxfs_handle *fs, uint64_t dirent_idx) {
    if (!fs->dir_hash_table) return;
    
    const char *path = fs->directory[dirent_idx].path;
    uint64_t hash = hash_path(path);
    uint64_t slot = hash % fs->dir_hash_size;
    
    while (fs->dir_hash_table[slot] != UINT64_MAX) {
        slot = (slot + 1) % fs->dir_hash_size;
    }
    
    fs->dir_hash_table[slot] = dirent_idx;
}

static void veloxfs_hash_remove(veloxfs_handle *fs, const char *path) {
    if (!fs->dir_hash_table) return;
    
    uint64_t hash = hash_path(path);
    uint64_t slot = hash % fs->dir_hash_size;
    
    uint64_t probes = 0;
    while (probes < fs->dir_hash_size) {
        uint64_t idx = fs->dir_hash_table[slot];
        
        if (idx == UINT64_MAX) break;
        
        if (strcmp(fs->directory[idx].path, path) == 0) {
            fs->dir_hash_table[slot] = UINT64_MAX;
            return;
        }
        
        slot = (slot + 1) % fs->dir_hash_size;
        probes++;
    }
}

static void veloxfs_hash_update(veloxfs_handle *fs, const char *old_path, uint64_t dirent_idx) {
    if (!fs->dir_hash_table) return;
    veloxfs_hash_remove(fs, old_path);
    veloxfs_hash_insert(fs, dirent_idx);
}

/* ========================================================================
 * LINKED-LIST BLOCK ALLOCATION - THE KEY CHANGE
 * ====================================================================== */

/* Allocate a single free block */
static uint64_t veloxfs_alloc_block(veloxfs_handle *fs) {
    /* CRITICAL: Start search at data_start, not 0 */
    uint64_t start = fs->last_alloc_idx;
    if (start < fs->super.data_start) start = fs->super.data_start;
    
    /* Search from last allocation point */
    for (uint64_t i = 0; i < fs->super.block_count - fs->super.data_start; i++) {
        uint64_t idx = start + i;
        if (idx >= fs->super.block_count) {
            idx = fs->super.data_start + (idx - fs->super.block_count);
        }
        
        if (fs->fat[idx] == veloxfs_FAT_FREE) {
            fs->fat[idx] = veloxfs_FAT_EOF;
            fs->dirty_fat = 1;
            fs->last_alloc_idx = idx + 1;
            
            /* CRITICAL FIX: Zero the block before returning it! */
            uint8_t zero_block[veloxfs_BLOCK_SIZE];
            memset(zero_block, 0, veloxfs_BLOCK_SIZE);
            veloxfs_write_block(fs, idx, zero_block);
            
            return idx;
        }
    }
    
    return 0;  /* No free blocks */
}

/* Free a chain of blocks */
static void veloxfs_free_chain(veloxfs_handle *fs, uint64_t block_num) {
    while (block_num != 0 && block_num < fs->super.block_count) {
        uint64_t next = fs->fat[block_num];
        
        if (next == veloxfs_FAT_FREE || next == veloxfs_FAT_BAD) {
            break;
        }
        
        fs->fat[block_num] = veloxfs_FAT_FREE;
        fs->dirty_fat = 1;
        
        if (next == veloxfs_FAT_EOF) {
            break;
        }
        
        block_num = next;
    }
}

/* Count blocks in a chain */
static uint64_t veloxfs_count_chain(veloxfs_handle *fs, uint64_t block_num) {
    uint64_t count = 0;
    
    while (block_num != 0 && block_num < fs->super.block_count) {
        count++;
        uint64_t next = fs->fat[block_num];
        
        if (next == veloxfs_FAT_EOF || next == veloxfs_FAT_FREE || next == veloxfs_FAT_BAD) {
            break;
        }
        
        block_num = next;
        
        /* Safety: prevent infinite loops */
        if (count > fs->super.block_count) {
            printf("WARNING: Detected FAT loop at block %lu\n", (unsigned long)block_num);
            break;
        }
    }
    
    return count;
}

/* Get the Nth block in a chain */
static uint64_t veloxfs_get_nth_block(veloxfs_handle *fs, uint64_t first_block, uint64_t n) {
    uint64_t block = first_block;
    
    for (uint64_t i = 0; i < n && block != 0; i++) {
        uint64_t next = fs->fat[block];
        if (next == veloxfs_FAT_EOF || next == veloxfs_FAT_FREE || next == veloxfs_FAT_BAD) {
            return 0;
        }
        block = next;
    }
    
    return block;
}

/* Extend a file's chain by allocating more blocks */
static int veloxfs_extend_chain(veloxfs_handle *fs, veloxfs_inode *inode, uint64_t new_blocks) {
    /* If file is empty, allocate first block */
    if (inode->first_block == 0) {
        inode->first_block = veloxfs_alloc_block(fs);
        if (inode->first_block == 0) {
            return veloxfs_ERR_NO_SPACE;
        }
        new_blocks--;
        if (new_blocks == 0) {
            fs->dirty_inodes = 1;
            return veloxfs_OK;
        }
    }
    
    /* Find last block in chain */
    uint64_t last_block = inode->first_block;
    while (fs->fat[last_block] != veloxfs_FAT_EOF) {
        uint64_t next = fs->fat[last_block];
        if (next == veloxfs_FAT_FREE || next == veloxfs_FAT_BAD || next >= fs->super.block_count) {
            return veloxfs_ERR_CORRUPT;
        }
        last_block = next;
    }
    
    /* Allocate and link new blocks */
    for (uint64_t i = 0; i < new_blocks; i++) {
        uint64_t new_block = veloxfs_alloc_block(fs);
        if (new_block == 0) {
            return veloxfs_ERR_NO_SPACE;
        }
        
        fs->fat[last_block] = new_block;
        fs->fat[new_block] = veloxfs_FAT_EOF;
        last_block = new_block;
        fs->dirty_fat = 1;
    }
    
    fs->dirty_inodes = 1;
    return veloxfs_OK;
}

/* Truncate a file's chain to N blocks */
static int veloxfs_truncate_chain(veloxfs_handle *fs, veloxfs_inode *inode, uint64_t keep_blocks) {
    if (keep_blocks == 0) {
        if (inode->first_block != 0) {
            veloxfs_free_chain(fs, inode->first_block);
            inode->first_block = 0;
            fs->dirty_inodes = 1;
        }
        return veloxfs_OK;
    }
    
    /* Find the block to become the new end */
    uint64_t block = inode->first_block;
    for (uint64_t i = 1; i < keep_blocks && block != 0; i++) {
        uint64_t next = fs->fat[block];
        if (next == veloxfs_FAT_EOF) return veloxfs_OK;  /* Already short enough */
        block = next;
    }
    
    if (block == 0) return veloxfs_ERR_CORRUPT;
    
    /* Free everything after this block */
    uint64_t next = fs->fat[block];
    fs->fat[block] = veloxfs_FAT_EOF;
    fs->dirty_fat = 1;
    
    if (next != veloxfs_FAT_EOF && next != veloxfs_FAT_FREE) {
        veloxfs_free_chain(fs, next);
    }
    
    fs->dirty_inodes = 1;
    return veloxfs_OK;
}

/* ========================================================================
 * JOURNAL OPERATIONS
 * ====================================================================== */

static int veloxfs_journal_log(veloxfs_handle *fs, uint32_t op_type, uint64_t inode_num,
                              uint64_t block_addr, uint64_t old_val, uint64_t new_val) {
    if (!fs->super.journal_enabled) return veloxfs_OK;
    
    uint32_t idx = fs->next_journal_seq % veloxfs_JOURNAL_SIZE;
    veloxfs_journal_entry *entry = &fs->journal[idx];
    
    entry->sequence = fs->next_journal_seq++;
    entry->op_type = op_type;
    entry->inode_num = inode_num;
    entry->block_addr = block_addr;
    entry->old_value = old_val;
    entry->new_value = new_val;
    entry->committed = 0;
    entry->checksum = crc32_compute(entry, offsetof(veloxfs_journal_entry, checksum));
    
    uint8_t buf[veloxfs_BLOCK_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &fs->journal[idx], sizeof(veloxfs_journal_entry));
    veloxfs_write_block(fs, fs->super.journal_start + idx, buf);
    
    return veloxfs_OK;
}

static int veloxfs_journal_commit(veloxfs_handle *fs) {
    if (!fs->super.journal_enabled) return veloxfs_OK;
    
    for (uint32_t i = 0; i < veloxfs_JOURNAL_SIZE; i++) {
        if (fs->journal[i].op_type != veloxfs_JOP_NONE && !fs->journal[i].committed) {
            fs->journal[i].committed = 1;
            
            uint8_t buf[veloxfs_BLOCK_SIZE];
            memset(buf, 0, sizeof(buf));
            memcpy(buf, &fs->journal[i], sizeof(veloxfs_journal_entry));
            veloxfs_write_block(fs, fs->super.journal_start + i, buf);
        }
    }
    
    return veloxfs_OK;
}

/* Forward declarations for Journal Replay */
static veloxfs_inode* veloxfs_get_inode(veloxfs_handle *fs, uint64_t inode_num);
static int veloxfs_flush_inodes(veloxfs_handle *fs);

static int veloxfs_journal_replay(veloxfs_handle *fs) {
    if (!fs->super.journal_enabled) return veloxfs_OK;
    
    int replayed = 0;
    for (uint32_t i = 0; i < veloxfs_JOURNAL_SIZE; i++) {
        veloxfs_journal_entry *entry = &fs->journal[i];
        
        if (entry->op_type == veloxfs_JOP_NONE) continue;
        if (entry->committed) continue;
        
        uint32_t expected = entry->checksum;
        uint32_t actual = crc32_compute(entry, offsetof(veloxfs_journal_entry, checksum));
        
        if (expected != actual) continue;
        
        switch (entry->op_type) {
            case veloxfs_JOP_WRITE:
                // FIX: Verify this is actually a FAT update, or handle Inode Size rollback
                // Currently, JOP_WRITE logs Inode Size, so we must update Inode, NOT FAT.
                {
                    veloxfs_inode *inode = veloxfs_get_inode(fs, entry->inode_num);
                    if (inode) {
                        inode->size = entry->old_value;
                        fs->dirty_inodes = 1;
                        replayed++;
                    }
                }
                break;
            case veloxfs_JOP_DELETE:
                // Handle delete rollback if necessary
                break;
            case veloxfs_JOP_CREATE:
                // Handle create rollback if necessary
                break;
            default:
                break;
        }
        
        // Clear entry after processing
        memset(entry, 0, sizeof(veloxfs_journal_entry));
    }
    
    if (replayed > 0) {
        printf("Journal: Rolled back %d uncommitted operations\n", replayed);
        // Ensure the recovered state is written to disk
        veloxfs_flush_inodes(fs); 
    }
    
    return veloxfs_OK;
}

/* ========================================================================
 * PERMISSION CHECKING
 * ====================================================================== */

static int veloxfs_check_permission(veloxfs_handle *fs, veloxfs_inode *inode, int need_write) {
    if (fs->current_uid == 0) return 1;  /* Root can do anything */
    
    uint32_t mode = inode->mode;
    int can_read, can_write;
    
    if (inode->uid == fs->current_uid) {
        can_read = (mode & veloxfs_S_IRUSR) != 0;
        can_write = (mode & veloxfs_S_IWUSR) != 0;
    } else if (inode->gid == fs->current_gid) {
        can_read = (mode & veloxfs_S_IRGRP) != 0;
        can_write = (mode & veloxfs_S_IWGRP) != 0;
    } else {
        can_read = (mode & veloxfs_S_IROTH) != 0;
        can_write = (mode & veloxfs_S_IWOTH) != 0;
    }
    
    return need_write ? can_write : can_read;
}

/* ========================================================================
 * INODE OPERATIONS
 * ====================================================================== */

static veloxfs_inode* veloxfs_alloc_inode(veloxfs_handle *fs) {
    for (uint64_t i = 0; i < fs->num_inodes; i++) {
        if (fs->inodes[i].inode_num == 0) {
            memset(&fs->inodes[i], 0, sizeof(veloxfs_inode));
            fs->inodes[i].inode_num = i + 1;
            fs->inodes[i].first_block = 0;
            fs->dirty_inodes = 1;
            return &fs->inodes[i];
        }
    }
    return NULL;
}

static veloxfs_inode* veloxfs_get_inode(veloxfs_handle *fs, uint64_t inode_num) {
    if (inode_num == 0 || inode_num > fs->num_inodes) return NULL;
    veloxfs_inode *inode = &fs->inodes[inode_num - 1];
    if (inode->inode_num == 0) return NULL;
    return inode;
}

static void veloxfs_free_inode(veloxfs_handle *fs, uint64_t inode_num) {
    if (inode_num == 0 || inode_num > fs->num_inodes) return;
    
    veloxfs_inode *inode = &fs->inodes[inode_num - 1];
    
    if (inode->first_block != 0) {
        veloxfs_free_chain(fs, inode->first_block);
    }
    
    memset(inode, 0, sizeof(veloxfs_inode));
    fs->dirty_inodes = 1;
}

/* ========================================================================
 * DIRECTORY OPERATIONS
 * ====================================================================== */

static veloxfs_dirent* veloxfs_find_dirent(veloxfs_handle *fs, const char *path) {
    char normalized[veloxfs_MAX_PATH];
    normalize_path(path, normalized, sizeof(normalized));
    
    /* Use hash table if available */
    if (fs->dir_hash_table) {
        uint64_t hash = hash_path(normalized);
        uint64_t slot = hash % fs->dir_hash_size;
        
        uint64_t probes = 0;
        while (probes < fs->dir_hash_size) {
            uint64_t idx = fs->dir_hash_table[slot];
            
            if (idx == UINT64_MAX) {
                return NULL;
            }
            
            if (fs->directory[idx].inode_num != 0 &&
                strcmp(fs->directory[idx].path, normalized) == 0) {
                return &fs->directory[idx];
            }
            
            slot = (slot + 1) % fs->dir_hash_size;
            probes++;
        }
        
        return NULL;
    }
    
    /* Fallback: Linear search */
    for (uint64_t i = 0; i < fs->num_dirents; i++) {
        if (fs->directory[i].inode_num != 0 &&
            strcmp(fs->directory[i].path, normalized) == 0) {
            return &fs->directory[i];
        }
    }
    return NULL;
}

static veloxfs_dirent* veloxfs_find_free_dirent(veloxfs_handle *fs) {
    for (uint64_t i = 0; i < fs->num_dirents; i++) {
        if (fs->directory[i].inode_num == 0) {
            return &fs->directory[i];
        }
    }
    return NULL;
}

/* ========================================================================
 * FLUSH OPERATIONS
 * ====================================================================== */

static int veloxfs_flush_fat(veloxfs_handle *fs) {
    if (!fs->dirty_fat) return veloxfs_OK;
    
    uint64_t entries_per_block = veloxfs_BLOCK_SIZE / sizeof(uint64_t);
    uint8_t block_buf[veloxfs_BLOCK_SIZE];
    
    for (uint64_t i = 0; i < fs->super.fat_blocks; i++) {
        memset(block_buf, 0, veloxfs_BLOCK_SIZE);
        
        uint64_t offset = i * entries_per_block;
        uint64_t count = entries_per_block;
        if (offset + count > fs->super.block_count) {
            count = fs->super.block_count - offset;
        }
        
        memcpy(block_buf, &fs->fat[offset], count * sizeof(uint64_t));
        
        if (veloxfs_write_block(fs, fs->super.fat_start + i, block_buf) != 0) {
            return veloxfs_ERR_IO;
        }
    }
    
    fs->dirty_fat = 0;
    return veloxfs_OK;
}

static int veloxfs_flush_inodes(veloxfs_handle *fs) {
    if (!fs->dirty_inodes) return veloxfs_OK;
    
    uint64_t inodes_per_block = veloxfs_BLOCK_SIZE / sizeof(veloxfs_inode);
    uint8_t block_buf[veloxfs_BLOCK_SIZE];
    
    for (uint64_t i = 0; i < fs->super.inode_blocks; i++) {
        uint64_t offset = i * inodes_per_block;
        uint64_t count = inodes_per_block;
        if (offset + count > fs->num_inodes) {
            count = fs->num_inodes - offset;
        }
        
        memset(block_buf, 0, veloxfs_BLOCK_SIZE);
        memcpy(block_buf, &fs->inodes[offset], count * sizeof(veloxfs_inode));
        
        if (veloxfs_write_block(fs, fs->super.inode_start + i, block_buf) != 0) {
            return veloxfs_ERR_IO;
        }
    }
    
    fs->dirty_inodes = 0;
    return veloxfs_OK;
}

static int veloxfs_flush_dir(veloxfs_handle *fs) {
    if (!fs->dirty_dir) return veloxfs_OK;
    
    uint64_t entries_per_block = veloxfs_BLOCK_SIZE / sizeof(veloxfs_dirent);
    uint8_t block_buf[veloxfs_BLOCK_SIZE];
    
    uint64_t dir_blocks = veloxfs_calculate_dir_blocks(fs->super.block_count);
    for (uint64_t i = 0; i < dir_blocks; i++) {
        uint64_t offset = i * entries_per_block;
        uint64_t count = entries_per_block;
        if (offset + count > fs->num_dirents) {
            count = fs->num_dirents - offset;
        }
        
        memset(block_buf, 0, veloxfs_BLOCK_SIZE);
        memcpy(block_buf, &fs->directory[offset], count * sizeof(veloxfs_dirent));
        
        /* Directory is stored right after inodes */
        uint64_t dir_start = fs->super.inode_start + fs->super.inode_blocks;
        if (veloxfs_write_block(fs, dir_start + i, block_buf) != 0) {
            return veloxfs_ERR_IO;
        }
    }
    
    fs->dirty_dir = 0;
    return veloxfs_OK;
}

/* ========================================================================
 * CORE FILESYSTEM OPERATIONS
 * ====================================================================== */

int veloxfs_format(veloxfs_io io, uint64_t block_count, int enable_journal) {
    crc32_init();
    
    veloxfs_superblock super;
    memset(&super, 0, sizeof(super));
    
    super.magic = veloxfs_MAGIC;
    super.version = veloxfs_VERSION;
    super.block_size = veloxfs_BLOCK_SIZE;
    super.block_count = block_count;
    super.journal_enabled = enable_journal;
    
    /* Layout: [superblock][FAT][journal][inodes][directory][data] */
    uint64_t current_block = 1;
    
    /* FAT region */
    super.fat_start = current_block;
    super.fat_blocks = veloxfs_calculate_fat_blocks(block_count);
    current_block += super.fat_blocks;
    
    /* Journal region */
    super.journal_start = current_block;
    super.journal_blocks = enable_journal ? veloxfs_JOURNAL_SIZE : 0;
    current_block += super.journal_blocks;
    
    /* Inode region */
    super.inode_start = current_block;
    super.inode_blocks = veloxfs_calculate_inode_blocks(block_count);

    current_block += super.inode_blocks;
    
    /* Directory region */
    uint64_t dir_blocks = veloxfs_calculate_dir_blocks(block_count);
    current_block += dir_blocks;
    
    /* Data region - CRITICAL */
    super.data_start = current_block;
    
    /* Write superblock */
    if (io.write(io.user_data, 0, &super, sizeof(super)) != 0) {
        return veloxfs_ERR_IO;
    }
    
    /* Initialize FAT */
    uint64_t *fat = calloc(block_count, sizeof(uint64_t));
    if (!fat) return veloxfs_ERR_IO;
    
    /* Mark metadata blocks as BAD (unavailable) */
    for (uint64_t i = 0; i < super.data_start; i++) {
        fat[i] = veloxfs_FAT_BAD;
    }
    
    /* Write FAT */
    uint64_t entries_per_block = veloxfs_BLOCK_SIZE / sizeof(uint64_t);
    uint8_t block_buf[veloxfs_BLOCK_SIZE];
    
    for (uint64_t i = 0; i < super.fat_blocks; i++) {
        memset(block_buf, 0, veloxfs_BLOCK_SIZE);
        uint64_t offset = i * entries_per_block;
        uint64_t count = entries_per_block;
        if (offset + count > block_count) {
            count = block_count - offset;
        }
        memcpy(block_buf, &fat[offset], count * sizeof(uint64_t));
        
        uint64_t block_offset = (super.fat_start + i) * veloxfs_BLOCK_SIZE;
        io.write(io.user_data, block_offset, block_buf, veloxfs_BLOCK_SIZE);
    }
    
    free(fat);
    
    /* Zero inodes */
    memset(block_buf, 0, veloxfs_BLOCK_SIZE);
    for (uint64_t i = 0; i < super.inode_blocks; i++) {
        uint64_t block_offset = (super.inode_start + i) * veloxfs_BLOCK_SIZE;
        io.write(io.user_data, block_offset, block_buf, veloxfs_BLOCK_SIZE);
    }
    
    /* Zero directory */
    uint64_t dir_start = super.inode_start + super.inode_blocks;
    for (uint64_t i = 0; i < dir_blocks; i++) {
        uint64_t block_offset = (dir_start + i) * veloxfs_BLOCK_SIZE;
        io.write(io.user_data, block_offset, block_buf, veloxfs_BLOCK_SIZE);
    }
    
    return veloxfs_OK;
}

int veloxfs_mount(veloxfs_handle *fs, veloxfs_io io) {
    crc32_init();
    
    memset(fs, 0, sizeof(*fs));
    fs->io = io;
    
    /* Read superblock */
    if (io.read(io.user_data, 0, &fs->super, sizeof(fs->super)) != 0) {
        return veloxfs_ERR_IO;
    }
    
    /* Validate */
    if (fs->super.magic != veloxfs_MAGIC) {
        return veloxfs_ERR_CORRUPT;
    }
    
    /* Load FAT */
    uint64_t fat_bytes = fs->super.block_count * sizeof(uint64_t);
    fs->fat = malloc(fat_bytes);
    if (!fs->fat) return veloxfs_ERR_IO;
    
    uint64_t entries_per_block = veloxfs_BLOCK_SIZE / sizeof(uint64_t);
    uint8_t block_buf[veloxfs_BLOCK_SIZE];
    
    for (uint64_t i = 0; i < fs->super.fat_blocks; i++) {
        if (veloxfs_read_block(fs, fs->super.fat_start + i, block_buf) != 0) {
            free(fs->fat);
            return veloxfs_ERR_IO;
        }
        
        uint64_t offset = i * entries_per_block;
        uint64_t count = entries_per_block;
        if (offset + count > fs->super.block_count) {
            count = fs->super.block_count - offset;
        }
        
        memcpy(&fs->fat[offset], block_buf, count * sizeof(uint64_t));
    }
    
    /* Load inodes */
    uint64_t inode_bytes = fs->super.inode_blocks * veloxfs_BLOCK_SIZE;
    fs->inodes = malloc(inode_bytes);
    if (!fs->inodes) {
        free(fs->fat);
        return veloxfs_ERR_IO;
    }
    
    uint64_t inodes_per_block = veloxfs_BLOCK_SIZE / sizeof(veloxfs_inode);
    for (uint64_t i = 0; i < fs->super.inode_blocks; i++) {
        if (veloxfs_read_block(fs, fs->super.inode_start + i, block_buf) != 0) {
            free(fs->fat);
            free(fs->inodes);
            return veloxfs_ERR_IO;
        }
        
        uint64_t offset = i * inodes_per_block;
        memcpy(&fs->inodes[offset], block_buf, veloxfs_BLOCK_SIZE);
    }
    
    fs->num_inodes = inode_bytes / sizeof(veloxfs_inode);
    
    /* Load directory */
    uint64_t dir_blocks = veloxfs_calculate_dir_blocks(fs->super.block_count);
    uint64_t dir_bytes = dir_blocks * veloxfs_BLOCK_SIZE;
    fs->directory = malloc(dir_bytes);
    if (!fs->directory) {
        free(fs->fat);
        free(fs->inodes);
        return veloxfs_ERR_IO;
    }
    
    uint64_t dir_start = fs->super.inode_start + fs->super.inode_blocks;
    uint64_t dirents_per_block = veloxfs_BLOCK_SIZE / sizeof(veloxfs_dirent);
    
    for (uint64_t i = 0; i < dir_blocks; i++) {
        if (veloxfs_read_block(fs, dir_start + i, block_buf) != 0) {
            free(fs->fat);
            free(fs->inodes);
            free(fs->directory);
            return veloxfs_ERR_IO;
        }
        
        uint64_t offset = i * dirents_per_block;
        memcpy(&fs->directory[offset], block_buf, veloxfs_BLOCK_SIZE);
    }
    
    fs->num_dirents = dir_bytes / sizeof(veloxfs_dirent);
    
    /* Build hash table */
    veloxfs_build_hash_table(fs);
    
    /* Load journal if enabled */
    if (fs->super.journal_enabled) {
        uint64_t journal_bytes = veloxfs_JOURNAL_SIZE * sizeof(veloxfs_journal_entry);
        fs->journal = malloc(journal_bytes);
        if (!fs->journal) {
            free(fs->fat);
            free(fs->inodes);
            free(fs->directory);
            free(fs->dir_hash_table);
            return veloxfs_ERR_IO;
        }
        
        for (uint32_t i = 0; i < veloxfs_JOURNAL_SIZE; i++) {
            if (veloxfs_read_block(fs, fs->super.journal_start + i, block_buf) != 0) {
                free(fs->fat);
                free(fs->inodes);
                free(fs->directory);
                free(fs->dir_hash_table);
                free(fs->journal);
                return veloxfs_ERR_IO;
            }
            memcpy(&fs->journal[i], block_buf, sizeof(veloxfs_journal_entry));
        }
        
        /* Replay journal */
        veloxfs_journal_replay(fs);
    }
    
    fs->last_alloc_idx = fs->super.data_start;
    
    return veloxfs_OK;
}

int veloxfs_unmount(veloxfs_handle *fs) {
    if (!fs) return veloxfs_OK;

    // 1. Flush everything while memory is still valid
    veloxfs_sync(fs);
    
    // 2. Free and NULL - This prevents the double-free crash
    if (fs->fat) { free(fs->fat); fs->fat = NULL; }
    if (fs->inodes) { free(fs->inodes); fs->inodes = NULL; }
    if (fs->directory) { free(fs->directory); fs->directory = NULL; }
    if (fs->dir_hash_table) { free(fs->dir_hash_table); fs->dir_hash_table = NULL; }
    if (fs->journal) { free(fs->journal); fs->journal = NULL; }
    
    // Do NOT memset(fs, 0, sizeof(*fs)) if the handle 
    // itself is a global or managed by FUSE's state. 
    // Only zero it if you are 100% sure nothing else will touch it.
    
    return veloxfs_OK;
}
int veloxfs_sync(veloxfs_handle *fs) {
    if (!fs) return veloxfs_ERR_INVALID;
    
    int ret = veloxfs_OK;
    
    if (veloxfs_flush_fat(fs) != veloxfs_OK) ret = veloxfs_ERR_IO;
    if (veloxfs_flush_inodes(fs) != veloxfs_OK) ret = veloxfs_ERR_IO;
    if (veloxfs_flush_dir(fs) != veloxfs_OK) ret = veloxfs_ERR_IO;
    
    if (fs->super.journal_enabled) {
        veloxfs_journal_commit(fs);
    }
    
    return ret;
}

/* ========================================================================
 * USER CONTEXT
 * ====================================================================== */

void veloxfs_set_user(veloxfs_handle *fs, uint32_t uid, uint32_t gid) {
    fs->current_uid = uid;
    fs->current_gid = gid;
}

void veloxfs_get_user(veloxfs_handle *fs, uint32_t *uid, uint32_t *gid) {
    if (uid) *uid = fs->current_uid;
    if (gid) *gid = fs->current_gid;
}

/* ========================================================================
 * FILE OPERATIONS
 * ====================================================================== */

int veloxfs_create(veloxfs_handle *fs, const char *path, uint32_t mode) {
    char normalized[veloxfs_MAX_PATH];
    normalize_path(path, normalized, sizeof(normalized));
    
    if (veloxfs_find_dirent(fs, normalized) != NULL) {
        return veloxfs_ERR_EXISTS;
    }
    
    veloxfs_inode *inode = veloxfs_alloc_inode(fs);
    if (!inode) return veloxfs_ERR_TOO_MANY_FILES;
    
    inode->mode = mode;
    inode->uid = fs->current_uid;
    inode->gid = fs->current_gid;
    inode->ctime = inode->mtime = inode->atime = veloxfs_TIME();
    inode->first_block = 0;
    inode->size = 0;
    
    veloxfs_dirent *dirent = veloxfs_find_free_dirent(fs);
    if (!dirent) {
        veloxfs_free_inode(fs, inode->inode_num);
        return veloxfs_ERR_TOO_MANY_FILES;
    }
    
    strncpy(dirent->path, normalized, veloxfs_MAX_PATH - 1);
    dirent->path[veloxfs_MAX_PATH - 1] = '\0';
    dirent->inode_num = inode->inode_num;
    fs->dirty_dir = 1;
    
    /* Update hash table */
    for (uint64_t i = 0; i < fs->num_dirents; i++) {
        if (&fs->directory[i] == dirent) {
            veloxfs_hash_insert(fs, i);
            break;
        }
    }
    
    if (fs->super.journal_enabled) {
        veloxfs_journal_log(fs, veloxfs_JOP_CREATE, inode->inode_num, 0, 0, 0);
    }
    
    return veloxfs_OK;
}

int veloxfs_delete(veloxfs_handle *fs, const char *path) {
    char normalized[veloxfs_MAX_PATH];
    normalize_path(path, normalized, sizeof(normalized));
    
    veloxfs_dirent *dirent = veloxfs_find_dirent(fs, normalized);
    if (!dirent) return veloxfs_ERR_NOT_FOUND;
    
    veloxfs_inode *inode = veloxfs_get_inode(fs, dirent->inode_num);
    if (!inode) return veloxfs_ERR_CORRUPT;
    
    if (!veloxfs_check_permission(fs, inode, 1)) {
        return veloxfs_ERR_PERMISSION;
    }
    
    if (fs->super.journal_enabled) {
        veloxfs_journal_log(fs, veloxfs_JOP_DELETE, inode->inode_num, 0, 0, 0);
    }
    
    veloxfs_free_inode(fs, dirent->inode_num);
    
    veloxfs_hash_remove(fs, normalized);
    
    memset(dirent, 0, sizeof(*dirent));
    fs->dirty_dir = 1;
    
    if (fs->super.journal_enabled) {
        veloxfs_journal_commit(fs);
    }
    
    return veloxfs_OK;
}

int veloxfs_rename(veloxfs_handle *fs, const char *old_path, const char *new_path) {
    char old_norm[veloxfs_MAX_PATH], new_norm[veloxfs_MAX_PATH];
    normalize_path(old_path, old_norm, sizeof(old_norm));
    normalize_path(new_path, new_norm, sizeof(new_norm));
    
    veloxfs_dirent *old_dirent = veloxfs_find_dirent(fs, old_norm);
    if (!old_dirent) return veloxfs_ERR_NOT_FOUND;
    
    if (veloxfs_find_dirent(fs, new_norm)) return veloxfs_ERR_EXISTS;
    
    veloxfs_inode *inode = veloxfs_get_inode(fs, old_dirent->inode_num);
    if (!inode) return veloxfs_ERR_CORRUPT;
    
    if (!veloxfs_check_permission(fs, inode, 1)) {
        return veloxfs_ERR_PERMISSION;
    }
    
    strncpy(old_dirent->path, new_norm, veloxfs_MAX_PATH - 1);
    old_dirent->path[veloxfs_MAX_PATH - 1] = '\0';
    fs->dirty_dir = 1;
    
    /* Update hash table */
    for (uint64_t i = 0; i < fs->num_dirents; i++) {
        if (&fs->directory[i] == old_dirent) {
            veloxfs_hash_update(fs, old_norm, i);
            break;
        }
    }
    
    return veloxfs_OK;
}

int veloxfs_chmod(veloxfs_handle *fs, const char *path, uint32_t mode) {
    char normalized[veloxfs_MAX_PATH];
    normalize_path(path, normalized, sizeof(normalized));
    
    veloxfs_dirent *dirent = veloxfs_find_dirent(fs, normalized);
    if (!dirent) return veloxfs_ERR_NOT_FOUND;
    
    veloxfs_inode *inode = veloxfs_get_inode(fs, dirent->inode_num);
    if (!inode) return veloxfs_ERR_CORRUPT;
    
    if (fs->current_uid != 0 && fs->current_uid != inode->uid) {
        return veloxfs_ERR_PERMISSION;
    }
    
    if (fs->super.journal_enabled) {
        veloxfs_journal_log(fs, veloxfs_JOP_CHMOD, inode->inode_num, 0, inode->mode, mode);
    }
    
    inode->mode = mode;
    fs->dirty_inodes = 1;
    
    if (fs->super.journal_enabled) {
        veloxfs_journal_commit(fs);
    }
    
    return veloxfs_OK;
}

int veloxfs_chown(veloxfs_handle *fs, const char *path, uint32_t uid, uint32_t gid) {
    char normalized[veloxfs_MAX_PATH];
    normalize_path(path, normalized, sizeof(normalized));
    
    veloxfs_dirent *dirent = veloxfs_find_dirent(fs, normalized);
    if (!dirent) return veloxfs_ERR_NOT_FOUND;
    
    veloxfs_inode *inode = veloxfs_get_inode(fs, dirent->inode_num);
    if (!inode) return veloxfs_ERR_CORRUPT;
    
    if (fs->current_uid != 0) {
        return veloxfs_ERR_PERMISSION;
    }
    
    if (fs->super.journal_enabled) {
        veloxfs_journal_log(fs, veloxfs_JOP_CHOWN, inode->inode_num, 0, 
                          (uint64_t)inode->uid << 32 | inode->gid,
                          (uint64_t)uid << 32 | gid);
    }
    
    inode->uid = uid;
    inode->gid = gid;
    fs->dirty_inodes = 1;
    
    if (fs->super.journal_enabled) {
        veloxfs_journal_commit(fs);
    }
    
    return veloxfs_OK;
}

/* ========================================================================
 * I/O OPERATIONS - THE CRITICAL FUNCTIONS
 * ====================================================================== */

int veloxfs_write_file(veloxfs_handle *fs, const char *path, const void *data, uint64_t size) {
    char normalized[veloxfs_MAX_PATH];
    normalize_path(path, normalized, sizeof(normalized));
    
    veloxfs_dirent *dirent = veloxfs_find_dirent(fs, normalized);
    if (!dirent) return veloxfs_ERR_NOT_FOUND;
    
    veloxfs_inode *inode = veloxfs_get_inode(fs, dirent->inode_num);
    if (!inode) return veloxfs_ERR_CORRUPT;
    
    if (!veloxfs_check_permission(fs, inode, 1)) {
        return veloxfs_ERR_PERMISSION;
    }
    
    /* Calculate blocks needed */
    uint64_t blocks_needed = (size + veloxfs_BLOCK_SIZE - 1) / veloxfs_BLOCK_SIZE;
    uint64_t blocks_have = veloxfs_count_chain(fs, inode->first_block);
    
    if (fs->super.journal_enabled) {
        veloxfs_journal_log(fs, veloxfs_JOP_WRITE, inode->inode_num, inode->first_block, inode->size, size);
    }
    
    /* Extend chain if needed */
    if (blocks_needed > blocks_have) {
        int ret = veloxfs_extend_chain(fs, inode, blocks_needed - blocks_have);
        if (ret != veloxfs_OK) {
            return ret;
        }
    }
    /* Truncate chain if shrinking */
    else if (blocks_needed < blocks_have) {
        veloxfs_truncate_chain(fs, inode, blocks_needed);
    }
    
    /* Write data block by block */
    const uint8_t *src = (const uint8_t*)data;
    uint64_t remaining = size;
    uint64_t block = inode->first_block;
    
    while (remaining > 0 && block != 0) {
        uint8_t block_buf[veloxfs_BLOCK_SIZE];
        uint64_t to_write = (remaining < veloxfs_BLOCK_SIZE) ? remaining : veloxfs_BLOCK_SIZE;
        
        /* If partial block, read-modify-write */
        if (to_write < veloxfs_BLOCK_SIZE) {
            if (veloxfs_read_block(fs, block, block_buf) != 0) {
                /* If read fails, just zero it */
                memset(block_buf, 0, veloxfs_BLOCK_SIZE);
            }
        }
        
        memcpy(block_buf, src, to_write);
        
        if (veloxfs_write_block(fs, block, block_buf) != 0) {
            return veloxfs_ERR_IO;
        }
        
        src += to_write;
        remaining -= to_write;
        
        /* Get next block in chain */
        if (remaining > 0) {
            uint64_t next = fs->fat[block];
            if (next == veloxfs_FAT_EOF || next == veloxfs_FAT_FREE) {
                break;
            }
            block = next;
        }
    }
    
    inode->size = size;
    inode->mtime = veloxfs_TIME();
    fs->dirty_inodes = 1;
    
    if (fs->super.journal_enabled) {
        veloxfs_journal_commit(fs);
    }
    
    return veloxfs_OK;
}

int veloxfs_read_file(veloxfs_handle *fs, const char *path, void *out, uint64_t max_size, uint64_t *out_size) {
    char normalized[veloxfs_MAX_PATH];
    normalize_path(path, normalized, sizeof(normalized));
    
    veloxfs_dirent *dirent = veloxfs_find_dirent(fs, normalized);
    if (!dirent) return veloxfs_ERR_NOT_FOUND;
    
    veloxfs_inode *inode = veloxfs_get_inode(fs, dirent->inode_num);
    if (!inode) return veloxfs_ERR_CORRUPT;
    
    if (!veloxfs_check_permission(fs, inode, 0)) {
        return veloxfs_ERR_PERMISSION;
    }
    
    uint64_t to_read = (max_size < inode->size) ? max_size : inode->size;
    uint8_t *dest = (uint8_t*)out;
    uint64_t remaining = to_read;
    uint64_t block = inode->first_block;
    
    while (remaining > 0 && block != 0) {
        uint8_t block_buf[veloxfs_BLOCK_SIZE];
        
        if (veloxfs_read_block(fs, block, block_buf) != 0) {
            return veloxfs_ERR_IO;
        }
        
        uint64_t chunk = (remaining < veloxfs_BLOCK_SIZE) ? remaining : veloxfs_BLOCK_SIZE;
        memcpy(dest, block_buf, chunk);
        
        dest += chunk;
        remaining -= chunk;
        
        /* Get next block */
        if (remaining > 0) {
            uint64_t next = fs->fat[block];
            if (next == veloxfs_FAT_EOF || next == veloxfs_FAT_FREE) {
                break;
            }
            block = next;
        }
    }
    
    if (out_size) *out_size = to_read - remaining;
    
    inode->atime = veloxfs_TIME();
    fs->dirty_inodes = 1;
    
    return veloxfs_OK;
}

/* ========================================================================
 * FILE HANDLE OPERATIONS
 * ====================================================================== */

int veloxfs_open(veloxfs_handle *fs, const char *path, int flags, veloxfs_file *file) {
    char normalized[veloxfs_MAX_PATH];
    normalize_path(path, normalized, sizeof(normalized));
    
    veloxfs_dirent *dirent = veloxfs_find_dirent(fs, normalized);
    if (!dirent) return veloxfs_ERR_NOT_FOUND;
    
    veloxfs_inode *inode = veloxfs_get_inode(fs, dirent->inode_num);
    if (!inode) return veloxfs_ERR_CORRUPT;
    
    int need_write = (flags & veloxfs_O_WRONLY) || (flags & veloxfs_O_RDWR);
    if (!veloxfs_check_permission(fs, inode, need_write)) {
        return veloxfs_ERR_PERMISSION;
    }
    
    memset(file, 0, sizeof(*file));
    file->fs = fs;
    file->inode = inode;
    file->position = 0;
    file->current_block = inode->first_block;
    file->current_block_idx = 0;
    file->is_open = 1;
    file->can_read = (flags & veloxfs_O_RDONLY) || (flags & veloxfs_O_RDWR);
    file->can_write = need_write;
    
    return veloxfs_OK;
}

int veloxfs_close(veloxfs_file *file) {
    if (!file || !file->is_open) return veloxfs_ERR_INVALID;
    
    if (file->modified) {
        file->inode->mtime = veloxfs_TIME();
        file->fs->dirty_inodes = 1;
    }
    
    memset(file, 0, sizeof(*file));
    return veloxfs_OK;
}

int veloxfs_read(veloxfs_file *file, void *buf, uint64_t count, uint64_t *bytes_read) {
    if (!file || !file->is_open || !file->can_read) return veloxfs_ERR_INVALID;
    
    uint64_t to_read = count;
    if (file->position + to_read > file->inode->size) {
        to_read = file->inode->size - file->position;
    }
    
    uint8_t *dest = (uint8_t*)buf;
    uint64_t remaining = to_read;
    
    while (remaining > 0 && file->current_block != 0) {
        uint8_t block_buf[veloxfs_BLOCK_SIZE];
        
        if (veloxfs_read_block(file->fs, file->current_block, block_buf) != 0) {
            if (bytes_read) *bytes_read = to_read - remaining;
            return veloxfs_ERR_IO;
        }
        
        uint64_t offset_in_block = file->position % veloxfs_BLOCK_SIZE;
        uint64_t chunk = veloxfs_BLOCK_SIZE - offset_in_block;
        if (chunk > remaining) chunk = remaining;
        
        memcpy(dest, block_buf + offset_in_block, chunk);
        
        dest += chunk;
        remaining -= chunk;
        file->position += chunk;
        
        /* Move to next block if needed */
        if ((file->position % veloxfs_BLOCK_SIZE) == 0 && remaining > 0) {
            uint64_t next = file->fs->fat[file->current_block];
            if (next == veloxfs_FAT_EOF || next == veloxfs_FAT_FREE) {
                break;
            }
            file->current_block = next;
            file->current_block_idx++;
        }
    }
    
    if (bytes_read) *bytes_read = to_read - remaining;
    
    file->inode->atime = veloxfs_TIME();
    file->fs->dirty_inodes = 1;
    
    return veloxfs_OK;
}

int veloxfs_write(veloxfs_file *file, const void *buf, uint64_t count) {
    if (!file || !file->is_open || !file->can_write) return veloxfs_ERR_INVALID;
    
    /* Check if we need to extend the file */
    uint64_t end_position = file->position + count;
    if (end_position > file->inode->size) {
        uint64_t blocks_needed = (end_position + veloxfs_BLOCK_SIZE - 1) / veloxfs_BLOCK_SIZE;
        uint64_t blocks_have = veloxfs_count_chain(file->fs, file->inode->first_block);
        
        if (blocks_needed > blocks_have) {
            int ret = veloxfs_extend_chain(file->fs, file->inode, blocks_needed - blocks_have);
            if (ret != veloxfs_OK) return ret;
        }
        
        file->inode->size = end_position;
    }
    
    const uint8_t *src = (const uint8_t*)buf;
    uint64_t remaining = count;
    
    /* Navigate to current position.
     * CRITICAL: also re-navigate when current_block==0 but first_block!=0 —
     * this happens on the very first write to an empty file where extend_chain
     * just allocated first_block but the cached current_block is still 0. */
    uint64_t target_block_idx = file->position / veloxfs_BLOCK_SIZE;
    if (target_block_idx != file->current_block_idx ||
        (file->current_block == 0 && file->inode->first_block != 0)) {
        file->current_block = file->inode->first_block;
        file->current_block_idx = 0;
        
        for (uint64_t i = 0; i < target_block_idx && file->current_block != 0; i++) {
            uint64_t next = file->fs->fat[file->current_block];
            if (next == veloxfs_FAT_EOF || next == veloxfs_FAT_FREE) break;
            file->current_block = next;
            file->current_block_idx++;
        }
    }
    
    while (remaining > 0 && file->current_block != 0) {
        uint8_t block_buf[veloxfs_BLOCK_SIZE];
        
        uint64_t offset_in_block = file->position % veloxfs_BLOCK_SIZE;
        uint64_t chunk = veloxfs_BLOCK_SIZE - offset_in_block;
        if (chunk > remaining) chunk = remaining;
        
        /* If partial block, read-modify-write */
        if (offset_in_block != 0 || chunk < veloxfs_BLOCK_SIZE) {
            if (veloxfs_read_block(file->fs, file->current_block, block_buf) != 0) {
                memset(block_buf, 0, veloxfs_BLOCK_SIZE);
            }
        }
        
        memcpy(block_buf + offset_in_block, src, chunk);
        
        if (veloxfs_write_block(file->fs, file->current_block, block_buf) != 0) {
            return veloxfs_ERR_IO;
        }
        
        src += chunk;
        remaining -= chunk;
        file->position += chunk;
        
        /* Move to next block if needed */
        if ((file->position % veloxfs_BLOCK_SIZE) == 0 && remaining > 0) {
            uint64_t next = file->fs->fat[file->current_block];
            if (next == veloxfs_FAT_EOF || next == veloxfs_FAT_FREE) {
                break;
            }
            file->current_block = next;
            file->current_block_idx++;
        }
    }
    
    file->modified = 1;
    file->inode->mtime = veloxfs_TIME();
    file->fs->dirty_inodes = 1;
    
    return veloxfs_OK;
}

int veloxfs_seek(veloxfs_file *file, int64_t offset, int whence) {
    if (!file || !file->is_open) return veloxfs_ERR_INVALID;
    
    int64_t new_pos = 0;
    
    switch (whence) {
        case 0: /* SEEK_SET */
            new_pos = offset;
            break;
        case 1: /* SEEK_CUR */
            new_pos = (int64_t)file->position + offset;
            break;
        case 2: /* SEEK_END */
            new_pos = (int64_t)file->inode->size + offset;
            break;
        default:
            return veloxfs_ERR_INVALID;
    }
    
    if (new_pos < 0) {
        return veloxfs_ERR_INVALID;
    }
    /* Allow seeking to exactly file->inode->size (one-past-end) so writers can
     * append; veloxfs_write will extend the chain as needed.  Seeking beyond
     * the current size is NOT allowed via this path — the write handler must
     * have already extended first, or it can set position directly. */
    if ((uint64_t)new_pos > file->inode->size) {
        /* For write-capable handles, clamp to size so callers can detect the
         * out-of-range condition themselves.  Return OK so fuse_veloxfs_write
         * can extend-then-write correctly. */
        if (file->can_write) {
            /* Allow: veloxfs_write will extend the chain */
        } else {
            return veloxfs_ERR_INVALID;
        }
    }
    
    file->position = (uint64_t)new_pos;
    
    /* Update block cache.
     * Also re-navigate when current_block==0 but first_block!=0 (stale pointer). */
    uint64_t target_block_idx = file->position / veloxfs_BLOCK_SIZE;
    if (target_block_idx != file->current_block_idx ||
        (file->current_block == 0 && file->inode->first_block != 0)) {
        file->current_block = file->inode->first_block;
        file->current_block_idx = 0;
        
        for (uint64_t i = 0; i < target_block_idx && file->current_block != 0; i++) {
            uint64_t next = file->fs->fat[file->current_block];
            if (next == veloxfs_FAT_EOF || next == veloxfs_FAT_FREE) break;
            file->current_block = next;
            file->current_block_idx++;
        }
    }
    
    return veloxfs_OK;
}

uint64_t veloxfs_tell(veloxfs_file *file) {
    if (!file || !file->is_open) return 0;
    return file->position;
}

int veloxfs_truncate_handle(veloxfs_file *file, uint64_t new_size) {
    if (!file || !file->is_open || !file->can_write) return veloxfs_ERR_INVALID;
    
    uint64_t blocks_needed = (new_size + veloxfs_BLOCK_SIZE - 1) / veloxfs_BLOCK_SIZE;
    uint64_t blocks_have = veloxfs_count_chain(file->fs, file->inode->first_block);
    
    if (blocks_needed > blocks_have) {
        int ret = veloxfs_extend_chain(file->fs, file->inode, blocks_needed - blocks_have);
        if (ret != veloxfs_OK) return ret;
    } else if (blocks_needed < blocks_have) {
        veloxfs_truncate_chain(file->fs, file->inode, blocks_needed);
    }
    
    file->inode->size = new_size;
    file->modified = 1;
    file->fs->dirty_inodes = 1;
    
    /* Adjust position if it's beyond new size */
    if (file->position > new_size) {
        file->position = new_size;
    }
    
    return veloxfs_OK;
}

/* ========================================================================
 * DIRECTORY OPERATIONS
 * ====================================================================== */

int veloxfs_mkdir(veloxfs_handle *fs, const char *path, uint32_t mode) {
    /* In this simple implementation, mkdir is the same as create */
    /* A real implementation would have a separate directory type */
    return veloxfs_create(fs, path, mode | veloxfs_S_IRWXU);
}

int veloxfs_list(veloxfs_handle *fs, const char *path, veloxfs_list_callback callback, void *user_data) {
    char normalized[veloxfs_MAX_PATH];
    normalize_path(path, normalized, sizeof(normalized));
    
    size_t prefix_len = strlen(normalized);
    if (prefix_len > 0 && normalized[prefix_len - 1] != '/') {
        strncat(normalized, "/", veloxfs_MAX_PATH - prefix_len - 1);
        prefix_len++;
    }
    
    for (uint64_t i = 0; i < fs->num_dirents; i++) {
        if (fs->directory[i].inode_num == 0) continue;
        
        const char *entry_path = fs->directory[i].path;
        
        /* Check if this entry is under the specified path */
        if (strcmp(normalized, "/") == 0 || strncmp(entry_path, normalized, prefix_len) == 0) {
            veloxfs_inode *inode = veloxfs_get_inode(fs, fs->directory[i].inode_num);
            if (!inode) continue;
            
            veloxfs_stat_t stat;
            stat.size = inode->size;
            stat.uid = inode->uid;
            stat.gid = inode->gid;
            stat.mode = inode->mode;
            stat.block_count = veloxfs_count_chain(fs, inode->first_block);
            stat.ctime = inode->ctime;
            stat.mtime = inode->mtime;
            stat.atime = inode->atime;
            
            callback(entry_path, &stat, 0, user_data);
        }
    }
    
    return veloxfs_OK;
}

/* ========================================================================
 * STATISTICS
 * ====================================================================== */

int veloxfs_stat(veloxfs_handle *fs, const char *path, veloxfs_stat_t *stat) {
    char normalized[veloxfs_MAX_PATH];
    normalize_path(path, normalized, sizeof(normalized));
    
    veloxfs_dirent *dirent = veloxfs_find_dirent(fs, normalized);
    if (!dirent) return veloxfs_ERR_NOT_FOUND;
    
    veloxfs_inode *inode = veloxfs_get_inode(fs, dirent->inode_num);
    if (!inode) return veloxfs_ERR_CORRUPT;
    
    stat->size = inode->size;
    stat->uid = inode->uid;
    stat->gid = inode->gid;
    stat->mode = inode->mode;
    stat->block_count = veloxfs_count_chain(fs, inode->first_block);
    stat->ctime = inode->ctime;
    stat->mtime = inode->mtime;
    stat->atime = inode->atime;
    
    return veloxfs_OK;
}

int veloxfs_statfs(veloxfs_handle *fs, uint64_t *total, uint64_t *used, uint64_t *free_blocks) {
    uint64_t used_count = 0;
    
    for (uint64_t i = 0; i < fs->super.block_count; i++) {
        if (fs->fat[i] != veloxfs_FAT_FREE) {
            used_count++;
        }
    }
    
    if (total) *total = fs->super.block_count;
    if (used) *used = used_count;
    if (free_blocks) *free_blocks = fs->super.block_count - used_count;
    
    return veloxfs_OK;
}

int veloxfs_alloc_stats_get(veloxfs_handle *fs, veloxfs_alloc_stats *stats) {
    memset(stats, 0, sizeof(*stats));
    
    uint64_t used = 0;
    for (uint64_t i = 0; i < fs->super.block_count; i++) {
        if (fs->fat[i] != veloxfs_FAT_FREE) used++;
    }
    
    stats->total_blocks = fs->super.block_count;
    stats->used_blocks = used;
    stats->free_blocks = fs->super.block_count - used;
    
    /* Find longest chain and average */
    uint64_t total_chains = 0;
    uint64_t total_chain_length = 0;
    
    for (uint64_t i = 0; i < fs->num_inodes; i++) {
        if (fs->inodes[i].inode_num != 0 && fs->inodes[i].first_block != 0) {
            uint64_t len = veloxfs_count_chain(fs, fs->inodes[i].first_block);
            if (len > stats->longest_chain) {
                stats->longest_chain = len;
            }
            total_chains++;
            total_chain_length += len;
        }
    }
    
    stats->avg_chain_length = (total_chains > 0) ? 
        ((float)total_chain_length / total_chains) : 0.0f;
    
    return veloxfs_OK;
}

/* ========================================================================
 * FILESYSTEM CHECK
 * ====================================================================== */

int veloxfs_fsck(veloxfs_handle *fs) {
    int errors = 0;
    
    uint8_t *used = calloc(fs->super.block_count, 1);
    if (!used) return veloxfs_ERR_IO;
    
    /* Mark metadata blocks */
    for (uint64_t i = 0; i < fs->super.data_start; i++) {
        used[i] = 1;
    }
    
    /* Walk all chains and mark blocks */
    for (uint64_t i = 0; i < fs->num_inodes; i++) {
        if (fs->inodes[i].inode_num == 0) continue;
        
        uint64_t block = fs->inodes[i].first_block;
        while (block != 0 && block < fs->super.block_count) {
            if (used[block]) {
                printf("fsck: Block %lu used by multiple files\n", (unsigned long)block);
                errors++;
            }
            used[block] = 1;
            
            uint64_t next = fs->fat[block];
            if (next == veloxfs_FAT_EOF || next == veloxfs_FAT_FREE) break;
            block = next;
        }
    }
    
    /* Find orphaned blocks */
    int orphaned = 0;
    for (uint64_t i = fs->super.data_start; i < fs->super.block_count; i++) {
        if (fs->fat[i] != veloxfs_FAT_FREE && !used[i]) {
            fs->fat[i] = veloxfs_FAT_FREE;
            orphaned++;
            fs->dirty_fat = 1;
        }
    }
    
    if (orphaned > 0) {
        printf("fsck: Freed %d orphaned blocks\n", orphaned);
        veloxfs_flush_fat(fs);
    }
    
    free(used);
    
    if (errors == 0 && orphaned == 0) {
        printf("fsck: Filesystem is clean\n");
    }
    
    return errors == 0 ? veloxfs_OK : veloxfs_ERR_CORRUPT;
}

#endif /* veloxfs_IMPLEMENTATION */

/*
------------------------------------------------------------------------------
This software is available under 2 licenses -- choose whichever you prefer.
------------------------------------------------------------------------------
====================================================================================
====================================================================================
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
====================================================================================
====================================================================================
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
====================================================================================
*/
