/*
 * veloxfs_fuse.c -- FUSE adapter for veloxfs v6 (Storage-Optimized Edition)
 *
 * v6 FUSE CHANGES vs v5
 * =====================
 *  - max_write / max_read bumped to 1 MB (was 128 KB)
 *    Large writes align with the contiguous allocator; the kernel coalesces
 *    many small fuse_write() calls into one 1 MB I/O that lands in a single
 *    pwrite() call to disk.
 *
 *  - --format-hdd   : 64 KB blocks  (default, good for spinning drives)
 *  - --format-nand  : 128 KB blocks (aligns to typical NAND erase boundaries)
 *  - --format-flash : 64 KB blocks  (NOR/managed flash)
 *  - --format-nvme  : 4 KB blocks   (NVMe -- back-compat with v5 feel)
 *  - --format       : alias for --format-hdd
 *
 *  - Storage hint printed on mount so users know which mode they formatted
 *
 *  - posix_fadvise(POSIX_FADV_SEQUENTIAL) on the disk fd: tells the kernel
 *    to read-ahead aggressively, which benefits HDD sequential workloads.
 *
 *  - All other bug fixes from v5 are retained:
 *    * pread/pwrite for thread-safe I/O
 *    * Global pthread_mutex serialising all veloxfs API calls
 *    * keep_cache=0 (no stale kernel pages)
 *    * Correct fsync handler
 *    * BLKGETSIZE64 support for block devices
 *
 * COMPILE
 *   gcc -Wall -O2 -pthread veloxfs_fuse.c -o veloxfs_fuse \
 *       `pkg-config fuse --cflags --libs`
 *
 * QUICK START (HDD / spinning drive)
 *   # Real drive:
 *   ./veloxfs_fuse --format-hdd /dev/sdb
 *   mkdir /mnt/velox
 *   ./veloxfs_fuse /dev/sdb /mnt/velox -o big_writes,max_write=1048576,max_read=1048576
 *
 *   # Image file:
 *   dd if=/dev/zero of=veloxfs.img bs=1M count=512
 *   ./veloxfs_fuse --format-hdd veloxfs.img
 *   mkdir /tmp/mnt
 *   ./veloxfs_fuse veloxfs.img /tmp/mnt -o big_writes,max_write=1048576,max_read=1048576
 *   cp bigfile.bin /tmp/mnt/
 *   fusermount -u /tmp/mnt
 *
 * QUICK START (NAND flash)
 *   ./veloxfs_fuse --format-nand /dev/mtdblock0
 *   ./veloxfs_fuse /dev/mtdblock0 /mnt/velox \
 *       -o big_writes,max_write=1048576,max_read=1048576,direct_io
 *
 * DEBUG / TESTING
 *   ./veloxfs_fuse veloxfs.img /tmp/mnt \
 *       -d -o big_writes,max_write=1048576,max_read=1048576
 */

#define FUSE_USE_VERSION 26
#define veloxfs_IMPLEMENTATION

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#ifdef POSIX_FADV_SEQUENTIAL
#  include <fcntl.h>
#endif

#include "veloxfs.h"

/* =========================================================================
 * Global FUSE state
 * ======================================================================= */

static struct {
    int              disk_fd;
    veloxfs_handle   fs;
    char            *disk_path;
    pthread_mutex_t  lock;
} g;

#define FS_LOCK()   pthread_mutex_lock(&g.lock)
#define FS_UNLOCK() pthread_mutex_unlock(&g.lock)

/* We need STORAGE_NVME for the name helper even though the superblock uses
 * veloxfs_STORAGE_AUTO for NVMe-formatted images. */
#define veloxfs_STORAGE_NVME 99

/* =========================================================================
 * Storage hint name helper
 * ======================================================================= */
static const char *storage_hint_name(uint32_t hint) {
    switch (hint) {
        case veloxfs_STORAGE_HDD:   return "HDD (64 KB blocks, contiguous alloc)";
        case veloxfs_STORAGE_FLASH: return "Flash (64 KB blocks)";
        case veloxfs_STORAGE_NAND:  return "NAND (128 KB blocks, erase-aligned)";
        case veloxfs_STORAGE_NVME:
        case veloxfs_STORAGE_AUTO:
        default:
            return (g.fs.super.block_size <= 4096) ?
                   "NVMe/SSD (4 KB blocks)" : "Auto";
    }
}



/* =========================================================================
 * Thread-safe I/O callbacks  (pread/pwrite: atomic w.r.t. file position)
 * ======================================================================= */

static int disk_read_cb(void *user, uint64_t offset, void *buf, uint32_t size) {
    int fd = *(int*)user;
    ssize_t n = pread(fd, buf, size, (off_t)offset);
    return (n == (ssize_t)size) ? 0 : -1;
}

static int disk_write_cb(void *user, uint64_t offset, const void *buf, uint32_t size) {
    int fd = *(int*)user;
    ssize_t n = pwrite(fd, buf, size, (off_t)offset);
    return (n == (ssize_t)size) ? 0 : -1;
}

static void disk_fsync_all(void) {
    if (g.disk_fd >= 0) fsync(g.disk_fd);
}

/* =========================================================================
 * destroy
 * ======================================================================= */

static void fuse_veloxfs_destroy(void *private_data) {
    (void)private_data;
    fprintf(stderr, "[veloxfs] destroy: flushing and unmounting\n");
    veloxfs_sync(&g.fs);
    disk_fsync_all();
    veloxfs_unmount(&g.fs);
    if (g.disk_fd >= 0) { close(g.disk_fd); g.disk_fd = -1; }
}

/* =========================================================================
 * getattr
 * ======================================================================= */

static int veloxfs_fuse_getattr(const char *path, struct stat *stbuf) {
    memset(stbuf, 0, sizeof(*stbuf));

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode  = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    FS_LOCK();
    veloxfs_stat_t tfs;
    int ret = veloxfs_stat(&g.fs, path, &tfs);
    if (ret == veloxfs_OK) {
        stbuf->st_mode    = S_IFREG | tfs.mode;
        stbuf->st_nlink   = 1;
        stbuf->st_size    = (off_t)tfs.size;
        stbuf->st_uid     = tfs.uid;
        stbuf->st_gid     = tfs.gid;
        stbuf->st_ctime   = (time_t)tfs.ctime;
        stbuf->st_mtime   = (time_t)tfs.mtime;
        stbuf->st_atime   = (time_t)tfs.atime;
        stbuf->st_blocks  = (blkcnt_t)((tfs.size + 511) / 512);
        stbuf->st_blksize = (blksize_t)veloxfs_BS(&g.fs);
        FS_UNLOCK();
        return 0;
    }

    /* Check implicit directory */
    char prefix[veloxfs_MAX_PATH];
    snprintf(prefix, sizeof(prefix), "%s/", path);
    size_t plen = strlen(prefix);
    for (uint64_t i = 0; i < g.fs.num_dirents; i++) {
        if (g.fs.directory[i].inode_num != 0 &&
            strncmp(g.fs.directory[i].path, prefix, plen) == 0) {
            stbuf->st_mode  = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            FS_UNLOCK();
            return 0;
        }
    }
    FS_UNLOCK();
    return -ENOENT;
}

/* =========================================================================
 * readdir
 * ======================================================================= */

#define READDIR_SEEN_SIZE 512
struct readdir_ctx {
    void           *buf;
    fuse_fill_dir_t filler;
    const char     *dir_path;
    size_t          dir_path_len;
    char  seen[READDIR_SEEN_SIZE][veloxfs_MAX_PATH];
    int   seen_count;
};

static int readdir_already_seen(struct readdir_ctx *ctx, const char *name) {
    if (ctx->seen_count == 0) return 0;
    uint64_t h = 14695981039346656037ULL;
    for (const char *p = name; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    uint64_t slot = h % READDIR_SEEN_SIZE;
    for (int i = 0; i < READDIR_SEEN_SIZE; i++) {
        if (ctx->seen[slot][0] == '\0') return 0;
        if (strcmp(ctx->seen[slot], name) == 0) return 1;
        slot = (slot + 1) % READDIR_SEEN_SIZE;
    }
    return 0;
}

static void readdir_mark_seen(struct readdir_ctx *ctx, const char *name) {
    if (ctx->seen_count >= READDIR_SEEN_SIZE - 1) return;
    uint64_t h = 14695981039346656037ULL;
    for (const char *p = name; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    uint64_t slot = h % READDIR_SEEN_SIZE;
    while (ctx->seen[slot][0] != '\0')
        slot = (slot + 1) % READDIR_SEEN_SIZE;
    strncpy(ctx->seen[slot], name, veloxfs_MAX_PATH - 1);
    ctx->seen[slot][veloxfs_MAX_PATH - 1] = '\0';
    ctx->seen_count++;
}

static void readdir_cb(const char *path, const veloxfs_stat_t *st,
                        int is_dir, void *user_data) {
    (void)is_dir;
    struct readdir_ctx *ctx = (struct readdir_ctx*)user_data;
    const char *rel;

    if (strcmp(ctx->dir_path, "/") == 0)
        rel = path + 1;  /* skip leading '/' */
    else {
        if (strncmp(path, ctx->dir_path, ctx->dir_path_len) != 0) return;
        rel = path + ctx->dir_path_len;
        if (*rel == '/') rel++;
    }
    if (*rel == '\0') return;

    /* Only emit direct children, not deeper */
    char name[veloxfs_MAX_PATH];
    strncpy(name, rel, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char *slash = strchr(name, '/');
    int   child_is_dir = (slash != NULL);
    if (slash) *slash = '\0';

    if (child_is_dir) {
        if (readdir_already_seen(ctx, name)) return;
        readdir_mark_seen(ctx, name);
        struct stat stbuf;
        memset(&stbuf, 0, sizeof(stbuf));
        stbuf.st_mode  = S_IFDIR | 0755;
        stbuf.st_nlink = 2;
        ctx->filler(ctx->buf, name, &stbuf, 0);
    } else {
        struct stat stbuf;
        memset(&stbuf, 0, sizeof(stbuf));
        stbuf.st_mode    = S_IFREG | st->mode;
        stbuf.st_nlink   = 1;
        stbuf.st_size    = (off_t)st->size;
        stbuf.st_uid     = st->uid;
        stbuf.st_gid     = st->gid;
        stbuf.st_ctime   = (time_t)st->ctime;
        stbuf.st_mtime   = (time_t)st->mtime;
        stbuf.st_atime   = (time_t)st->atime;
        stbuf.st_blksize = (blksize_t)veloxfs_BS(&g.fs);
        ctx->filler(ctx->buf, name, &stbuf, 0);
    }
}

static int veloxfs_fuse_readdir(const char *path, void *buf,
                                 fuse_fill_dir_t filler, off_t offset,
                                 struct fuse_file_info *fi) {
    (void)offset; (void)fi;

    filler(buf, ".",  NULL, 0);
    filler(buf, "..", NULL, 0);

    struct readdir_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buf         = buf;
    ctx.filler      = filler;
    ctx.dir_path    = path;
    ctx.dir_path_len = strlen(path);
    if (ctx.dir_path_len > 0 && path[ctx.dir_path_len - 1] == '/')
        ctx.dir_path_len--;

    FS_LOCK();
    veloxfs_list(&g.fs, path, readdir_cb, &ctx);
    FS_UNLOCK();
    return 0;
}

/* =========================================================================
 * open / create / release
 * ======================================================================= */

static int veloxfs_fuse_open(const char *path, struct fuse_file_info *fi) {
    int flags = veloxfs_O_RDONLY;
    int acc   = fi->flags & O_ACCMODE;
    if (acc == O_WRONLY)  flags = veloxfs_O_WRONLY;
    if (acc == O_RDWR)    flags = veloxfs_O_RDWR;

    veloxfs_file *vf = (veloxfs_file*)malloc(sizeof(veloxfs_file));
    if (!vf) return -ENOMEM;

    FS_LOCK();
    veloxfs_set_user(&g.fs,
                     fuse_get_context()->uid,
                     fuse_get_context()->gid);
    int ret = veloxfs_open(&g.fs, path, flags, vf);
    FS_UNLOCK();

    if (ret != veloxfs_OK) { free(vf); return -ENOENT; }

    fi->fh         = (uint64_t)(uintptr_t)vf;
    fi->keep_cache = 0;  /* always 0: prevents stale kernel page-cache reads */
    return 0;
}

static int veloxfs_fuse_create(const char *path, mode_t mode,
                                struct fuse_file_info *fi) {
    FS_LOCK();
    veloxfs_set_user(&g.fs,
                     fuse_get_context()->uid,
                     fuse_get_context()->gid);
    int ret = veloxfs_create(&g.fs, path, (uint32_t)(mode & 0777));
    FS_UNLOCK();
    if (ret != veloxfs_OK) return -EACCES;

    return veloxfs_fuse_open(path, fi);
}

static int veloxfs_fuse_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    veloxfs_file *vf = (veloxfs_file*)(uintptr_t)fi->fh;
    if (!vf) return 0;

    FS_LOCK();
    veloxfs_close(vf);
    FS_UNLOCK();

    free(vf);
    fi->fh = 0;
    return 0;
}

/* =========================================================================
 * read / write
 * ======================================================================= */

static int veloxfs_fuse_read(const char *path, char *buf, size_t size,
                              off_t offset, struct fuse_file_info *fi) {
    (void)path;
    veloxfs_file *vf = (veloxfs_file*)(uintptr_t)fi->fh;
    if (!vf) return -EBADF;

    FS_LOCK();
    veloxfs_seek(vf, (int64_t)offset, 0 /*SEEK_SET*/);
    uint64_t bytes_read = 0;
    int ret = veloxfs_read(vf, buf, (uint64_t)size, &bytes_read);
    FS_UNLOCK();

    return (ret == veloxfs_OK) ? (int)bytes_read : -EIO;
}

static int veloxfs_fuse_write(const char *path, const char *buf, size_t size,
                               off_t offset, struct fuse_file_info *fi) {
    (void)path;
    veloxfs_file *vf = (veloxfs_file*)(uintptr_t)fi->fh;
    if (!vf) return -EBADF;

    FS_LOCK();
    veloxfs_seek(vf, (int64_t)offset, 0);
    int ret = veloxfs_write(vf, buf, (uint64_t)size);
    FS_UNLOCK();

    return (ret == veloxfs_OK) ? (int)size : -EIO;
}

/* =========================================================================
 * flush / fsync
 * ======================================================================= */

static int veloxfs_fuse_flush(const char *path, struct fuse_file_info *fi) {
    (void)path; (void)fi;
    FS_LOCK();
    veloxfs_sync(&g.fs);
    FS_UNLOCK();
    return 0;
}

static int veloxfs_fuse_fsync(const char *path, int datasync,
                               struct fuse_file_info *fi) {
    (void)path; (void)datasync; (void)fi;
    FS_LOCK();
    veloxfs_sync(&g.fs);
    FS_UNLOCK();
    disk_fsync_all();
    return 0;
}

/* =========================================================================
 * truncate / ftruncate
 * ======================================================================= */

static int veloxfs_fuse_truncate(const char *path, off_t size) {
    veloxfs_file vf;
    FS_LOCK();
    veloxfs_set_user(&g.fs,
                     fuse_get_context()->uid,
                     fuse_get_context()->gid);
    int ret = veloxfs_open(&g.fs, path, veloxfs_O_RDWR, &vf);
    if (ret == veloxfs_OK) {
        ret = veloxfs_truncate_handle(&vf, (uint64_t)size);
        veloxfs_close(&vf);
    }
    FS_UNLOCK();
    return (ret == veloxfs_OK) ? 0 : -EIO;
}

static int veloxfs_fuse_ftruncate(const char *path, off_t size,
                                   struct fuse_file_info *fi) {
    (void)path;
    veloxfs_file *vf = (veloxfs_file*)(uintptr_t)fi->fh;
    if (!vf) return -EBADF;
    FS_LOCK();
    int ret = veloxfs_truncate_handle(vf, (uint64_t)size);
    FS_UNLOCK();
    return (ret == veloxfs_OK) ? 0 : -EIO;
}

/* =========================================================================
 * unlink / mkdir / rmdir / rename
 * ======================================================================= */

static int veloxfs_fuse_unlink(const char *path) {
    FS_LOCK();
    veloxfs_set_user(&g.fs, fuse_get_context()->uid, fuse_get_context()->gid);
    int ret = veloxfs_delete(&g.fs, path);
    FS_UNLOCK();
    if (ret == veloxfs_ERR_NOT_FOUND) return -ENOENT;
    if (ret == veloxfs_ERR_PERMISSION) return -EACCES;
    return (ret == veloxfs_OK) ? 0 : -EIO;
}

static int veloxfs_fuse_mkdir(const char *path, mode_t mode) {
    FS_LOCK();
    veloxfs_set_user(&g.fs, 
                     fuse_get_context()->uid, 
                     fuse_get_context()->gid);

    /* * We create a 'virtual' directory by adding a path entry.
     * If your veloxfs_create implementation doesn't support 
     * a S_IFDIR flag, we can just create the path as a 0-byte entry.
     */
    int ret = veloxfs_create(&g.fs, path, (uint32_t)(mode & 0777));
    
    if (ret == veloxfs_OK) {
        veloxfs_sync(&g.fs); // Ensure the new path is flushed to disk
    }

    FS_UNLOCK();
    return (ret == veloxfs_OK) ? 0 : -EACCES;
}

static int veloxfs_fuse_rmdir(const char *path) {
    /* Simple implementation: just delete the directory entry */
    FS_LOCK();
    veloxfs_set_user(&g.fs, fuse_get_context()->uid, fuse_get_context()->gid);
    int ret = veloxfs_delete(&g.fs, path);
    FS_UNLOCK();
    if (ret == veloxfs_ERR_NOT_FOUND) return -ENOENT;
    return (ret == veloxfs_OK) ? 0 : -EIO;
}

static int veloxfs_fuse_rename(const char *old_path, const char *new_path) {
    FS_LOCK();
    veloxfs_set_user(&g.fs, fuse_get_context()->uid, fuse_get_context()->gid);
    /* If destination exists, remove it first */
    veloxfs_delete(&g.fs, new_path);
    int ret = veloxfs_rename(&g.fs, old_path, new_path);
    FS_UNLOCK();
    if (ret == veloxfs_ERR_NOT_FOUND) return -ENOENT;
    return (ret == veloxfs_OK) ? 0 : -EIO;
}

/* =========================================================================
 * chmod / chown / utimens
 * ======================================================================= */

static int veloxfs_fuse_chmod(const char *path, mode_t mode) {
    FS_LOCK();
    veloxfs_set_user(&g.fs, fuse_get_context()->uid, fuse_get_context()->gid);
    int ret = veloxfs_chmod(&g.fs, path, (uint32_t)(mode & 07777));
    FS_UNLOCK();
    if (ret == veloxfs_ERR_NOT_FOUND) return -ENOENT;
    if (ret == veloxfs_ERR_PERMISSION) return -EACCES;
    return (ret == veloxfs_OK) ? 0 : -EIO;
}

static int veloxfs_fuse_chown(const char *path, uid_t uid, gid_t gid) {
    FS_LOCK();
    veloxfs_set_user(&g.fs, fuse_get_context()->uid, fuse_get_context()->gid);
    int ret = veloxfs_chown(&g.fs, path, (uint32_t)uid, (uint32_t)gid);
    FS_UNLOCK();
    if (ret == veloxfs_ERR_NOT_FOUND) return -ENOENT;
    if (ret == veloxfs_ERR_PERMISSION) return -EACCES;
    return (ret == veloxfs_OK) ? 0 : -EIO;
}

static int veloxfs_fuse_utimens(const char *path, const struct timespec tv[2]) {
    FS_LOCK();
    veloxfs_stat_t st;
    int ret = veloxfs_stat(&g.fs, path, &st);
    if (ret == veloxfs_OK) {
        /* Find inode and update times directly */
        for (uint64_t i = 0; i < g.fs.num_dirents; i++) {
            if (g.fs.directory[i].inode_num != 0 &&
                strcmp(g.fs.directory[i].path, path) == 0) {
                uint64_t inum = g.fs.directory[i].inode_num;
                if (inum > 0 && inum <= g.fs.num_inodes) {
                    veloxfs_inode *in = &g.fs.inodes[inum - 1];
                    if (in->inode_num != 0) {
                        in->atime = (uint64_t)tv[0].tv_sec;
                        in->mtime = (uint64_t)tv[1].tv_sec;
                        g.fs.dirty_inodes = 1;
                    }
                }
                break;
            }
        }
    }
    FS_UNLOCK();
    return 0;
}

/* =========================================================================
 * statfs
 * ======================================================================= */

static int veloxfs_fuse_statfs(const char *path, struct statvfs *stbuf) {
    (void)path;
    uint64_t total = 0, used = 0, free_blocks = 0;
    FS_LOCK();
    veloxfs_statfs(&g.fs, &total, &used, &free_blocks);
    FS_UNLOCK();

    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->f_bsize   = veloxfs_BS(&g.fs);
    stbuf->f_frsize  = veloxfs_BS(&g.fs);
    stbuf->f_blocks  = total;
    stbuf->f_bfree   = free_blocks;
    stbuf->f_bavail  = free_blocks;
    stbuf->f_namemax = veloxfs_MAX_PATH - 1;
    return 0;
}

/* =========================================================================
 * FUSE operations table
 * ======================================================================= */

static struct fuse_operations veloxfs_fuse_ops = {
    .destroy   = fuse_veloxfs_destroy,
    .getattr   = veloxfs_fuse_getattr,
    .readdir   = veloxfs_fuse_readdir,
    .open      = veloxfs_fuse_open,
    .create    = veloxfs_fuse_create,
    .read      = veloxfs_fuse_read,
    .write     = veloxfs_fuse_write,
    .flush     = veloxfs_fuse_flush,
    .release   = veloxfs_fuse_release,
    .fsync     = veloxfs_fuse_fsync,
    .truncate  = veloxfs_fuse_truncate,
    .ftruncate = veloxfs_fuse_ftruncate,
    .unlink    = veloxfs_fuse_unlink,
    .mkdir     = veloxfs_fuse_mkdir,
    .rmdir     = veloxfs_fuse_rmdir,
    .rename    = veloxfs_fuse_rename,
    .chmod     = veloxfs_fuse_chmod,
    .chown     = veloxfs_fuse_chown,
    .utimens   = veloxfs_fuse_utimens,
    .statfs    = veloxfs_fuse_statfs,
};

/* =========================================================================
 * Format utility
 * ======================================================================= */

static int format_disk(const char *path, uint64_t blocks,
                        uint32_t block_size, uint32_t storage_hint) {
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    veloxfs_io io = { disk_read_cb, disk_write_cb, &fd };

    const char *mode_name = "HDD";
    if (storage_hint == veloxfs_STORAGE_NAND)  mode_name = "NAND";
    if (storage_hint == veloxfs_STORAGE_FLASH) mode_name = "Flash";
    if (block_size <= 4096)                    mode_name = "NVMe/SSD";

    printf("Formatting %s\n", path);
    printf("  Mode       : %s\n", mode_name);
    printf("  Block size : %u bytes (%u KB)\n", block_size, block_size / 1024);
    printf("  Blocks     : %lu\n", (unsigned long)blocks);
    printf("  Capacity   : %.2f MB\n",
           (double)blocks * block_size / (1024.0 * 1024.0));

    int ret = veloxfs_format_ex(io, blocks, 1, block_size, storage_hint);
    fsync(fd);
    close(fd);

    if (ret != veloxfs_OK) {
        fprintf(stderr, "Format failed: %d\n", ret);
        return 1;
    }

    printf("Format complete!\n");
    printf("  Journaling : ENABLED\n");
    printf("  Allocation : Extent-first + FAT overflow (v6)\n");
    printf("  Tip: Mount with: -o big_writes,max_write=1048576,max_read=1048576\n");
    return 0;
}

static uint64_t detect_device_size(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return 0; }
    uint64_t dev_size = 0;
    if (ioctl(fd, BLKGETSIZE64, &dev_size) < 0) {
        struct stat st;
        if (fstat(fd, &st) == 0) dev_size = (uint64_t)st.st_size;
    }
    close(fd);
    return dev_size;
}

/* =========================================================================
 * --stats utility
 * ======================================================================= */

static int show_stats(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    veloxfs_io io = { disk_read_cb, disk_write_cb, &fd };
    veloxfs_handle fs;

    if (veloxfs_mount(&fs, io) != veloxfs_OK) {
        fprintf(stderr, "Failed to mount (try formatting first)\n");
        close(fd);
        return 1;
    }

    veloxfs_alloc_stats stats;
    veloxfs_alloc_stats_get(&fs, &stats);

    printf("\n=== veloxfs v6 Statistics ===\n\n");
    printf("Storage mode : %s\n", storage_hint_name(fs.super.storage_hint));
    printf("Block size   : %u bytes (%u KB)\n",
           fs.super.block_size, fs.super.block_size / 1024);
    printf("Total        : %lu blocks (%.2f MB)\n",
           (unsigned long)stats.total_blocks,
           (double)stats.total_blocks * fs.super.block_size / (1024.0 * 1024.0));
    printf("Used         : %lu blocks (%.1f%%)\n",
           (unsigned long)stats.used_blocks,
           100.0 * stats.used_blocks / (double)(stats.total_blocks + 1));
    printf("Free         : %lu blocks\n", (unsigned long)stats.free_blocks);
    printf("Extent files : %lu\n", (unsigned long)stats.extent_files);
    printf("FAT overflow : %lu (fragmented beyond 4 extents)\n",
           (unsigned long)stats.fat_overflow);
    printf("Avg extents  : %.2f per file\n", stats.avg_extents);

    veloxfs_unmount(&fs);
    close(fd);
    return 0;
}

/* =========================================================================
 * main
 * ======================================================================= */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "veloxfs v6 -- Storage-Optimized FUSE Filesystem\n"
        "================================================\n\n"
        "FORMAT (choose one):\n"
        "  %s --format-hdd   <disk>   64 KB blocks, best for HDD (default)\n"
        "  %s --format-nand  <disk>  128 KB blocks, best for NAND flash\n"
        "  %s --format-flash <disk>   64 KB blocks, for NOR/managed flash\n"
        "  %s --format-nvme  <disk>    4 KB blocks, NVMe/SSD compat mode\n"
        "  %s --format       <disk>   alias for --format-hdd\n\n"
        "MOUNT:\n"
        "  %s <disk> <mountpoint> [FUSE options]\n\n"
        "STATS:\n"
        "  %s --stats <disk>\n\n"
        "RECOMMENDED MOUNT OPTIONS:\n"
        "  -o big_writes,max_write=1048576,max_read=1048576\n\n"
        "EXAMPLE (HDD):\n"
        "  dd if=/dev/zero of=veloxfs.img bs=1M count=1024\n"
        "  %s --format-hdd veloxfs.img\n"
        "  mkdir /tmp/mnt\n"
        "  %s veloxfs.img /tmp/mnt -o big_writes,max_write=1048576,max_read=1048576\n"
        "  cp /path/to/largefile /tmp/mnt/\n"
        "  fusermount -u /tmp/mnt\n\n"
        "EXAMPLE (NAND block device):\n"
        "  %s --format-nand /dev/mtdblock0\n"
        "  %s /dev/mtdblock0 /mnt/data \\\n"
        "      -o big_writes,max_write=1048576,max_read=1048576,direct_io\n\n",
        prog, prog, prog, prog, prog,
        prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    if (argc < 3) { print_usage(argv[0]); return 1; }

    /* ---- Format subcommands ---- */
    struct { const char *flag; uint32_t bs; uint32_t hint; } fmt_modes[] = {
        { "--format",       veloxfs_BS_HDD,  veloxfs_STORAGE_HDD   },
        { "--format-hdd",   veloxfs_BS_HDD,  veloxfs_STORAGE_HDD   },
        { "--format-nand",  veloxfs_BS_NAND, veloxfs_STORAGE_NAND  },
        { "--format-flash", veloxfs_BS_FLASH,veloxfs_STORAGE_FLASH  },
        { "--format-nvme",  veloxfs_BS_NVME, veloxfs_STORAGE_AUTO  },
        { NULL, 0, 0 }
    };

    if (argc == 3) {
        for (int m = 0; fmt_modes[m].flag; m++) {
            if (strcmp(argv[1], fmt_modes[m].flag) == 0) {
                const char *disk = argv[2];
                uint64_t dev_size = detect_device_size(disk);
                if (dev_size == 0) {
                    fprintf(stderr, "Cannot determine size of %s\n", disk);
                    return 1;
                }
                uint64_t blocks = dev_size / fmt_modes[m].bs;
                if (blocks < 64) {
                    fprintf(stderr, "Device too small: %lu bytes "
                            "(need at least %u blocks of %u KB)\n",
                            (unsigned long)dev_size, 64, fmt_modes[m].bs / 1024);
                    return 1;
                }
                return format_disk(disk, blocks, fmt_modes[m].bs, fmt_modes[m].hint);
            }
        }

        if (strcmp(argv[1], "--stats") == 0)
            return show_stats(argv[2]);
    }

    /* ---- Mount ---- */
    if (argc < 3) { print_usage(argv[0]); return 1; }

    g.disk_path = argv[1];
    g.disk_fd   = -1;
    pthread_mutex_init(&g.lock, NULL);

    g.disk_fd = open(g.disk_path, O_RDWR);
    if (g.disk_fd < 0) { perror("Failed to open disk"); return 1; }

    /* Sequential read-ahead hint to the kernel (benefits HDD reads) */
#ifdef POSIX_FADV_SEQUENTIAL
    posix_fadvise(g.disk_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    veloxfs_io io = { disk_read_cb, disk_write_cb, &g.disk_fd };

    printf("Mounting veloxfs v6 from %s ...\n", g.disk_path);
    int ret = veloxfs_mount(&g.fs, io);
    if (ret != veloxfs_OK) {
        fprintf(stderr, "Mount failed (%d). Run: %s --format-hdd %s\n",
                ret, argv[0], g.disk_path);
        close(g.disk_fd);
        return 1;
    }

    printf("  Version      : veloxfs v6 (extent-first allocation)\n");
    printf("  Storage mode : %s\n", storage_hint_name(g.fs.super.storage_hint));
    printf("  Block size   : %u bytes (%u KB)\n",
           g.fs.super.block_size, g.fs.super.block_size / 1024);
    printf("  Journal      : %s\n",
           g.fs.super.journal_enabled ? "ENABLED" : "DISABLED");

    printf("Running fsck...\n");
    veloxfs_fsck(&g.fs);

    veloxfs_alloc_stats stats;
    veloxfs_alloc_stats_get(&g.fs, &stats);
    printf("Usage: %lu / %lu blocks (%.1f%% used)\n",
           (unsigned long)stats.used_blocks,
           (unsigned long)stats.total_blocks,
           100.0 * stats.used_blocks / (double)(stats.total_blocks + 1));

    printf("NOTE: For best throughput, mount with:\n");
    printf("  -o big_writes,max_write=1048576,max_read=1048576\n");

    /* Build FUSE argv: program + mountpoint + [extra opts] */
    int fuse_argc = argc - 1;
    char **fuse_argv = (char**)malloc(sizeof(char*) * (size_t)(fuse_argc + 1));
    if (!fuse_argv) { perror("malloc"); return 1; }
    fuse_argv[0] = argv[0];
    for (int i = 2; i < argc; i++)
        fuse_argv[i - 1] = argv[i];
    fuse_argv[fuse_argc] = NULL;

    printf("Starting FUSE (multithreaded + global lock)...\n");
    ret = fuse_main(fuse_argc, fuse_argv, &veloxfs_fuse_ops, NULL);

    free(fuse_argv);
    pthread_mutex_destroy(&g.lock);
    return ret;
}
