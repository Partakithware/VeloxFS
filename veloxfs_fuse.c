/*
 * veloxfs_fuse.c - FUSE adapter for veloxfs v5 (Linked-list allocation)
 *
 * BUGS FIXED IN THIS REVISION:
 *  1. DATA LOSS (PRIMARY): veloxfs_write navigation skipped re-navigation
 *     when current_block_idx==target_block_idx but current_block was a stale
 *     0-pointer (empty file's first write).  FAT chain was built and inode
 *     size set, but the write loop exited immediately — kernel page cache
 *     served reads while mounted; remount showed all-zeros.  Fixed in
 *     veloxfs.h by also re-navigating when current_block==0 && first_block!=0.
 *
 *  2. THREAD SAFETY — DISK I/O: fseeko+fwrite is NOT atomic. Two concurrent
 *     FUSE threads race: T1 seeks to offset A, T2 seeks to offset B, T1 writes
 *     at B, T2 writes at A => data lands at wrong block.  Fixed: FILE* replaced
 *     by int fd + pread()/pwrite() which are atomic w.r.t. position.
 *
 *  3. THREAD SAFETY — IN-MEMORY STATE: FAT allocation, inode table, and
 *     directory are shared across FUSE threads with no lock.  Two threads
 *     can double-allocate the same block.  Fixed: global pthread_mutex_t
 *     serialises all veloxfs API calls.
 *
 *  4. SEEK PAST END: veloxfs_seek rejected offset>size for write-capable
 *     handles.  Fixed in veloxfs.h to allow write-path seeks past current size.
 *
 *  5. STALE KERNEL CACHE: keep_cache=1 allowed the kernel to serve old pages
 *     on re-open, hiding the write bug while mounted.  Now 0.
 *
 *  6. MISSING fsync HANDLER: applications calling fsync() got a no-op.
 *     Now properly flushes all metadata + disk file.
 *
 *  7. FORMAT PHANTOM BLOCKS: +512 block gap in veloxfs_format.  Removed.
 *
 * COMPILE:
 *   gcc -Wall -O2 -pthread veloxfs_fuse.c -o veloxfs_fuse \
 *       `pkg-config fuse --cflags --libs`
 *
 * USAGE:
 *   dd if=/dev/zero of=veloxfs.img bs=1M count=512
 *   ./veloxfs_fuse --format veloxfs.img
 *   mkdir /tmp/mnt
 *   ./veloxfs_fuse veloxfs.img /tmp/mnt -o big_writes,max_write=131072
 *   cp largefile.bin /tmp/mnt/
 *   fusermount -u /tmp/mnt
 *   ./veloxfs_fuse veloxfs.img /tmp/mnt   # remount -- data intact
 *   or for testing include -d
 *   ./veloxfs_fuse veloxfs.img /tmp/mnt -d -o big_writes,max_write=131072,max_read=131072
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

#include "veloxfs.h"

/* =========================================================================
 * Global FUSE state
 * ======================================================================= */

static struct {
    int              disk_fd;   /* raw fd -- pread/pwrite are thread-safe    */
    veloxfs_handle   fs;
    char            *disk_path;
    pthread_mutex_t  lock;      /* serialises ALL veloxfs API calls           */
} g_fuse_state;

#define FS_LOCK()   pthread_mutex_lock(&g_fuse_state.lock)
#define FS_UNLOCK() pthread_mutex_unlock(&g_fuse_state.lock)

/* =========================================================================
 * Thread-safe disk I/O -- pread/pwrite are POSIX-guaranteed atomic
 * ======================================================================= */

static int disk_read_cb(void *user, uint64_t offset, void *buf, uint32_t size) {
    int fd = *(int *)user;
    ssize_t n = pread(fd, buf, size, (off_t)offset);
    return (n == (ssize_t)size) ? 0 : -1;
}

static int disk_write_cb(void *user, uint64_t offset,
                          const void *buf, uint32_t size) {
    int fd = *(int *)user;
    ssize_t n = pwrite(fd, buf, size, (off_t)offset);
    return (n == (ssize_t)size) ? 0 : -1;
}

static void disk_fsync(void) {
    if (g_fuse_state.disk_fd >= 0)
        fsync(g_fuse_state.disk_fd);
}

/* =========================================================================
 * FUSE destroy (called once, no other threads active)
 * ======================================================================= */

static void fuse_veloxfs_destroy(void *private_data) {
    (void)private_data;
    fprintf(stderr, "[veloxfs] destroy: flushing and unmounting\n");
    /* No lock needed -- FUSE guarantees single-threaded at destroy */
    veloxfs_sync(&g_fuse_state.fs);
    disk_fsync();
    veloxfs_unmount(&g_fuse_state.fs);
    if (g_fuse_state.disk_fd >= 0) {
        close(g_fuse_state.disk_fd);
        g_fuse_state.disk_fd = -1;
    }
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
    int ret = veloxfs_stat(&g_fuse_state.fs, path, &tfs);

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
        stbuf->st_blksize = veloxfs_BLOCK_SIZE;
        FS_UNLOCK();
        return 0;
    }

    /* Check implicit directory: any file stored under path/ */
    char prefix[veloxfs_MAX_PATH];
    snprintf(prefix, sizeof(prefix), "%s/", path);
    size_t plen = strlen(prefix);

    for (uint64_t i = 0; i < g_fuse_state.fs.num_dirents; i++) {
        if (g_fuse_state.fs.directory[i].inode_num != 0 &&
            strncmp(g_fuse_state.fs.directory[i].path, prefix, plen) == 0) {
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

/*
 * readdir context — tracks which subdirectory names we have already emitted
 * so we don't produce duplicate entries when a directory contains many files.
 * We use a small fixed hash set of the names already filled.
 */
#define READDIR_SEEN_SIZE 512
struct readdir_ctx {
    void           *buf;
    fuse_fill_dir_t filler;
    const char     *dir_path;
    size_t          dir_path_len;
    /* simple open-addressing hash set of already-emitted child dir names */
    char seen[READDIR_SEEN_SIZE][veloxfs_MAX_PATH];
    int  seen_count;
};

static int readdir_already_seen(struct readdir_ctx *ctx, const char *name) {
    if (ctx->seen_count == 0) return 0;
    uint64_t h = 14695981039346656037ULL;
    for (const char *p = name; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    uint64_t slot = h % READDIR_SEEN_SIZE;
    for (int i = 0; i < READDIR_SEEN_SIZE; i++) {
        if (ctx->seen[slot][0] == '\0') return 0;          /* empty slot */
        if (strcmp(ctx->seen[slot], name) == 0) return 1;  /* found */
        slot = (slot + 1) % READDIR_SEEN_SIZE;
    }
    return 0; /* table full — shouldn't happen for reasonable dir sizes */
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

static void readdir_cb(const char *path, const veloxfs_stat_t *st, int is_dir,
                       void *user_data) {
    (void)is_dir;
    struct readdir_ctx *ctx = (struct readdir_ctx *)user_data;

    /* Strip the listing directory prefix to get the relative portion */
    const char *rel;
    if (strcmp(ctx->dir_path, "/") == 0) {
        if (path[0] != '/') return;
        rel = path + 1;                     /* skip the leading '/' */
    } else {
        if (strncmp(path, ctx->dir_path, ctx->dir_path_len) != 0) return;
        rel = path + ctx->dir_path_len;
        if (*rel != '/') return;
        rel++;                              /* skip the separator '/' */
    }

    if (*rel == '\0') return;

    /* Find the first path component (everything up to the next '/' or end) */
    const char *slash = strchr(rel, '/');

    if (slash == NULL) {
        /* ---- Direct file child ---- */
        if (strcmp(rel, ".veloxfs_dir") == 0) return; /* skip dir markers */

        struct stat stbuf;
        memset(&stbuf, 0, sizeof(stbuf));
        stbuf.st_mode  = S_IFREG | st->mode;
        stbuf.st_size  = (off_t)st->size;
        stbuf.st_uid   = st->uid;
        stbuf.st_gid   = st->gid;
        stbuf.st_mtime = (time_t)st->mtime;
        ctx->filler(ctx->buf, rel, &stbuf, 0);
    } else {
        /*
         * ---- Implicit subdirectory ----
         * rel looks like "SubDir/..." — extract just "SubDir" and emit it
         * as a directory entry once (guard duplicates with the seen set).
         */
        size_t name_len = (size_t)(slash - rel);
        if (name_len == 0 || name_len >= veloxfs_MAX_PATH) return;

        char dir_name[veloxfs_MAX_PATH];
        memcpy(dir_name, rel, name_len);
        dir_name[name_len] = '\0';

        if (readdir_already_seen(ctx, dir_name)) return;
        readdir_mark_seen(ctx, dir_name);

        struct stat stbuf;
        memset(&stbuf, 0, sizeof(stbuf));
        stbuf.st_mode  = S_IFDIR | 0755;
        stbuf.st_nlink = 2;
        ctx->filler(ctx->buf, dir_name, &stbuf, 0);
    }
}

static int veloxfs_fuse_readdir(const char *path, void *buf,
                                 fuse_fill_dir_t filler, off_t offset,
                                 struct fuse_file_info *fi) {
    (void)offset; (void)fi;

    filler(buf, ".",  NULL, 0);
    filler(buf, "..", NULL, 0);

    /* Normalise path: strip trailing slash unless root */
    size_t plen = strlen(path);
    char norm_path[veloxfs_MAX_PATH];
    strncpy(norm_path, path, sizeof(norm_path) - 1);
    norm_path[sizeof(norm_path) - 1] = '\0';
    if (plen > 1 && norm_path[plen - 1] == '/') {
        norm_path[plen - 1] = '\0';
        plen--;
    }

    struct readdir_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buf          = buf;
    ctx.filler       = filler;
    ctx.dir_path     = norm_path;
    ctx.dir_path_len = plen;

    FS_LOCK();
    veloxfs_list(&g_fuse_state.fs, norm_path, readdir_cb, &ctx);
    FS_UNLOCK();

    return 0;
}

/* =========================================================================
 * open / create
 * ======================================================================= */

static int do_open(const char *path, struct fuse_file_info *fi, uid_t uid,
                   gid_t gid) {
    veloxfs_set_user(&g_fuse_state.fs, uid, gid);

    int flags;
    switch (fi->flags & O_ACCMODE) {
        case O_RDONLY: flags = veloxfs_O_RDONLY; break;
        case O_WRONLY: flags = veloxfs_O_WRONLY; break;
        default:       flags = veloxfs_O_RDWR;   break;
    }

    veloxfs_file *file = calloc(1, sizeof(veloxfs_file));
    if (!file) return -ENOMEM;

    int ret = veloxfs_open(&g_fuse_state.fs, path, flags, file);
    if (ret != veloxfs_OK) {
        free(file);
        return (ret == veloxfs_ERR_PERMISSION) ? -EACCES : -EIO;
    }

    fi->fh         = (uint64_t)(uintptr_t)file;
    fi->direct_io  = 0;
    fi->keep_cache = 0; /* BUG FIX: was 1 -- stale cache masked write loss */
    return 0;
}

static int veloxfs_fuse_open(const char *path, struct fuse_file_info *fi) {
    struct fuse_context *ctx = fuse_get_context();
    FS_LOCK();
    int ret = do_open(path, fi, ctx->uid, ctx->gid);
    FS_UNLOCK();
    return ret;
}

static int veloxfs_fuse_create(const char *path, mode_t mode,
                                struct fuse_file_info *fi) {
    struct fuse_context *ctx = fuse_get_context();

    FS_LOCK();
    veloxfs_set_user(&g_fuse_state.fs, ctx->uid, ctx->gid);
    int ret = veloxfs_create(&g_fuse_state.fs, path, (uint32_t)mode);
    if (ret != veloxfs_OK && ret != veloxfs_ERR_EXISTS) {
        FS_UNLOCK();
        return (ret == veloxfs_ERR_EXISTS) ? -EEXIST : -EIO;
    }
    ret = do_open(path, fi, ctx->uid, ctx->gid);
    FS_UNLOCK();
    return ret;
}

/* =========================================================================
 * flush / release / fsync
 * ======================================================================= */

static int veloxfs_fuse_flush(const char *path, struct fuse_file_info *fi) {
    (void)path; (void)fi;
    FS_LOCK();
    veloxfs_sync(&g_fuse_state.fs);
    FS_UNLOCK();
    disk_fsync();
    return 0;
}

static int veloxfs_fuse_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    veloxfs_file *file = (veloxfs_file *)(uintptr_t)fi->fh;
    if (!file) return 0;

    FS_LOCK();
    veloxfs_close(file);              /* finalize inode mtime, sets dirty     */
    veloxfs_sync(&g_fuse_state.fs);   /* write FAT + inodes + dir to img      */
    FS_UNLOCK();
    disk_fsync();                     /* flush OS kernel buffer to device     */
    free(file);
    return 0;
}

static int veloxfs_fuse_fsync(const char *path, int datasync,
                               struct fuse_file_info *fi) {
    (void)path; (void)datasync; (void)fi;
    FS_LOCK();
    veloxfs_sync(&g_fuse_state.fs);
    FS_UNLOCK();
    disk_fsync();
    return 0;
}

/* =========================================================================
 * read / write
 * ======================================================================= */

static int veloxfs_fuse_read(const char *path, char *buf, size_t size,
                              off_t offset, struct fuse_file_info *fi) {
    (void)path;
    veloxfs_file *file = (veloxfs_file *)(uintptr_t)fi->fh;

    FS_LOCK();
    if (veloxfs_seek(file, (int64_t)offset, SEEK_SET) != veloxfs_OK) {
        FS_UNLOCK();
        return -EIO;
    }
    uint64_t bytes_read = 0;
    int ret = veloxfs_read(file, buf, (uint64_t)size, &bytes_read);
    FS_UNLOCK();

    return (ret == veloxfs_OK) ? (int)bytes_read : -EIO;
}

static int veloxfs_fuse_write(const char *path, const char *buf, size_t size,
                               off_t offset, struct fuse_file_info *fi) {
    (void)path;
    veloxfs_file *file = (veloxfs_file *)(uintptr_t)fi->fh;

    FS_LOCK();
    /*
     * Seek to the write offset.  veloxfs_seek now allows seeking past
     * inode->size for write-capable handles (fixed in veloxfs.h) so that
     * veloxfs_write can handle the chain extension itself.
     */
    if (veloxfs_seek(file, (int64_t)offset, SEEK_SET) != veloxfs_OK) {
        FS_UNLOCK();
        return -EIO;
    }
    int ret = veloxfs_write(file, buf, (uint64_t)size);
    FS_UNLOCK();

    return (ret == veloxfs_OK) ? (int)size : -ENOSPC;
}

/* =========================================================================
 * truncate (path-based and fd-based)
 * ======================================================================= */

static int veloxfs_fuse_truncate(const char *path, off_t size) {
    struct fuse_context *ctx = fuse_get_context();

    FS_LOCK();
    veloxfs_set_user(&g_fuse_state.fs, ctx->uid, ctx->gid);

    veloxfs_file file;
    int ret = veloxfs_open(&g_fuse_state.fs, path, veloxfs_O_RDWR, &file);
    if (ret != veloxfs_OK) {
        FS_UNLOCK();
        return (ret == veloxfs_ERR_NOT_FOUND) ? -ENOENT : -EIO;
    }

    ret = veloxfs_truncate_handle(&file, (uint64_t)size);
    veloxfs_close(&file);
    veloxfs_sync(&g_fuse_state.fs);
    FS_UNLOCK();
    disk_fsync();

    return (ret == veloxfs_OK) ? 0 : -EIO;
}

static int veloxfs_fuse_ftruncate(const char *path, off_t size,
                                   struct fuse_file_info *fi) {
    (void)path;
    veloxfs_file *file = (veloxfs_file *)(uintptr_t)fi->fh;

    FS_LOCK();
    int ret = veloxfs_truncate_handle(file, (uint64_t)size);
    FS_UNLOCK();

    return (ret == veloxfs_OK) ? 0 : -EIO;
}

/* =========================================================================
 * unlink / mkdir / rmdir / rename
 * ======================================================================= */

static int veloxfs_fuse_unlink(const char *path) {
    struct fuse_context *ctx = fuse_get_context();

    FS_LOCK();
    veloxfs_set_user(&g_fuse_state.fs, ctx->uid, ctx->gid);
    int ret = veloxfs_delete(&g_fuse_state.fs, path);
    if (ret == veloxfs_OK) veloxfs_sync(&g_fuse_state.fs);
    FS_UNLOCK();
    if (ret == veloxfs_OK) disk_fsync();

    if (ret == veloxfs_OK)             return 0;
    if (ret == veloxfs_ERR_NOT_FOUND)  return -ENOENT;
    if (ret == veloxfs_ERR_PERMISSION) return -EACCES;
    return -EIO;
}

static int veloxfs_fuse_mkdir(const char *path, mode_t mode) {
    struct fuse_context *ctx = fuse_get_context();

    /*
     * veloxfs is a flat-file namespace: directories have no inode of their own.
     * getattr detects a directory by finding at least one file under "path/".
     * To make empty directories survive mount/unmount we auto-create a hidden
     * marker file ".veloxfs_dir" inside every new directory.
     * readdir already skips this file so users never see it.
     */
    char marker[veloxfs_MAX_PATH];
    int n = snprintf(marker, sizeof(marker), "%s/.veloxfs_dir",
                     strcmp(path, "/") == 0 ? "" : path);
    if (n < 0 || n >= (int)sizeof(marker))
        return -ENAMETOOLONG;

    FS_LOCK();
    veloxfs_set_user(&g_fuse_state.fs, ctx->uid, ctx->gid);

    /* Reject if directory already exists (any file under path/ exists) */
    char prefix[veloxfs_MAX_PATH];
    snprintf(prefix, sizeof(prefix), "%s/", path);
    size_t plen = strlen(prefix);
    for (uint64_t i = 0; i < g_fuse_state.fs.num_dirents; i++) {
        if (g_fuse_state.fs.directory[i].inode_num != 0 &&
            strncmp(g_fuse_state.fs.directory[i].path, prefix, plen) == 0) {
            FS_UNLOCK();
            return -EEXIST;
        }
    }

    /* Create the hidden marker file to anchor the directory on disk */
    int ret = veloxfs_create(&g_fuse_state.fs, marker, (uint32_t)(mode & 0777));
    if (ret == veloxfs_OK)
        veloxfs_sync(&g_fuse_state.fs);
    FS_UNLOCK();
    if (ret == veloxfs_OK) disk_fsync();

    if (ret == veloxfs_OK)                 return 0;
    if (ret == veloxfs_ERR_EXISTS)         return -EEXIST;
    if (ret == veloxfs_ERR_TOO_MANY_FILES) return -ENOSPC;
    return -EIO;
}

static int veloxfs_fuse_rmdir(const char *path) {
    /*
     * Remove the hidden marker and any remaining files inside the directory.
     * We delete everything under path/ so partial cleanup also works.
     */
    struct fuse_context *ctx = fuse_get_context();

    FS_LOCK();
    veloxfs_set_user(&g_fuse_state.fs, ctx->uid, ctx->gid);

    /* Collect all entries under this directory */
    char prefix[veloxfs_MAX_PATH];
    snprintf(prefix, sizeof(prefix), "%s/", path);
    size_t plen = strlen(prefix);

    /* Check emptiness: only allow .veloxfs_dir, no real files */
    for (uint64_t i = 0; i < g_fuse_state.fs.num_dirents; i++) {
        veloxfs_dirent *de = &g_fuse_state.fs.directory[i];
        if (de->inode_num == 0) continue;
        if (strncmp(de->path, prefix, plen) != 0) continue;
        /* Is this a non-marker file? */
        const char *rel = de->path + plen;
        if (strcmp(rel, ".veloxfs_dir") != 0) {
            FS_UNLOCK();
            return -ENOTEMPTY;
        }
    }

    /* Delete the marker (and anything else under path/ for safety) */
    int deleted = 0;
    int any_err = 0;
restart:
    for (uint64_t i = 0; i < g_fuse_state.fs.num_dirents; i++) {
        veloxfs_dirent *de = &g_fuse_state.fs.directory[i];
        if (de->inode_num == 0) continue;
        if (strncmp(de->path, prefix, plen) != 0) continue;
        char entry_path[veloxfs_MAX_PATH];
        strncpy(entry_path, de->path, sizeof(entry_path) - 1);
        entry_path[sizeof(entry_path) - 1] = '\0';
        int r = veloxfs_delete(&g_fuse_state.fs, entry_path);
        if (r == veloxfs_OK) { deleted++; goto restart; }
        else any_err = r;
    }

    if (deleted > 0 || any_err == 0)
        veloxfs_sync(&g_fuse_state.fs);
    FS_UNLOCK();
    if (deleted > 0) disk_fsync();

    if (any_err != 0) return -EIO;
    return 0;
}

static int veloxfs_fuse_rename(const char *old_path, const char *new_path) {
    struct fuse_context *ctx = fuse_get_context();

    FS_LOCK();
    veloxfs_set_user(&g_fuse_state.fs, ctx->uid, ctx->gid);

    /*
     * Determine whether old_path is a file or a directory.
     * A path is a directory if NO direct inode matches it but files exist
     * under "old_path/".  A plain file has a direct dirent match.
     */
    int is_dir = 0;
    {
        veloxfs_stat_t st;
        if (veloxfs_stat(&g_fuse_state.fs, old_path, &st) != veloxfs_OK) {
            /* No direct file match — check for directory */
            char prefix[veloxfs_MAX_PATH];
            snprintf(prefix, sizeof(prefix), "%s/", old_path);
            size_t plen = strlen(prefix);
            for (uint64_t i = 0; i < g_fuse_state.fs.num_dirents; i++) {
                if (g_fuse_state.fs.directory[i].inode_num != 0 &&
                    strncmp(g_fuse_state.fs.directory[i].path, prefix, plen) == 0) {
                    is_dir = 1;
                    break;
                }
            }
            if (!is_dir) {
                FS_UNLOCK();
                return -ENOENT;
            }
        }
    }

    int ret = veloxfs_OK;

    if (!is_dir) {
        /* ---- Simple file rename ---- */
        ret = veloxfs_rename(&g_fuse_state.fs, old_path, new_path);
    } else {
        /* ---- Directory rename: rewrite the prefix on every child ---- */

        /* Refuse if new_path already exists as a directory with real content */
        char new_prefix[veloxfs_MAX_PATH];
        snprintf(new_prefix, sizeof(new_prefix), "%s/", new_path);
        size_t new_plen = strlen(new_prefix);
        for (uint64_t i = 0; i < g_fuse_state.fs.num_dirents; i++) {
            veloxfs_dirent *de = &g_fuse_state.fs.directory[i];
            if (de->inode_num == 0) continue;
            if (strncmp(de->path, new_prefix, new_plen) == 0) {
                const char *rel = de->path + new_plen;
                if (strcmp(rel, ".veloxfs_dir") != 0) {
                    FS_UNLOCK();
                    return -EEXIST;
                }
            }
        }

        char old_prefix[veloxfs_MAX_PATH];
        snprintf(old_prefix, sizeof(old_prefix), "%s/", old_path);
        size_t old_plen = strlen(old_prefix);

        /*
         * Walk every dirent.  For each entry whose path starts with
         * old_prefix, build the new path by swapping the prefix and
         * updating the dirent in place.
         *
         * We cannot call veloxfs_rename() here because that checks for
         * ERR_EXISTS on the destination; instead we patch the dirent path
         * and the hash table directly.
         */
        int renamed = 0;
        for (uint64_t i = 0; i < g_fuse_state.fs.num_dirents; i++) {
            veloxfs_dirent *de = &g_fuse_state.fs.directory[i];
            if (de->inode_num == 0) continue;
            if (strncmp(de->path, old_prefix, old_plen) != 0) continue;

            /* Suffix after the old prefix (e.g. ".veloxfs_dir" or "file.txt") */
            const char *suffix = de->path + old_plen;

            char new_entry[veloxfs_MAX_PATH];
            int n = snprintf(new_entry, sizeof(new_entry), "%s/%s",
                             new_path, suffix);
            if (n < 0 || n >= (int)sizeof(new_entry)) {
                ret = veloxfs_ERR_INVALID;
                break;
            }

            /* Remove old hash entry, update path, insert new hash entry */
            char old_entry[veloxfs_MAX_PATH];
            strncpy(old_entry, de->path, sizeof(old_entry) - 1);
            old_entry[sizeof(old_entry) - 1] = '\0';

            strncpy(de->path, new_entry, veloxfs_MAX_PATH - 1);
            de->path[veloxfs_MAX_PATH - 1] = '\0';

            /* Update hash table: remove old slot, insert new */
            if (g_fuse_state.fs.dir_hash_table) {
                /* find and clear old slot */
                uint64_t h = 14695981039346656037ULL;
                for (const char *p = old_entry; *p; p++) {
                    h ^= (uint64_t)(unsigned char)(*p);
                    h *= 1099511628211ULL;
                }
                uint64_t slot = h % g_fuse_state.fs.dir_hash_size;
                for (uint64_t s = 0; s < g_fuse_state.fs.dir_hash_size; s++) {
                    uint64_t idx = g_fuse_state.fs.dir_hash_table[slot];
                    if (idx == UINT64_MAX) break;
                    if (idx == i) {
                        g_fuse_state.fs.dir_hash_table[slot] = UINT64_MAX;
                        break;
                    }
                    slot = (slot + 1) % g_fuse_state.fs.dir_hash_size;
                }
                /* insert new slot */
                h = 14695981039346656037ULL;
                for (const char *p = new_entry; *p; p++) {
                    h ^= (uint64_t)(unsigned char)(*p);
                    h *= 1099511628211ULL;
                }
                slot = h % g_fuse_state.fs.dir_hash_size;
                while (g_fuse_state.fs.dir_hash_table[slot] != UINT64_MAX)
                    slot = (slot + 1) % g_fuse_state.fs.dir_hash_size;
                g_fuse_state.fs.dir_hash_table[slot] = i;
            }

            g_fuse_state.fs.dirty_dir = 1;
            renamed++;
        }

        if (renamed == 0 && ret == veloxfs_OK)
            ret = veloxfs_ERR_NOT_FOUND;
    }

    if (ret == veloxfs_OK) veloxfs_sync(&g_fuse_state.fs);
    FS_UNLOCK();
    if (ret == veloxfs_OK) disk_fsync();

    if (ret == veloxfs_OK)             return 0;
    if (ret == veloxfs_ERR_NOT_FOUND)  return -ENOENT;
    if (ret == veloxfs_ERR_EXISTS)     return -EEXIST;
    return -EIO;
}

/* =========================================================================
 * chmod / chown / utimens
 * ======================================================================= */

static int veloxfs_fuse_chmod(const char *path, mode_t mode) {
    struct fuse_context *ctx = fuse_get_context();

    FS_LOCK();
    veloxfs_set_user(&g_fuse_state.fs, ctx->uid, ctx->gid);
    int ret = veloxfs_chmod(&g_fuse_state.fs, path, (uint32_t)mode);
    if (ret == veloxfs_OK) veloxfs_sync(&g_fuse_state.fs);
    FS_UNLOCK();

    if (ret == veloxfs_OK)             return 0;
    if (ret == veloxfs_ERR_NOT_FOUND)  return -ENOENT;
    if (ret == veloxfs_ERR_PERMISSION) return -EACCES;
    return -EIO;
}

static int veloxfs_fuse_chown(const char *path, uid_t uid, gid_t gid) {
    struct fuse_context *ctx = fuse_get_context();

    FS_LOCK();
    veloxfs_set_user(&g_fuse_state.fs, ctx->uid, ctx->gid);
    int ret = veloxfs_chown(&g_fuse_state.fs, path, uid, gid);
    if (ret == veloxfs_OK) veloxfs_sync(&g_fuse_state.fs);
    FS_UNLOCK();

    if (ret == veloxfs_OK)             return 0;
    if (ret == veloxfs_ERR_NOT_FOUND)  return -ENOENT;
    if (ret == veloxfs_ERR_PERMISSION) return -EACCES;
    return -EIO;
}

static int veloxfs_fuse_utimens(const char *path,
                                 const struct timespec tv[2]) {
    FS_LOCK();
    /* Find and update inode timestamps directly */
    for (uint64_t i = 0; i < g_fuse_state.fs.num_dirents; i++) {
        veloxfs_dirent *de = &g_fuse_state.fs.directory[i];
        if (de->inode_num == 0) continue;
        if (strcmp(de->path, path) != 0) continue;

        uint64_t inum = de->inode_num;
        if (inum > 0 && inum <= g_fuse_state.fs.num_inodes) {
            veloxfs_inode *inode = &g_fuse_state.fs.inodes[inum - 1];
            if (inode->inode_num != 0) {
                inode->atime = (uint64_t)tv[0].tv_sec;
                inode->mtime = (uint64_t)tv[1].tv_sec;
                g_fuse_state.fs.dirty_inodes = 1;
            }
        }
        break;
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
    veloxfs_statfs(&g_fuse_state.fs, &total, &used, &free_blocks);
    FS_UNLOCK();

    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->f_bsize   = veloxfs_BLOCK_SIZE;
    stbuf->f_frsize  = veloxfs_BLOCK_SIZE;
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
 * --format utility
 * ======================================================================= */

static int format_disk(const char *path, uint64_t blocks) {
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    veloxfs_io io = { disk_read_cb, disk_write_cb, &fd };

    printf("Formatting %s: %lu blocks (%.2f MB)\n",
           path, (unsigned long)blocks,
           (double)(blocks * veloxfs_BLOCK_SIZE) / (1024.0 * 1024.0));

    int ret = veloxfs_format(io, blocks, 1);
    fsync(fd);
    close(fd);

    if (ret != veloxfs_OK) {
        fprintf(stderr, "Format failed: %d\n", ret);
        return 1;
    }

    printf("Format complete!\n");
    printf("  Journaling : ENABLED\n");
    printf("  Allocation : Linked-list FAT-style (v5)\n");
    return 0;
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
        fprintf(stderr, "Failed to mount\n");
        close(fd);
        return 1;
    }

    veloxfs_alloc_stats stats;
    veloxfs_alloc_stats_get(&fs, &stats);

    printf("\n=== veloxfs v5 Statistics ===\n\n");
    printf("Total : %lu blocks (%.2f MB)\n",
           (unsigned long)stats.total_blocks,
           (double)(stats.total_blocks * veloxfs_BLOCK_SIZE) / (1024.0*1024.0));
    printf("Used  : %lu blocks (%.1f%%)\n",
           (unsigned long)stats.used_blocks,
           100.0 * stats.used_blocks / (double)stats.total_blocks);
    printf("Free  : %lu blocks\n", (unsigned long)stats.free_blocks);
    printf("Longest chain : %lu blocks\n", (unsigned long)stats.longest_chain);
    printf("Average chain : %.1f blocks\n", stats.avg_chain_length);

    veloxfs_unmount(&fs);
    close(fd);
    return 0;
}

/* =========================================================================
 * main
 * ======================================================================= */

int main(int argc, char *argv[]) {
    // Replace the existing stat logic with this version that can detect the size of physical drives
    if (argc == 3 && strcmp(argv[1], "--format") == 0) {
    int fd = open(argv[2], O_RDONLY);
    if (fd < 0) { perror(argv[2]); return 1; }

    uint64_t dev_size = 0;
    // Try to get block device size; if it fails, fall back to regular file stat
    if (ioctl(fd, BLKGETSIZE64, &dev_size) < 0) {
        struct stat st;
        if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return 1; }
        dev_size = st.st_size;
    }
    close(fd);

    uint64_t blocks = dev_size / veloxfs_BLOCK_SIZE;
    if (blocks < 64) {
        fprintf(stderr, "Device too small (detected %lu bytes)\n", (unsigned long)dev_size);
        return 1;
    }
    return format_disk(argv[2], blocks);
    }

    if (argc == 3 && strcmp(argv[1], "--stats") == 0)
        return show_stats(argv[2]);

    if (argc < 3) {
        fprintf(stderr,
            "veloxfs v5 FUSE Adapter\n"
            "=======================\n\n"
            "Usage:\n"
            "  Format : %s --format <disk.img>\n"
            "  Stats  : %s --stats  <disk.img>\n"
            "  Mount  : %s <disk.img> <mountpoint> [FUSE opts]\n\n"
            "Example:\n"
            "  dd if=/dev/zero of=veloxfs.img bs=1M count=512\n"
            "  %s --format veloxfs.img\n"
            "  mkdir /tmp/mnt\n"
            "  %s veloxfs.img /tmp/mnt -o big_writes,max_write=131072\n"
            "  fusermount -u /tmp/mnt\n",
            argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    g_fuse_state.disk_path = argv[1];
    g_fuse_state.disk_fd   = -1;
    pthread_mutex_init(&g_fuse_state.lock, NULL);

    g_fuse_state.disk_fd = open(g_fuse_state.disk_path, O_RDWR);
    if (g_fuse_state.disk_fd < 0) {
        perror("Failed to open disk image");
        return 1;
    }

    veloxfs_io io = { disk_read_cb, disk_write_cb, &g_fuse_state.disk_fd };

    printf("Mounting veloxfs v5 from %s ...\n", g_fuse_state.disk_path);
    int ret = veloxfs_mount(&g_fuse_state.fs, io);
    if (ret != veloxfs_OK) {
        fprintf(stderr, "Mount failed (%d). Try: %s --format %s\n",
                ret, argv[0], g_fuse_state.disk_path);
        close(g_fuse_state.disk_fd);
        return 1;
    }

    printf("  Version : veloxfs v5 (linked-list FAT)\n");
    printf("  Journal : %s\n",
           g_fuse_state.fs.super.journal_enabled ? "ENABLED" : "DISABLED");

    printf("Running fsck ...\n");
    veloxfs_fsck(&g_fuse_state.fs);

    veloxfs_alloc_stats stats;
    veloxfs_alloc_stats_get(&g_fuse_state.fs, &stats);
    printf("Usage: %lu / %lu blocks (%.1f%% used)\n",
           (unsigned long)stats.used_blocks,
           (unsigned long)stats.total_blocks,
           100.0 * stats.used_blocks / (double)stats.total_blocks);

    /* Build FUSE argv: program, mountpoint, [options...] */
    int fuse_argc = argc - 1;
    char **fuse_argv = malloc(sizeof(char *) * (size_t)(fuse_argc + 1));
    if (!fuse_argv) { perror("malloc"); return 1; }
    fuse_argv[0] = argv[0];          /* program name */
    for (int i = 2; i < argc; i++)
        fuse_argv[i - 1] = argv[i]; /* mountpoint + options */
    fuse_argv[fuse_argc] = NULL;

    printf("Starting FUSE (multithreaded + global lock)...\n");
    ret = fuse_main(fuse_argc, fuse_argv, &veloxfs_fuse_ops, NULL);

    free(fuse_argv);
    pthread_mutex_destroy(&g_fuse_state.lock);
    return ret;
}
