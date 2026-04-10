#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <endian.h>

#define VELOX_MAGIC 0x564C5836
#define BLOCK_SIZE  4096
#define INODE_SIZE  144

typedef uint32_t __u32;
typedef uint64_t __u64;

struct __attribute__((packed)) veloxfs_superblock {
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
};

struct __attribute__((packed)) veloxfs_inode {
    uint16_t i_mode;
    uint16_t i_nlink;
    uint32_t i_uid;
    uint32_t i_gid;
    uint64_t i_size;
    uint64_t i_blocks;
    uint64_t i_atime;
    uint64_t i_mtime;
    uint64_t i_ctime;
    uint32_t i_flags;
    uint32_t i_data[15];
    uint8_t  padding[24]; 
};

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <device>\n", argv[0]);
        return 1;
    }

    int fd = open(argv[1], O_RDWR | O_SYNC);
    if (fd < 0) { perror("open"); return 1; }

    // 1. Get the actual 8GB size without using linux/fs.h (prevents conflicts)
    off_t device_size = lseek(fd, 0, SEEK_END);
    if (device_size == -1) { perror("lseek"); return 1; }
    uint64_t total_blocks = device_size / BLOCK_SIZE;
    lseek(fd, 0, SEEK_SET);

    void *buf;
    if (posix_memalign(&buf, BLOCK_SIZE, BLOCK_SIZE) != 0) return 1;
    memset(buf, 0, BLOCK_SIZE);

    // 2. SUPERBLOCK (Matches your working logic)
    struct veloxfs_superblock *sb = (struct veloxfs_superblock *)buf;
    sb->magic = htole32(VELOX_MAGIC);
    sb->version = htole32(6);
    sb->block_size = htole32(BLOCK_SIZE);
    
    // NEW: Use actual capacity instead of 1024
    sb->block_count = htole64(total_blocks);
    
    // Maintain your working layout offsets
    sb->fat_start = htole64(1);
    sb->fat_blocks = htole64(1); // Keeps it simple for the mount
    sb->inode_start = htole64(2);
    sb->inode_blocks = htole64(1);
    sb->data_start = htole64(3);
    
    pwrite(fd, buf, BLOCK_SIZE, 0);

    // 3. ROOT INODE (Block 2)
    memset(buf, 0, BLOCK_SIZE);
    struct veloxfs_inode *ri = (struct veloxfs_inode *)buf;
    ri->i_mode = htole16(040755); 
    ri->i_nlink = htole16(2);
    ri->i_uid = htole32(1000); // Usually the 'max' user
    ri->i_gid = htole32(1000); // Usually the 'max' group
    ri->i_size = htole64(BLOCK_SIZE);
    ri->i_blocks = htole64(1);
    ri->i_data[0] = htole32(3); 
    
    pwrite(fd, buf, BLOCK_SIZE, 2 * BLOCK_SIZE);

    printf("VeloxFS v6 formatted. Capacity: %lu blocks (~%.2f GB)\n", 
            total_blocks, (double)device_size / (1024*1024*1024));
    
    free(buf);
    close(fd);
    return 0;
}