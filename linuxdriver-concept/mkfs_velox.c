#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <endian.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#define VELOX_BS        4096           
#define INODE_SIZE      256
#define INLINE_EXTENTS  4
#define INODE_F_EXTENTS 0x01
#define INODE_F_DIR     0x02
#define FAT_EOF         0xFFFFFFFFFFFFFFFFULL

typedef struct {
    uint64_t start_block;
    uint64_t block_count;
} __attribute__((packed)) veloxfs_extent;

struct __attribute__((packed)) veloxfs_superblock {
    uint32_t magic; uint32_t version; uint32_t block_size;
    uint32_t journal_enabled; uint32_t storage_hint; uint32_t _pad0;
    uint64_t block_count; uint64_t fat_start; uint64_t fat_blocks;
    uint64_t journal_start; uint64_t journal_blocks;
    uint64_t inode_start; uint64_t inode_blocks;
    uint64_t data_start; uint64_t reserved[4];
};

struct __attribute__((packed)) veloxfs_inode {
    uint64_t inode_num; uint64_t size;
    uint32_t uid; uint32_t gid; uint32_t mode; uint32_t inode_flags;
    uint64_t ctime; uint64_t mtime; uint64_t atime;
    uint64_t fat_head;
    veloxfs_extent extents[INLINE_EXTENTS];
    uint64_t extent_count;
    uint64_t _ipad; uint64_t _reserved_padding[14]; 
};

int main(int argc, char *argv[]) {
    if (argc != 2) return 1;
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    struct stat st;
    fstat(fd, &st);
    uint64_t dev_size = 0;
    if (S_ISBLK(st.st_mode)) ioctl(fd, BLKGETSIZE64, &dev_size);
    else dev_size = st.st_size;

    uint64_t total_blocks = dev_size / VELOX_BS;
    
    /* Layout perfectly matching veloxfs_mod.c */
    uint64_t fat_blocks   = (total_blocks * 8 + VELOX_BS - 1) / VELOX_BS;
    uint64_t inode_blocks = 125000; 
    uint64_t dir_blocks   = total_blocks / 100; // Driver hardcodes this to 1%
    if (dir_blocks == 0) dir_blocks = 1;
    
    // Data now starts safely AFTER the directory table
    uint64_t data_start   = 1 + fat_blocks + inode_blocks + dir_blocks;

    void *buf = calloc(1, VELOX_BS);
    uint64_t now = (uint64_t)time(NULL);

    /* 1. Superblock */
    struct veloxfs_superblock *sb = (struct veloxfs_superblock *)buf;
    sb->magic = htole32(0x564C5836UL);
    sb->version = htole32(6);
    sb->block_size = htole32(VELOX_BS);
    sb->block_count = htole64(total_blocks);
    sb->fat_start = htole64(1);
    sb->fat_blocks = htole64(fat_blocks);
    sb->inode_start = htole64(1 + fat_blocks);
    sb->inode_blocks = htole64(inode_blocks);
    sb->data_start = htole64(data_start);
    pwrite(fd, buf, VELOX_BS, 0);

    /* 2. WIPE INODE AND DIRENT TABLES */
    printf("Wiping Inode and Dirent Tables (%lu blocks)...\n", inode_blocks + dir_blocks);
    void *zero_buf = calloc(1, VELOX_BS);
    for (uint64_t i = 0; i < (inode_blocks + dir_blocks); i++) {
        pwrite(fd, zero_buf, VELOX_BS, (1 + fat_blocks + i) * VELOX_BS);
        if (i > 0 && i % 25000 == 0) printf("Progress: %lu blocks cleaned...\n", i);
    }
    free(zero_buf);

    /* 3. Root Inode (Slot 0) */
    struct veloxfs_inode *root = (struct veloxfs_inode *)buf;
    memset(buf, 0, VELOX_BS);
    root->inode_num = htole64(0);
    root->mode = htole32(S_IFDIR | 0755);
    root->inode_flags = htole32(INODE_F_DIR | INODE_F_EXTENTS);
    root->size = htole64(VELOX_BS);
    root->extent_count = htole64(1);
    root->extents[0].start_block = htole64(data_start);
    root->extents[0].block_count = htole64(1);
    pwrite(fd, buf, VELOX_BS, (1 + fat_blocks) * VELOX_BS);

    /* 4. FAT Initialization */
    printf("Finalizing FAT...\n");
    for (uint64_t b = 0; b < fat_blocks; b++) {
        uint64_t *fat_ptr = (uint64_t *)buf;
        memset(buf, 0, VELOX_BS);
        for (int i = 0; i < (VELOX_BS / 8); i++) {
            uint64_t abs_idx = (b * (VELOX_BS / 8)) + i;
            // Mark SB, FAT, Inodes, and Dirent tables as reserved
            if (abs_idx < data_start) fat_ptr[i] = htole64(FAT_EOF);
        }
        pwrite(fd, buf, VELOX_BS, (1 + b) * VELOX_BS);
    }

    /* 5. Clear Root Dir Data */
    memset(buf, 0, VELOX_BS);
    pwrite(fd, buf, VELOX_BS, data_start * VELOX_BS);

    free(buf);
    close(fd);
    printf("Success! Filesystem mapped correctly for VeloxFS v6 Kernel Module.\n");
    return 0;
}
