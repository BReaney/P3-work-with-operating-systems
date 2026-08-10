# Final score: 15.0/15.0

## Basic Functionality (1.0/1.0)

- [0.5/0.5] README present — README.md, README.txt
- [0.5/0.5] `make` builds all four executables

## `diskinfo` (2.0/2.0)

- [1/1] `diskinfo` output format
- [0.5/0.5] `diskinfo` correct on `test.img`
- [0.5/0.5] `diskinfo` correct on `large.img`

## `disklist` (4.0/4.0)

- [1/1] `disklist` output format
- [1/1] `disklist non-empty.img /`
- [1/1] `disklist non-empty.img /sub_Dir`
- [1/1] `disklist large.img /dir1/dir2/dir3/dir4/dir5`

## `diskget` (4.0/4.0)

- [0.5/0.5] `diskget` reports a missing file
- [1/1] `diskget` copies `/sub_Dir/201.jpg`
- [1/1] `diskget` copies `/dir1/dir2/dir3/dir4/dir5/video.mp4`
- [1.5/1.5] copied files are byte-identical — 2/2 files matched the expected SHA-1

## `diskput` (4.0/4.0)

- [0.5/0.5] `diskput` reports a missing source file
- [2/2] `diskput` copies a file into the specified directory — 2/2 cases passed (root + text, existing sub-directory + binary)
- [1/1] copied-back file is identical to the original — 2/2 files read back byte-identical
- [0.5/0.5] `diskput` creates a missing sub-directory

Everything passed — nice work.
