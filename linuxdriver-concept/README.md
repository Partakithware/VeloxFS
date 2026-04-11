This is a test I made for a linux driver. I have it working on a real drive of mine (8gb).
This is heavily flawed and just a concept to test out. (currently uses a modified veloxfs.h)

I had to sign the module hence signit.sh (You will need your own signing files)

```
make clean

make
```

Load Driver

```sudo insmod veloxfs_mod.ko```

Verify
```
sudo dmesg | tail -n 20
cat /proc/filesystems | grep veloxfs
```


Build the formatter

```gcc mkfs_velox.c -o mkfs_velox```

Format Drive

```sudo ./mkfs_velox /dev/YOURDRIVEHERE```

Mount It

```sudo mount -t veloxfs /dev/YOURDRIVEHERE /home/YOURUSERNAME/mountLOCATION```

Other stuff I used (you will need to change to match your drive/user/locations)
```
sudo rmmod veloxfs_mod
sudo blockdev --flushbufs /dev/sdc1
df -i /home/max/VMount
sudo chown max:max /home/max/VMount
```

Issues
===
Alot.
Extremly Long Mount/Umount/Format time.
Data loss if umount fails or is stopped of course.
etc...

Again just an idea so far.

