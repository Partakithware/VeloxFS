VeloxFS v6 (Linux Kernel Driver Concept)

Author: Maxwell Wingate

Status: Functional Concept / Prototype

Verified On: Linux 7.0.0-rc6 (Ubuntu) | 8GB Physical External Drive

VeloxFS v6 is a storage-optimized filesystem driver built from scratch. This project demonstrates a working bridge between a custom on-disk format and the Linux Virtual File System (VFS).
Milestone Achieved

    Persistence: Survived umount with 10,014 files (10k empty files + 14 high-bitrate videos ~130MB each).

    Integrity: Video files played back perfectly after remounting, confirming FAT chain stability.

Getting Started
===
1. Build & Sign

You will need your own kernel signing keys to load the module. (Assuming Secure Boot)

I had to sign the module hence signit.sh (You will need your own signing files you can use signit.sh if you modify locations)

```
make clean
make
```

# Run your signing script (e.g., ./signit.sh)

2. Load the Driver

```sudo insmod veloxfs_mod.ko```

# Verify load
```
cat /proc/filesystems | grep veloxfs
sudo dmesg | tail -n 20
```
3. Format & Mount

# Build the formatter
```gcc mkfs_velox.c -o mkfs_velox```

# Format (CAUTION: Ensure path is correct)
```sudo ./mkfs_velox /dev/sdcX```

# Mount
```
sudo mount -t veloxfs /dev/sdcX /home/user/mountpoint
sudo chown $USER:$USER /home/user/mountpoint
```

Known Issues & Limitations

    Performance: Extremely long format, mount, and unmount times. The driver currently lacks Page Cache integration, meaning every operation is a direct, synchronous call to the hardware.

    Stability: Potential data loss if umount is interrupted or fails.

    Architecture: Currently uses a modified veloxfs.h authoritative header for both kernel and userspace.

On a USB 2.0 PNY drive, which usually caps out at 10-15 MB/s for sustained writes, you are seeing the "Synchronous Tax" of the driver. Because I haven't implemented the Linux Page Cache yet, the CPU is manually walking every single block and waiting for the physical hardware to confirm the write before moving to the next one.

### Performance Benchmarks (v6 Prototype)

| Metric | Value | Meaning |
| :--- | :--- | :--- |
| **Real Time** | 57.63s | The total human-wait time for the data to hit the platter. |
| **Kernel (Sys) Time** | 0.429s | The actual time your code spent running in the CPU. |
| **Hardware Latency** | ~57.2s | The time spent waiting for the PNY drive to finish "spinning." |
| **Effective Speed** | ~4.3 MB/s | Solid for a v6 driver with no caching on a legacy 512-byte sector device. |

The sys time of 0.429s is the most interesting number here. It shows that the driver's actual logic — calculating FAT offsets and updating inodes — is incredibly fast. The "missing" 57 seconds is almost entirely the hardware waiting for the slow USB 2.0 bus to finish the physical write.

In a standard filesystem like ext4, the cp command would have finished in about 2 seconds, and the sync command would have taken the remaining 55 seconds. The driver does everything at once because it's synchronous:

    The Inode Update: The driver writes to the inode table to say "Episode 15 is coming."

    The FAT Chain: It finds a free block, writes to the FAT, and waits for the disk.

    The Data Write: It writes the 4096-byte chunk of video and waits.

    Repeat: It repeats this thousands of times.

Modern OS performance comes from "lying" — the kernel tells the app the write is done while the data is still sitting in RAM. It's slower, but it's much easier to debug because you know exactly where the data is at any given millisecond.

    [!NOTE]
    That 512-byte physical block size is actually working against it. The driver is optimized for xxxx-byte chunks, meaning the hardware has to do "internal" work to split your driver's requests into those smaller 512-byte physical sectors.

Contributing

This is an experimental idea. If you’re interested in kernel development or filesystem architecture, feel free to fork, submit PRs, or help optimize the I/O path!


Other stuff I used (you will need to change to match your drive/user/locations)
```
sudo rmmod veloxfs_mod
sudo blockdev --flushbufs /dev/sdc1
df -i /home/max/VMount
sudo chown max:max /home/max/VMount
```


Again just an idea so far.

