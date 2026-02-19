# veloxfs

A single-header C filesystem library, following the [STB](https://github.com/nothings/stb) single-file library convention.

The entire filesystem implementation lives in `veloxfs.h`. The optional `veloxfs_fuse.c` adapter lets you mount a veloxfs image as a real filesystem on Linux via FUSE. It is not required to use the library.
Other examples can be found in [examples](./examples) here for different use case examples, for now there is only one. 

Ensure you read the issues tab.

---

## How it works

veloxfs stores data in a flat binary image (a file, a block device, a memory buffer, or anything you can read and write by offset). The layout is:

```
[ superblock | FAT | journal | inodes | directory | data blocks ]
```

Block allocation uses a FAT-style singly-linked list. Each inode stores the index of its first block; the FAT maps each block to the next, terminating with a sentinel value. This means files can grow without relocation and fragmentation does not cause data corruption.

Directories are implicit. There is no directory inode — a directory exists if at least one file path begins with that prefix. The FUSE adapter anchors empty directories with a hidden `.veloxfs_dir` marker file so they survive remount.

---

## What it supports

- Create, read, write, delete, rename files
- Create, delete, rename directories (arbitrarily nested)
- File data persists across mount/unmount
- Unix-style permissions (uid, gid, mode bits)
- Optional write-ahead journal (64-entry circular log)
- O(1) directory lookups via an in-memory FNV-1a hash table
- `fsck` to detect and free orphaned blocks
- Allocation statistics

## What it does not support

- Hard links or symbolic links
- Extended attributes
- File locking
- Access control lists
- Sparse files
- Timestamps with sub-second precision
- Concurrent access without external locking (the FUSE adapter adds a mutex; the library itself is single-threaded)

---

## Usage

### Library only (no FUSE)

In **one** C file, define the implementation before including the header:

```c
#define veloxfs_IMPLEMENTATION
#include "veloxfs.h"
```

All other files that need the API include it without the define:

```c
#include "veloxfs.h"
```

### Minimal example

```c
#define veloxfs_IMPLEMENTATION
#include "veloxfs.h"
#include <stdio.h>

// Provide two callbacks: read and write at a byte offset.
// The backing store can be anything.
static uint8_t storage[32 * 1024 * 1024]; // 32 MB RAM disk

static int mem_read(void *user, uint64_t off, void *buf, uint32_t n) {
    memcpy(buf, (uint8_t *)user + off, n);
    return 0;
}
static int mem_write(void *user, uint64_t off, const void *buf, uint32_t n) {
    memcpy((uint8_t *)user + off, buf, n);
    return 0;
}

int main(void) {
    veloxfs_io io = { mem_read, mem_write, storage };

    uint64_t blocks = sizeof(storage) / veloxfs_BLOCK_SIZE;
    veloxfs_format(io, blocks, 0 /* journaling off */);

    veloxfs_handle fs;
    veloxfs_mount(&fs, io);

    veloxfs_create(&fs, "/hello.txt", 0644);
    veloxfs_write_file(&fs, "/hello.txt", "hello", 5);

    char buf[16];
    uint64_t got;
    veloxfs_read_file(&fs, "/hello.txt", buf, sizeof(buf), &got);
    buf[got] = '\0';
    printf("%s\n", buf); // hello

    veloxfs_unmount(&fs);
    return 0;
}
```

### Mounting as a real filesystem (Linux, requires libfuse)

```sh
# Build
gcc -Wall -O2 -pthread veloxfs_fuse.c -o veloxfs_fuse \
    `pkg-config fuse --cflags --libs`

# Create and format an image
dd if=/dev/zero of=veloxfs.img bs=1M count=512
./veloxfs_fuse --format veloxfs.img

# Mount
mkdir /tmp/mnt
./veloxfs_fuse veloxfs.img /tmp/mnt -o big_writes,max_write=131072

# Use it normally — cp, mv, mkdir, rm, etc.
cp largefile.bin /tmp/mnt/
mkdir /tmp/mnt/docs
mv /tmp/mnt/docs /tmp/mnt/documents

# Unmount — data persists
fusermount -u /tmp/mnt

# Remount and verify
./veloxfs_fuse veloxfs.img /tmp/mnt
ls /tmp/mnt
```

---

## API reference

### Filesystem lifecycle

```c
int veloxfs_format(veloxfs_io io, uint64_t block_count, int enable_journal);
int veloxfs_mount(veloxfs_handle *fs, veloxfs_io io);
int veloxfs_unmount(veloxfs_handle *fs);
int veloxfs_sync(veloxfs_handle *fs);
int veloxfs_fsck(veloxfs_handle *fs);
```

### File operations

```c
int veloxfs_create(veloxfs_handle *fs, const char *path, uint32_t mode);
int veloxfs_delete(veloxfs_handle *fs, const char *path);
int veloxfs_rename(veloxfs_handle *fs, const char *old_path, const char *new_path);
int veloxfs_write_file(veloxfs_handle *fs, const char *path, const void *data, uint64_t size);
int veloxfs_read_file(veloxfs_handle *fs, const char *path, void *out, uint64_t max, uint64_t *out_size);
```

### File handle operations (streaming I/O)

```c
int veloxfs_open(veloxfs_handle *fs, const char *path, int flags, veloxfs_file *file);
int veloxfs_close(veloxfs_file *file);
int veloxfs_read(veloxfs_file *file, void *buf, uint64_t count, uint64_t *bytes_read);
int veloxfs_write(veloxfs_file *file, const void *buf, uint64_t count);
int veloxfs_seek(veloxfs_file *file, int64_t offset, int whence);
uint64_t veloxfs_tell(veloxfs_file *file);
int veloxfs_truncate_handle(veloxfs_file *file, uint64_t new_size);
```

### Metadata and statistics

```c
int veloxfs_stat(veloxfs_handle *fs, const char *path, veloxfs_stat_t *stat);
int veloxfs_statfs(veloxfs_handle *fs, uint64_t *total, uint64_t *used, uint64_t *free);
int veloxfs_chmod(veloxfs_handle *fs, const char *path, uint32_t mode);
int veloxfs_chown(veloxfs_handle *fs, const char *path, uint32_t uid, uint32_t gid);
int veloxfs_mkdir(veloxfs_handle *fs, const char *path, uint32_t mode);
int veloxfs_list(veloxfs_handle *fs, const char *path, veloxfs_list_callback cb, void *user);
```

### Error codes

| Code | Value | Meaning |
|------|-------|---------|
| `veloxfs_OK` | 0 | Success |
| `veloxfs_ERR_IO` | -1 | I/O callback returned an error |
| `veloxfs_ERR_CORRUPT` | -2 | On-disk structure is inconsistent |
| `veloxfs_ERR_NOT_FOUND` | -3 | Path does not exist |
| `veloxfs_ERR_EXISTS` | -4 | Path already exists |
| `veloxfs_ERR_NO_SPACE` | -5 | No free blocks |
| `veloxfs_ERR_INVALID` | -6 | Invalid argument |
| `veloxfs_ERR_TOO_LARGE` | -7 | Operation exceeds limits |
| `veloxfs_ERR_TOO_MANY_FILES` | -8 | Inode or directory table full |
| `veloxfs_ERR_PERMISSION` | -9 | Permission denied |

---

## On-disk layout

| Region | Size |
|--------|------|
| Superblock | 1 block |
| FAT | `ceil(block_count / 512)` blocks |
| Journal | 64 blocks (optional) |
| Inode table | `block_count / 50` blocks |
| Directory table | `block_count / 100` blocks |
| Data | remainder |

Block size is fixed at 4096 bytes. The FAT stores one `uint64_t` per block — `0` means free, `0xFFFFFFFFFFFFFFFF` means end of chain, any other value is the index of the next block in the file.

---

## Portability

The library requires C99 and the following standard headers: `stdint.h`, `stddef.h`, `string.h`, `stdlib.h`, `stdio.h`.

`time.h` is included automatically for the default timestamp implementation, but it is not required if you override the timestamp source (see below).

The FUSE adapter requires Linux and `libfuse` 2.x (`FUSE_USE_VERSION 26`).

There are no other dependencies.

### Systems without a real-time clock

All timestamp calls go through a single overridable macro. If your platform has no RTC or no `time()` function (bare-metal, RTOS, WASM, etc.), define `veloxfs_TIME()` before including the header:

```c
// No clock — store 0 for all timestamps
#define veloxfs_TIME() 0

// Custom clock source
#define veloxfs_TIME() my_rtc_get_unix_seconds()
```

If `veloxfs_TIME` is not defined, it defaults to `time(NULL)` and `time.h` is included automatically.

---

3. Multi-platform Support (Needs One Minor Fix ⚠️) : Should be fixed now!

## License

Public domain or MIT, your choice.
