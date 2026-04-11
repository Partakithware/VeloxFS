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

You will need your own kernel signing keys to load the module.

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

