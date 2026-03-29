// txfs_write.c — Host-side tool to write files into a TXFS disk image.
// Usage: txfs_write <disk.img> <local_file> <txfs_path> [<local_file> <txfs_path>...]
//
// Supports:
//   - Arbitrary nesting: /a/b/c/d (parent dirs created on demand)
//   - Single indirect block: files up to ~4 MB
//   - Correct block/inode counter maintenance

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define TXFS_MAGIC          0x54584653
#define TXFS_BLOCK_SIZE     4096
#define TXFS_MAX_INODES     128
#define TXFS_DIRECT_BLOCKS  12
#define TXFS_PTRS_PER_BLOCK (TXFS_BLOCK_SIZE / 4)
#define TXFS_BLOCK_SUPER    1
#define TXFS_BLOCK_IBITMAP  2
#define TXFS_BLOCK_BBITMAP  3
#define TXFS_BLOCK_INODES   4
#define TXFS_BLOCK_DATA     36
#define TXFS_TYPE_FILE      0x1
#define TXFS_TYPE_DIR       0x2

typedef struct {
    uint32_t magic, version, block_size, total_blocks, free_blocks;
    uint32_t total_inodes, free_inodes, root_inode;
    uint8_t  pad[4096 - 32];
} __attribute__((packed)) sb_t;

typedef struct {
    uint32_t mode, uid, size, created, modified, links;
    uint32_t blocks[TXFS_DIRECT_BLOCKS];
    uint32_t indirect, dindirect;
    uint8_t  pad[256 - (6 + TXFS_DIRECT_BLOCKS + 2) * 4];
} __attribute__((packed)) inode_t;

typedef struct {
    uint32_t inode;
    uint16_t name_len;
    uint8_t  type;
    char     name[256];
} __attribute__((packed)) dirent_t;

static FILE*   disk;
static sb_t    sb;
static uint8_t imap[TXFS_BLOCK_SIZE];
static uint8_t bmap[TXFS_BLOCK_SIZE];

// --- raw block I/O -----------------------------------------------------------

static void rd(uint32_t b, void* buf)
{
    fseek(disk, (long)b * TXFS_BLOCK_SIZE, SEEK_SET);
    fread(buf, TXFS_BLOCK_SIZE, 1, disk);
}

static void wr(uint32_t b, const void* buf)
{
    fseek(disk, (long)b * TXFS_BLOCK_SIZE, SEEK_SET);
    fwrite(buf, TXFS_BLOCK_SIZE, 1, disk);
    fflush(disk);
}

static void rdsb() { rd(TXFS_BLOCK_SUPER, &sb); }
static void wrsb() { wr(TXFS_BLOCK_SUPER, &sb); }

// --- bitmap helpers ----------------------------------------------------------

static int btest(uint8_t* m, int i) { return (m[i / 8] >> (i % 8)) & 1; }
static void bset(uint8_t* m, int i) { m[i / 8] |= (1 << (i % 8)); }
static void bclr(uint8_t* m, int i) { m[i / 8] &= ~(1 << (i % 8)); }

static int balloc(uint8_t* m, int max)
{
    for (int i = 0; i < max; i++)
        if (!btest(m, i)) { bset(m, i); return i; }
    return -1;
}

// --- inode I/O ---------------------------------------------------------------

static void rdinode(uint32_t n, inode_t* v)
{
    uint8_t buf[TXFS_BLOCK_SIZE];
    rd(TXFS_BLOCK_INODES + (n / 16), buf);
    memcpy(v, buf + (n % 16) * sizeof(inode_t), sizeof(inode_t));
}

static void wrinode(uint32_t n, const inode_t* v)
{
    uint8_t buf[TXFS_BLOCK_SIZE];
    rd(TXFS_BLOCK_INODES + (n / 16), buf);
    memcpy(buf + (n % 16) * sizeof(inode_t), v, sizeof(inode_t));
    wr(TXFS_BLOCK_INODES + (n / 16), buf);
}

// --- block allocation --------------------------------------------------------

static uint8_t zero_block[TXFS_BLOCK_SIZE];

static int ablock()
{
    rd(TXFS_BLOCK_BBITMAP, bmap);
    if (sb.total_blocks == 0 || sb.total_blocks > 100000) {
        fprintf(stderr, "bad total_blocks: %u\n", sb.total_blocks);
        return -1;
    }
    int b = balloc(bmap, (int)sb.total_blocks);
    if (b < 0) { fprintf(stderr, "out of blocks\n"); return -1; }
    wr(TXFS_BLOCK_BBITMAP, bmap);
    sb.free_blocks--;
    wrsb();
    // clear new block
    wr((uint32_t)(b + TXFS_BLOCK_DATA), zero_block);
    return b + TXFS_BLOCK_DATA;
}

static void freeblock(uint32_t blk)
{
    if (blk < TXFS_BLOCK_DATA) return;
    rd(TXFS_BLOCK_BBITMAP, bmap);
    bclr(bmap, (int)(blk - TXFS_BLOCK_DATA));
    wr(TXFS_BLOCK_BBITMAP, bmap);
    sb.free_blocks++;
    wrsb();
}

static int ainode()
{
    rd(TXFS_BLOCK_IBITMAP, imap);
    int i = balloc(imap, (int)sb.total_inodes);
    if (i < 0) return -1;
    wr(TXFS_BLOCK_IBITMAP, imap);
    sb.free_inodes--;
    wrsb();
    return i;
}

static void freeinode(uint32_t inum)
{
    rd(TXFS_BLOCK_IBITMAP, imap);
    bclr(imap, (int)inum);
    wr(TXFS_BLOCK_IBITMAP, imap);
    sb.free_inodes++;
    wrsb();
}

// --- indirect block block-getter ---------------------------------------------

// Returns physical block for logical index idx (allocates if alloc=1).
static uint32_t get_block(inode_t* ino, uint32_t idx, int alloc)
{
    if (idx < TXFS_DIRECT_BLOCKS) {
        if (!ino->blocks[idx] && alloc) {
            int b = ablock();
            if (b < 0) return 0;
            ino->blocks[idx] = (uint32_t)b;
        }
        return ino->blocks[idx];
    }

    uint32_t iidx = idx - TXFS_DIRECT_BLOCKS;
    if (iidx >= TXFS_PTRS_PER_BLOCK) return 0;

    if (!ino->indirect) {
        if (!alloc) return 0;
        int b = ablock();
        if (b < 0) return 0;
        ino->indirect = (uint32_t)b;
    }

    uint32_t ptrs[TXFS_PTRS_PER_BLOCK];
    rd(ino->indirect, ptrs);

    if (!ptrs[iidx]) {
        if (!alloc) return 0;
        int b = ablock();
        if (b < 0) return 0;
        ptrs[iidx] = (uint32_t)b;
        wr(ino->indirect, ptrs);
    }

    return ptrs[iidx];
}

// --- directory helpers -------------------------------------------------------

// Find a named entry in dir_ino. Returns child inode, or -1.
static int dir_lookup(int dir_ino, const char* name)
{
    inode_t dir;
    rdinode((uint32_t)dir_ino, &dir);

    uint32_t per_block = TXFS_BLOCK_SIZE / sizeof(dirent_t);
    uint32_t total     = dir.size / sizeof(dirent_t);
    uint32_t checked   = 0;

    for (uint32_t b = 0; checked < total; b++) {
        uint32_t blk = get_block(&dir, b, 0);
        if (!blk) break;

        uint8_t buf[TXFS_BLOCK_SIZE];
        rd(blk, buf);

        uint32_t in_this = total - checked;
        if (in_this > per_block) in_this = per_block;

        for (uint32_t i = 0; i < in_this; i++) {
            dirent_t* de = (dirent_t*)(buf + i * sizeof(dirent_t));
            if (de->inode && strcmp(de->name, name) == 0)
                return (int)de->inode;
        }
        checked += in_this;
    }
    return -1;
}

// Append a dirent to dir_ino. Returns 0 on success.
static int dir_append(int dir_ino, uint32_t child_ino, uint8_t type, const char* name)
{
    inode_t dir;
    rdinode((uint32_t)dir_ino, &dir);

    uint32_t per_block   = TXFS_BLOCK_SIZE / sizeof(dirent_t);
    uint32_t total_slots = dir.size / sizeof(dirent_t);
    uint32_t block_idx   = total_slots / per_block;
    uint32_t slot_in_blk = total_slots % per_block;

    uint32_t blk = get_block(&dir, block_idx, 1);
    if (!blk) return -1;

    // get_block may have updated dir.indirect — re-read to be safe
    // (we write inode at end anyway)
    uint8_t buf[TXFS_BLOCK_SIZE];
    rd(blk, buf);

    dirent_t* de = (dirent_t*)(buf + slot_in_blk * sizeof(dirent_t));
    de->inode    = child_ino;
    de->name_len = (uint16_t)strlen(name);
    de->type     = type;
    strncpy(de->name, name, 255);
    de->name[255] = 0;

    wr(blk, buf);
    dir.size += sizeof(dirent_t);
    wrinode((uint32_t)dir_ino, &dir);
    return 0;
}

// Remove entry named 'name' from dir_ino by zeroing its inode field.
static void dir_remove_entry(int dir_ino, const char* name)
{
    inode_t dir;
    rdinode((uint32_t)dir_ino, &dir);

    uint32_t per_block = TXFS_BLOCK_SIZE / sizeof(dirent_t);
    uint32_t total     = dir.size / sizeof(dirent_t);
    uint32_t checked   = 0;

    for (uint32_t b = 0; checked < total; b++) {
        uint32_t blk = get_block(&dir, b, 0);
        if (!blk) break;

        uint8_t buf[TXFS_BLOCK_SIZE];
        rd(blk, buf);

        uint32_t in_this = total - checked;
        if (in_this > per_block) in_this = per_block;

        for (uint32_t i = 0; i < in_this; i++) {
            dirent_t* de = (dirent_t*)(buf + i * sizeof(dirent_t));
            if (de->inode && strcmp(de->name, name) == 0) {
                de->inode = 0;
                wr(blk, buf);
                return;
            }
        }
        checked += in_this;
    }
}

// --- format ------------------------------------------------------------------

static void format(uint32_t total)
{
    printf("Formatting TXFS (%u blocks)...\n", total);
    memset(&sb, 0, sizeof(sb));
    sb.magic        = TXFS_MAGIC;
    sb.version      = 1;
    sb.block_size   = TXFS_BLOCK_SIZE;
    sb.total_blocks = total - TXFS_BLOCK_DATA;
    sb.free_blocks  = sb.total_blocks;
    sb.total_inodes = TXFS_MAX_INODES;
    sb.free_inodes  = TXFS_MAX_INODES - 1;
    wrsb();

    memset(imap, 0, sizeof(imap));
    memset(bmap, 0, sizeof(bmap));
    wr(TXFS_BLOCK_IBITMAP, imap);
    wr(TXFS_BLOCK_BBITMAP, bmap);

    bset(imap, 0);
    wr(TXFS_BLOCK_IBITMAP, imap);

    inode_t root = {0};
    root.mode  = (TXFS_TYPE_DIR << 12) | 0x1C0;
    root.links = 1;
    wrinode(0, &root);
    printf("Done.\n");
}

// --- ensure path exists (create dirs as needed), return leaf dir inode -------

static int ensure_dir_path(const char* txpath)
{
    // txpath is absolute, e.g. "/bin" or "/a/b/c"
    char buf[256];
    strncpy(buf, txpath, 255);
    buf[255] = 0;

    char* p = buf;
    if (*p == '/') p++;

    int cur = 0;  // root

    while (*p) {
        char* slash = strchr(p, '/');
        if (!slash) break;  // last component is the filename, stop here
        *slash = 0;

        int found = dir_lookup(cur, p);
        if (found < 0) {
            // create subdir
            int ni = ainode();
            inode_t nd = {0};
            nd.mode  = (TXFS_TYPE_DIR << 12) | 0x1C0;
            nd.links = 1;
            wrinode((uint32_t)ni, &nd);
            dir_append(cur, (uint32_t)ni, TXFS_TYPE_DIR, p);
            found = ni;
        }
        cur = found;
        p   = slash + 1;
    }

    return cur;
}

// --- write a file into the image ---------------------------------------------

static int write_file(const char* txpath, const char* lpath)
{
    FILE* f = fopen(lpath, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", lpath); return -1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);
    uint8_t* data = malloc((size_t)fsz);
    fread(data, 1, (size_t)fsz, f);
    fclose(f);

    // Split txpath into parent dir and filename
    char path_copy[256];
    strncpy(path_copy, txpath, 255);
    path_copy[255] = 0;
    char* fname = strrchr(path_copy, '/');
    if (!fname) { fprintf(stderr, "bad path: %s\n", txpath); free(data); return -1; }
    *fname = 0;
    fname++;

    int dir_ino = ensure_dir_path(txpath);

    // Remove existing file with same name (free its blocks and inode)
    int old_ino = dir_lookup(dir_ino, fname);
    if (old_ino >= 0) {
        inode_t old;
        rdinode((uint32_t)old_ino, &old);
        for (int i = 0; i < TXFS_DIRECT_BLOCKS; i++)
            if (old.blocks[i]) freeblock(old.blocks[i]);
        if (old.indirect) {
            uint32_t ptrs[TXFS_PTRS_PER_BLOCK];
            rd(old.indirect, ptrs);
            for (uint32_t i = 0; i < TXFS_PTRS_PER_BLOCK; i++)
                if (ptrs[i]) freeblock(ptrs[i]);
            freeblock(old.indirect);
        }
        freeinode((uint32_t)old_ino);
        dir_remove_entry(dir_ino, fname);
    }

    // Allocate new inode and write file data
    int ni = ainode();
    inode_t ino = {0};
    ino.mode  = (TXFS_TYPE_FILE << 12) | 0x1C0;
    ino.links = 1;
    ino.size  = (uint32_t)fsz;

    uint32_t done = 0;
    uint32_t bidx = 0;
    while (done < (uint32_t)fsz) {
        uint32_t blk = get_block(&ino, bidx++, 1);
        if (!blk) { fprintf(stderr, "out of blocks writing %s\n", txpath); break; }

        uint8_t blkbuf[TXFS_BLOCK_SIZE] = {0};
        uint32_t chunk = (uint32_t)fsz - done;
        if (chunk > TXFS_BLOCK_SIZE) chunk = TXFS_BLOCK_SIZE;
        memcpy(blkbuf, data + done, chunk);
        wr(blk, blkbuf);
        done += chunk;
    }
    wrinode((uint32_t)ni, &ino);

    dir_append(dir_ino, (uint32_t)ni, TXFS_TYPE_FILE, fname);

    free(data);
    printf("  %s -> %s (%ld bytes)\n", lpath, txpath, fsz);
    return 0;
}

// --- main --------------------------------------------------------------------

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <disk.img> [<local_file> <txfs_path>]...\n", argv[0]);
        return 1;
    }

    disk = fopen(argv[1], "r+b");
    if (!disk) {
        // Create new image
        disk = fopen(argv[1], "w+b");
        if (!disk) { perror("open"); return 1; }
        for (int i = 0; i < 25600; i++) {
            fwrite(zero_block, TXFS_BLOCK_SIZE, 1, disk);
        }
    }

    rdsb();
    if (sb.magic != TXFS_MAGIC) {
        fseek(disk, 0, SEEK_END);
        long dsz = ftell(disk);
        format((uint32_t)(dsz / TXFS_BLOCK_SIZE));
        rdsb();
    }

    for (int i = 2; i + 1 < argc; i += 2) {
        if (argv[i] && argv[i + 1])
            write_file(argv[i + 1], argv[i]);
    }

    fclose(disk);
    return 0;
}
