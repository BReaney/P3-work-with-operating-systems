Files(As they should work, this code is kinda buggy):
diskinfo reads superblock and provides data
disklist lists the directory info
diskget copies a regular file from the image into the file system
diskput copies a host file into the image and creates missing target directories

To run:
make

this compiles the four files, and then they can be run as specified in the assignment doc.