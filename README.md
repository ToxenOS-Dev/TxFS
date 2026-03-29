# TxFS — ToxenOS Native Filesystem

TxFS is a simple, custom filesystem designed for [ToxenOS](https://github.com/ToxenOS-Dev/ToxenOS). It is not compatible with any other operating system by design — TxFS is ToxenOS's own filesystem, built from scratch.

## Features

- FAT-free, Linux-free, Windows-free — completely original
- Fixed 4096-byte block size
- 12 direct blocks + 1 indirect block per file (~4 MB max file size)
- Multi-block directories (no entry count cap)
- Bitmap-based inode and block allocation
- Simple 8.3-style filenames (up to 255 chars in dirent)
- Host-side tool (`txfs_write`) to create and populate disk images

## Disk Layout

```
Block 0  — reserved (boot sector)
Block 1  — superblock
Block 2  — inode bitmap
Block 3  — block bitmap
Block 4  — inode table (32 blocks, 16 inodes per block = 512 inodes max)
Block 36 — data blocks start here
```

## Superblock

The superblock lives at block 1 and contains:

| Field         | Size    | Description                  |
|---------------|---------|------------------------------|
| magic         | uint32  | 0x54584653 ("TXFS")          |
| version       | uint32  | filesystem version           |
| block_size    | uint32  | always 4096                  |
| total_blocks  | uint32  | total data blocks            |
| free_blocks   | uint32  | free data blocks             |
| total_inodes  | uint32  | max inodes                   |
| free_inodes   | uint32  | free inodes                  |
| root_inode    | uint32  | always 0                     |

## Inode

Each inode is 256 bytes:

| Field         | Size    | Description                        |
|---------------|---------|------------------------------------|
| mode          | uint32  | type (top 4 bits) + permissions    |
| uid           | uint32  | owner                              |
| size          | uint32  | file size in bytes                 |
| created       | uint32  | creation timestamp                 |
| modified      | uint32  | modification timestamp             |
| links         | uint32  | hard link count                    |
| blocks[12]    | uint32  | direct block pointers              |
| indirect      | uint32  | single indirect block pointer      |
| dindirect     | uint32  | double indirect (reserved)         |

File types (top 4 bits of mode):
- `0x1` — regular file
- `0x2` — directory
- `0x3` — symlink

## Directory Entry

Each directory entry is 263 bytes:

| Field    | Size   | Description          |
|----------|--------|----------------------|
| inode    | uint32 | inode number (0 = deleted) |
| name_len | uint16 | length of name       |
| type     | uint8  | file type            |
| name     | char[256] | filename          |

## Building

TxFS is part of ToxenOS. To build the kernel-side driver, it is compiled as part of the ToxenOS kernel build. See the [ToxenOS repo](https://github.com/ToxenOS-Dev/ToxenOS) for the full build system.

## Host Tool — txfs_write

`txfs_write` lets you create and populate TxFS disk images from your host machine (Linux/macOS).

### Build

```bash
gcc -O2 -o txfs_write txfs_write.c
```

### Usage

```bash
# Write files into a disk image
./txfs_write disk.img localfile.txt /path/in/txfs.txt

# Write multiple files at once
./txfs_write disk.img file1 /dir/file1 file2 /dir/file2

# Creates the disk image and formats it automatically if it doesn't exist
```

### Example

```bash
# Create a 100MB TxFS image and populate it
dd if=/dev/zero of=disk.img bs=4096 count=25600
./txfs_write disk.img myprog.elf /bin/myprog.elf
./txfs_write disk.img readme.txt /readme.txt
```

The tool automatically creates intermediate directories as needed, so writing to `/bin/myprog.elf` will create the `/bin` directory if it doesn't exist.

## Magic Number

TxFS uses the magic number `0x54584653` which is the ASCII encoding of `TXFS`.

## License

TxFS is part of ToxenOS, developed by ToxenOS-Dev.
