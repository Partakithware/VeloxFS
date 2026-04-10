This is a test I made for a linux driver. I have it working on a real drive of mine (8gb).
This is heavily flawed and just a concept to test out.

I had to sign the module hence signit.sh (You will need your own signing files)

make clean

make

Build the formatter

gcc mkfs_velox.c -o mkfs_velox

Format Drive

sudo ./mkfs_velox /dev/YOURDRIVEHERE

Mount It

sudo mount -t veloxfs /dev/YOURDRIVEHERE /home/YOURUSERNAME/mountLOCATION

Issues
===
Alot.
Long Mount/Umount time
Data loss if umount fails or is stopped of course
etc...

Again just an idea so far.

