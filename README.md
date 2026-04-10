# veloxfs

> [!IMPORTANT]
> The `latest` branch contains **v6** — a full rewrite of the allocation engine tuned for **HDD, Flash, and NAND** storage. v6 images are **not compatible** with v5 images (different magic number `VLX6`). Re-format any existing images. The README below reflects v6.

A single-header C filesystem library, following the [STB](https://github.com/nothings/stb) single-file library convention.

The entire filesystem implementation lives in `veloxfs.h`. The optional `veloxfs_fuse.c` adapter lets you mount a veloxfs image as a real filesystem on Linux via FUSE. It is not required to use the library.

Other examples can be found in [examples](./examples). Ensure you read the issues tab.

---

## Why v6?

v5 was designed around NVMe assumptions that made it pathologically slow on rotating and flash storage:

| Problem (v5) | Impact |
|---|---|
| Fixed 4 KB block size | One seek per block on HDD → ~5 ms × 256 K blocks = **21 minutes per GB** |
| Zero-on-alloc | Every allocated block got an extra write before data landed (2× write amplification) |
| One `pread`/`pwrite` per block | Even sequential files issued 262 144 syscalls per GB |
| Random next-fit allocation | Blocks scattered across disk, destroying HDD seek patterns and NAND write efficiency |

v6 fixes all of this:

| Change (v6) | Benefit |
|---|---|
| Configurable block size at format time | 64 KB (HDD/Flash) or 128 KB (NAND) blocks → 64× fewer blocks per GB |
| Contiguous-first allocator | Searches for the best free run; sequential writes become sequential on disk |
| 4-extent inline inode map | A file in 1 extent = **one** `pread`/`pwrite` call, regardless of size |
| Zero-on-alloc removed | One write per block (data only) instead of two |
| Heap-allocated scratch buffer | Safe at 64 KB+ block sizes; no more stack-allocated `uint8_t buf[BLOCK_SIZE]` |
| 1 MB FUSE `max_write`/`max_read` | Kernel coalesces small writes into large aligned I/Os |

**Typical sequential throughput improvement (1 GB file):**

| Storage | v5-estimate | v6-estimate |
|---|---|---|
| HDD | ~0.1 MB/s | ~100 MB/s |
| NAND | ~1 MB/s | ~200 MB/s |
| Flash | ~1 MB/s | ~150 MB/s |

---

## How it works

veloxfs stores data in a flat binary image (a file, a block device, a memory buffer, or anything readable/writable by offset). The on-disk layout is:

```
[ superblock | FAT | journal | inodes | directory | data blocks ]
```

### Block allocation (v6: extent-first)

Each inode stores up to **4 inline extents** — each extent is a `(start_block, block_count)` pair describing a contiguous run of blocks. The allocator always tries to find the best contiguous free run first, so most files land in a single extent. Reading or writing a single-extent file issues exactly one I/O call regardless of file size.

If a file becomes fragmented beyond 4 extents (unusual with the contiguous allocator), it spills into a traditional FAT overflow chain, preserving correctness at the cost of the per-block overhead v5 always paid.

### FAT

The FAT is still present and serves two roles: tracking which blocks are free/allocated (every allocated block is marked `FAT_EOF`), and storing the overflow linked-list chain for heavily fragmented files. Within a contiguous extent, navigation is done through the inode's extent map — the FAT is not walked block by block.

### Directories

Directories are implicit. There is no directory inode — a directory exists if at least one file path begins with that prefix. O(1) lookups are provided by an in-memory FNV-1a hash table built at mount time.

---

## What it supports

- Create, read, write, delete, rename files
- Create, delete, rename directories (arbitrarily nested)
- File data persists across mount/unmount
- Unix-style permissions (uid, gid, mode bits)
- Optional write-ahead journal (64-entry circular log)
- O(1) directory lookups via an in-memory FNV-1a hash table
- `fsck` to detect and free orphaned blocks
- Allocation and extent statistics
- Four storage presets selectable at format time: `--format-hdd`, `--format-nand`, `--format-flash`, `--format-nvme`

## What it does not support

- Hard links or symbolic links
- Extended attributes
- File locking
- Access control lists
- Sparse files
- Timestamps with sub-second precision
- Concurrent access without external locking (the FUSE adapter serialises all calls with a `pthread_mutex_t`; the library itself is single-threaded)

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

### Choosing a block size

Use `veloxfs_format_ex()` to select the right block size for your storage. The convenience constants are:

| Constant | Size | Best for |
|---|---|---|
| `veloxfs_BS_HDD` | 64 KB | Spinning hard drives |
| `veloxfs_BS_FLASH` | 64 KB | NOR / managed flash |
| `veloxfs_BS_NAND` | 128 KB | Raw NAND (aligns to erase boundaries) |
| `veloxfs_BS_NVME` | 4 KB | NVMe / SSD (fine-grained latency) |

`veloxfs_format()` (no `_ex`) defaults to `veloxfs_BS_HDD`.

### Minimal example

```c
#define veloxfs_IMPLEMENTATION
#include "veloxfs.h"
#include <stdio.h>

static uint8_t storage[64 * 1024 * 1024]; // 64 MB RAM disk

static int mem_read(void *user, uint64_t off, void *buf, uint32_t n) {
    memcpy(buf, (uint8_t *)user + off, n); return 0;
}
static int mem_write(void *user, uint64_t off, const void *buf, uint32_t n) {
    memcpy((uint8_t *)user + off, buf, n); return 0;
}

int main(void) {
    veloxfs_io io = { mem_read, mem_write, storage };

    // For RAM disks, BS_NVME (4 KB) keeps block count reasonable
    uint64_t blocks = sizeof(storage) / veloxfs_BS_NVME;
    veloxfs_format_ex(io, blocks, 0, veloxfs_BS_NVME, veloxfs_STORAGE_AUTO);

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

# Create an image and format it for your storage type
dd if=/dev/zero of=veloxfs.img bs=1M count=512

./veloxfs_fuse --format-hdd   veloxfs.img   # spinning drive / image file
./veloxfs_fuse --format-nand  veloxfs.img   # raw NAND
./veloxfs_fuse --format-flash veloxfs.img   # NOR / managed flash
./veloxfs_fuse --format-nvme  veloxfs.img   # NVMe / SSD

# Mount — always pass big_writes and the 1 MB limits for best throughput
mkdir /tmp/mnt
./veloxfs_fuse veloxfs.img /tmp/mnt \
    -o big_writes,max_write=1048576,max_read=1048576

# Use it normally
cp largefile.bin /tmp/mnt/
mkdir /tmp/mnt/docs
mv /tmp/mnt/docs /tmp/mnt/documents

# Unmount — data persists
fusermount -u /tmp/mnt

# Remount and verify
./veloxfs_fuse veloxfs.img /tmp/mnt \
    -o big_writes,max_write=1048576,max_read=1048576
ls /tmp/mnt
```

#### Mounting a real block device (HDD, NAND, etc.)

```sh
# Format a physical drive (DANGEROUS — destroys all data on /dev/sdb)
./veloxfs_fuse --format-hdd /dev/sdb

# Mount
mkdir /mnt/velox
./veloxfs_fuse /dev/sdb /mnt/velox \
    -o big_writes,max_write=1048576,max_read=1048576

# NAND block device
./veloxfs_fuse --format-nand /dev/mtdblock0
./veloxfs_fuse /dev/mtdblock0 /mnt/data \
    -o big_writes,max_write=1048576,max_read=1048576,direct_io
```

#### Debug / testing

```sh
./veloxfs_fuse veloxfs.img /tmp/mnt \
    -d -o big_writes,max_write=1048576,max_read=1048576
```

---

## API reference

### Filesystem lifecycle

```c
// Format with default block size (64 KB, HDD-optimised)
int veloxfs_format(veloxfs_io io, uint64_t block_count, int enable_journal);

// Format with explicit block size and storage hint
int veloxfs_format_ex(veloxfs_io io, uint64_t block_count, int enable_journal,
                      uint32_t block_size, uint32_t storage_hint);

int veloxfs_mount  (veloxfs_handle *fs, veloxfs_io io);
int veloxfs_unmount(veloxfs_handle *fs);
int veloxfs_sync   (veloxfs_handle *fs);
int veloxfs_fsck   (veloxfs_handle *fs);
```

### File operations

```c
int veloxfs_create    (veloxfs_handle *fs, const char *path, uint32_t mode);
int veloxfs_delete    (veloxfs_handle *fs, const char *path);
int veloxfs_rename    (veloxfs_handle *fs, const char *old_path, const char *new_path);
int veloxfs_write_file(veloxfs_handle *fs, const char *path, const void *data, uint64_t size);
int veloxfs_read_file (veloxfs_handle *fs, const char *path, void *out,
                       uint64_t max, uint64_t *out_size);
```

### File handle operations (streaming I/O)

```c
int      veloxfs_open           (veloxfs_handle *fs, const char *path, int flags, veloxfs_file *file);
int      veloxfs_close          (veloxfs_file *file);
int      veloxfs_read           (veloxfs_file *file, void *buf, uint64_t count, uint64_t *bytes_read);
int      veloxfs_write          (veloxfs_file *file, const void *buf, uint64_t count);
int      veloxfs_seek           (veloxfs_file *file, int64_t offset, int whence);
uint64_t veloxfs_tell           (veloxfs_file *file);
int      veloxfs_truncate_handle(veloxfs_file *file, uint64_t new_size);
```

### Metadata and statistics

```c
int veloxfs_stat            (veloxfs_handle *fs, const char *path, veloxfs_stat_t *stat);
int veloxfs_statfs          (veloxfs_handle *fs, uint64_t *total, uint64_t *used, uint64_t *free);
int veloxfs_chmod           (veloxfs_handle *fs, const char *path, uint32_t mode);
int veloxfs_chown           (veloxfs_handle *fs, const char *path, uint32_t uid, uint32_t gid);
int veloxfs_mkdir           (veloxfs_handle *fs, const char *path, uint32_t mode);
int veloxfs_list            (veloxfs_handle *fs, const char *path,
                             veloxfs_list_callback cb, void *user);
int veloxfs_alloc_stats_get (veloxfs_handle *fs, veloxfs_alloc_stats *stats);
```

### Storage hints (for `veloxfs_format_ex`)

```c
veloxfs_STORAGE_AUTO    // let the library decide (default)
veloxfs_STORAGE_HDD     // 64 KB blocks, contiguous-first allocation
veloxfs_STORAGE_FLASH   // 64 KB blocks, page-aligned
veloxfs_STORAGE_NAND    // 128 KB blocks, erase-boundary aligned
```

### Error codes

| Code | Value | Meaning |
|---|---|---|
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
|---|---|
| Superblock | 1 block |
| FAT | `ceil(block_count × 8 / block_size)` blocks |
| Journal | 64 blocks (optional) |
| Inode table | `block_count / 50` blocks |
| Directory table | `block_count / 100` blocks |
| Data | remainder |

Block size is set at format time (4 KB – 1 MB, must be a power of two). Each FAT entry is a `uint64_t`: `0` = free, `0xFFFFFFFFFFFFFFFF` = allocated/end-of-chain, `0xFFFFFFFFFFFFFFFE` = bad block.

Each inode stores up to 4 inline extents (`start_block`, `block_count`). Files that exceed 4 extents spill into a traditional FAT linked-list chain anchored at `inode.fat_head`.

The on-disk magic for v6 images is `VLX6` (`0x564C5836`). v5 images (`VLXF`) cannot be mounted by v6.

---

## Portability

The library requires C99 and the following standard headers: `stdint.h`, `stddef.h`, `string.h`, `stdlib.h`, `stdio.h`.

`time.h` is included automatically for the default timestamp implementation but is not required if you override the timestamp source.

The FUSE adapter requires Linux and `libfuse` 2.x (`FUSE_USE_VERSION 26`).

There are no other dependencies.

### Systems without a real-time clock

All timestamp calls go through a single overridable macro. Define `veloxfs_TIME()` before including the header:

```c
// No clock — store 0 for all timestamps
#define veloxfs_TIME() 0

// Custom clock source
#define veloxfs_TIME() my_rtc_get_unix_seconds()
```

If `veloxfs_TIME` is not defined, it defaults to `time(NULL)`.

---

## License

Public domain or MIT, your choice.
