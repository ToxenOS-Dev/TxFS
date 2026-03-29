// ToxenOS/kernel/txfs.c
// TxFS — ToxenOS native filesystem
//
// Improvements over v1:
//   - txfs_mkdir: correctly resolves any-depth parent, not always root
//   - txfs_open (create): same fix; robust parent extraction
//   - txfs_remove: finds file in its actual parent dir, not always root
//   - Directories: entries spread across multiple blocks (no more 15-entry cap)
//   - Single indirect block: files up to ~4 MB (12 direct + 1024 indirect ptrs)
//   - free_blocks/free_inodes properly maintained on remove
//   - txfs_alloc_block: always clears the new block on every allocation

#include <stdint.h>
#include "../include/txfs.h"
#include "../include/ata.h"
#include "../include/mm.h"
#include "../include/vga.h"

static char txfs_mountpoint[64] = "/disk";

static const char* txfs_strip_mount(const char* path)
{
    int i = 0;
    while (txfs_mountpoint[i] && path[i] == txfs_mountpoint[i]) i++;
    if (!path[i]) return "/";
    return path + i;
}

// --- string helpers ----------------------------------------------------------

static int txfs_strlen(const char* s)
{
    int i = 0; while (s[i]) i++; return i;
}

static void txfs_strcpy(char* dst, const char* src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int txfs_strcmp(const char* a, const char* b)
{
    int i;
    for (i = 0; a[i] && b[i]; i++)
        if (a[i] != b[i]) return 1;
    return a[i] != b[i];
}

// --- block I/O ---------------------------------------------------------------

static uint8_t block_buf[TXFS_BLOCK_SIZE];

static int txfs_read_block(uint32_t block, uint8_t* buf)
{
    uint32_t sectors = TXFS_BLOCK_SIZE / 512;
    return ata_read(block * sectors, buf, sectors);
}

static int txfs_write_block(uint32_t block, const uint8_t* buf)
{
    uint32_t sectors = TXFS_BLOCK_SIZE / 512;
    return ata_write(block * sectors, buf, sectors);
}

// --- superblock --------------------------------------------------------------

static txfs_superblock_t sb;

static int txfs_read_super()
{
    if (txfs_read_block(TXFS_BLOCK_SUPER, (uint8_t*)&sb) < 0) return -1;
    if (sb.magic != TXFS_MAGIC) return -1;
    return 0;
}

static int txfs_write_super()
{
    return txfs_write_block(TXFS_BLOCK_SUPER, (uint8_t*)&sb);
}

// --- bitmap helpers ----------------------------------------------------------

static uint8_t inode_bitmap[TXFS_BLOCK_SIZE];
static uint8_t block_bitmap[TXFS_BLOCK_SIZE];

static int bitmap_test(uint8_t* bm, uint32_t bit)
{
    return (bm[bit / 8] >> (bit % 8)) & 1;
}

static void bitmap_set(uint8_t* bm, uint32_t bit)
{
    bm[bit / 8] |= (1 << (bit % 8));
}

static void bitmap_clear(uint8_t* bm, uint32_t bit)
{
    bm[bit / 8] &= ~(1 << (bit % 8));
}

static int bitmap_alloc(uint8_t* bm, uint32_t max)
{
    for (uint32_t i = 0; i < max; i++)
        if (!bitmap_test(bm, i)) { bitmap_set(bm, i); return i; }
    return -1;
}

// --- inode I/O ---------------------------------------------------------------

#define INODES_PER_BLOCK    16  // 4096 / 256 = 16

static int txfs_read_inode(uint32_t num, txfs_inode_t* inode)
{
    uint32_t block  = TXFS_BLOCK_INODES + (num / INODES_PER_BLOCK);
    uint32_t offset = (num % INODES_PER_BLOCK) * sizeof(txfs_inode_t);

    if (txfs_read_block(block, block_buf) < 0) return -1;

    uint8_t* src = block_buf + offset;
    uint8_t* dst = (uint8_t*)inode;
    for (uint32_t i = 0; i < sizeof(txfs_inode_t); i++)
        dst[i] = src[i];

    return 0;
}

static int txfs_write_inode(uint32_t num, const txfs_inode_t* inode)
{
    uint32_t block  = TXFS_BLOCK_INODES + (num / INODES_PER_BLOCK);
    uint32_t offset = (num % INODES_PER_BLOCK) * sizeof(txfs_inode_t);

    if (txfs_read_block(block, block_buf) < 0) return -1;

    uint8_t* dst = block_buf + offset;
    uint8_t* src = (uint8_t*)inode;
    for (uint32_t i = 0; i < sizeof(txfs_inode_t); i++)
        dst[i] = src[i];

    return txfs_write_block(block, block_buf);
}

// --- block allocation --------------------------------------------------------

static uint8_t zero_block[TXFS_BLOCK_SIZE];

static int txfs_alloc_block()
{
    if (txfs_read_block(TXFS_BLOCK_BBITMAP, block_bitmap) < 0) return -1;

    int b = bitmap_alloc(block_bitmap, sb.total_blocks);
    if (b < 0) return -1;

    txfs_write_block(TXFS_BLOCK_BBITMAP, block_bitmap);
    sb.free_blocks--;
    txfs_write_super();

    // always clear new block so stale data never leaks
    txfs_write_block((uint32_t)(b + TXFS_BLOCK_DATA), zero_block);

    return b + TXFS_BLOCK_DATA;
}

static void txfs_free_block(uint32_t block_num)
{
    if (block_num < TXFS_BLOCK_DATA) return;
    txfs_read_block(TXFS_BLOCK_BBITMAP, block_bitmap);
    bitmap_clear(block_bitmap, block_num - TXFS_BLOCK_DATA);
    txfs_write_block(TXFS_BLOCK_BBITMAP, block_bitmap);
    sb.free_blocks++;
    txfs_write_super();
}

static int txfs_alloc_inode()
{
    if (txfs_read_block(TXFS_BLOCK_IBITMAP, inode_bitmap) < 0) return -1;

    int i = bitmap_alloc(inode_bitmap, sb.total_inodes);
    if (i < 0) return -1;

    txfs_write_block(TXFS_BLOCK_IBITMAP, inode_bitmap);
    sb.free_inodes--;
    txfs_write_super();
    return i;
}

static void txfs_free_inode(uint32_t inum)
{
    txfs_read_block(TXFS_BLOCK_IBITMAP, inode_bitmap);
    bitmap_clear(inode_bitmap, inum);
    txfs_write_block(TXFS_BLOCK_IBITMAP, inode_bitmap);
    sb.free_inodes++;
    txfs_write_super();
}

// --- indirect block helpers --------------------------------------------------

// TXFS_PTRS_PER_BLOCK: a 4096-byte block holds 1024 uint32_t pointers
#define TXFS_PTRS_PER_BLOCK  (TXFS_BLOCK_SIZE / 4)

// Return the physical block number for logical block index idx within inode.
// If alloc=1, allocates missing indirect/data blocks.
// Returns 0 if the block doesn't exist and alloc=0.
static uint32_t txfs_get_block(txfs_inode_t* inode, uint32_t idx, int alloc)
{
    if (idx < TXFS_DIRECT_BLOCKS) {
        if (!inode->blocks[idx] && alloc) {
            int b = txfs_alloc_block();
            if (b < 0) return 0;
            inode->blocks[idx] = (uint32_t)b;
        }
        return inode->blocks[idx];
    }

    uint32_t indirect_idx = idx - TXFS_DIRECT_BLOCKS;
    if (indirect_idx >= TXFS_PTRS_PER_BLOCK)
        return 0;  // beyond single-indirect range

    if (!inode->indirect) {
        if (!alloc) return 0;
        int b = txfs_alloc_block();
        if (b < 0) return 0;
        inode->indirect = (uint32_t)b;
    }

    uint32_t ptrs[TXFS_PTRS_PER_BLOCK];
    txfs_read_block(inode->indirect, (uint8_t*)ptrs);

    if (!ptrs[indirect_idx]) {
        if (!alloc) return 0;
        int b = txfs_alloc_block();
        if (b < 0) return 0;
        ptrs[indirect_idx] = (uint32_t)b;
        txfs_write_block(inode->indirect, (uint8_t*)ptrs);
    }

    return ptrs[indirect_idx];
}

// --- format ------------------------------------------------------------------

int txfs_format(uint32_t total_blocks)
{
    uint8_t* p = (uint8_t*)&sb;
    for (uint32_t i = 0; i < sizeof(sb); i++) p[i] = 0;

    sb.magic        = TXFS_MAGIC;
    sb.version      = TXFS_VERSION;
    sb.block_size   = TXFS_BLOCK_SIZE;
    sb.total_blocks = total_blocks - TXFS_BLOCK_DATA;
    sb.free_blocks  = sb.total_blocks;
    sb.total_inodes = TXFS_MAX_INODES;
    sb.free_inodes  = TXFS_MAX_INODES - 1;
    sb.root_inode   = 0;

    txfs_write_super();

    for (int i = 0; i < TXFS_BLOCK_SIZE; i++)
        inode_bitmap[i] = block_bitmap[i] = 0;

    bitmap_set(inode_bitmap, 0);

    txfs_write_block(TXFS_BLOCK_IBITMAP, inode_bitmap);
    txfs_write_block(TXFS_BLOCK_BBITMAP, block_bitmap);

    txfs_inode_t root;
    p = (uint8_t*)&root;
    for (uint32_t i = 0; i < sizeof(root); i++) p[i] = 0;

    root.mode  = (TXFS_TYPE_DIR << 12) |
                 TXFS_PERM_OWNER_R | TXFS_PERM_OWNER_W | TXFS_PERM_OWNER_X;
    root.links = 1;
    root.size  = 0;

    txfs_write_inode(0, &root);

    return 0;
}

// --- directory helpers -------------------------------------------------------

// Search directory inode for a named entry. Returns child inode number or -1.
static int txfs_dir_lookup(txfs_inode_t* dir, const char* name)
{
    uint8_t  data_buf[TXFS_BLOCK_SIZE];
    uint32_t entries_total = dir->size / sizeof(txfs_dirent_t);
    uint32_t checked = 0;

    for (int b = 0; checked < entries_total; b++) {
        uint32_t blk = txfs_get_block(dir, (uint32_t)b, 0);
        if (!blk) break;
        txfs_read_block(blk, data_buf);

        uint32_t per_block = TXFS_BLOCK_SIZE / sizeof(txfs_dirent_t);
        uint32_t in_this   = entries_total - checked;
        if (in_this > per_block) in_this = per_block;

        for (uint32_t i = 0; i < in_this; i++) {
            txfs_dirent_t* de = (txfs_dirent_t*)(data_buf + i * sizeof(txfs_dirent_t));
            if (de->inode && !txfs_strcmp(de->name, name))
                return (int)de->inode;
        }
        checked += in_this;
    }
    return -1;
}

// Append a new dirent to a directory, allocating an extra data block if needed.
static int txfs_dir_append(int dir_inum, txfs_inode_t* dir,
                           uint32_t child_inum, uint8_t type, const char* name)
{
    uint32_t per_block   = TXFS_BLOCK_SIZE / sizeof(txfs_dirent_t);
    uint32_t total_slots = dir->size / sizeof(txfs_dirent_t);
    uint32_t block_idx   = total_slots / per_block;
    uint32_t slot_in_blk = total_slots % per_block;

    uint8_t  data_buf[TXFS_BLOCK_SIZE];
    uint32_t blk = txfs_get_block(dir, block_idx, 1);
    if (!blk) return -1;

    txfs_read_block(blk, data_buf);
    txfs_dirent_t* de = (txfs_dirent_t*)(data_buf + slot_in_blk * sizeof(txfs_dirent_t));

    de->inode    = child_inum;
    de->name_len = (uint16_t)txfs_strlen(name);
    de->type     = type;
    txfs_strcpy(de->name, name, 256);

    txfs_write_block(blk, data_buf);
    dir->size += sizeof(txfs_dirent_t);
    txfs_write_inode((uint32_t)dir_inum, dir);

    return 0;
}

// Mark a dirent as deleted (inode=0). Does NOT compact the directory.
static int txfs_dir_remove_entry(int dir_inum, txfs_inode_t* dir, const char* name)
{
    uint8_t  data_buf[TXFS_BLOCK_SIZE];
    uint32_t entries_total = dir->size / sizeof(txfs_dirent_t);
    uint32_t checked = 0;

    for (int b = 0; checked < entries_total; b++) {
        uint32_t blk = txfs_get_block(dir, (uint32_t)b, 0);
        if (!blk) break;
        txfs_read_block(blk, data_buf);

        uint32_t per_block = TXFS_BLOCK_SIZE / sizeof(txfs_dirent_t);
        uint32_t in_this   = entries_total - checked;
        if (in_this > per_block) in_this = per_block;

        for (uint32_t i = 0; i < in_this; i++) {
            txfs_dirent_t* de = (txfs_dirent_t*)(data_buf + i * sizeof(txfs_dirent_t));
            if (de->inode && !txfs_strcmp(de->name, name)) {
                de->inode = 0;
                txfs_write_block(blk, data_buf);
                (void)dir_inum;
                return 0;
            }
        }
        checked += in_this;
    }
    return -1;
}

// --- path helpers ------------------------------------------------------------

// Resolve a full path to an inode number, or -1 if not found.
static int txfs_lookup(const char* path)
{
    if (!txfs_strcmp(path, "/")) return 0;

    const char* p = path;
    if (*p == '/') p++;

    int cur = 0;

    while (*p) {
        char component[256];
        int  len = 0;
        while (p[len] && p[len] != '/') len++;
        for (int i = 0; i < len; i++) component[i] = p[i];
        component[len] = 0;
        p += len;
        if (*p == '/') p++;

        txfs_inode_t inode;
        if (txfs_read_inode((uint32_t)cur, &inode) < 0) return -1;

        int type = (inode.mode >> 12) & 0xF;
        if (type != TXFS_TYPE_DIR) return -1;

        cur = txfs_dir_lookup(&inode, component);
        if (cur < 0) return -1;
    }

    return cur;
}

// Split /a/b/c into parent="/a/b" and name="c".
static void txfs_split_path(const char* path, char* parent_out, char* name_out)
{
    int len   = txfs_strlen(path);
    int slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { slash = i; break; }
    }

    if (slash <= 0) {
        txfs_strcpy(parent_out, "/", 256);
        const char* n = path;
        if (*n == '/') n++;
        txfs_strcpy(name_out, n, 256);
    } else {
        for (int i = 0; i < slash; i++) parent_out[i] = path[i];
        parent_out[slash] = 0;
        txfs_strcpy(name_out, path + slash + 1, 256);
    }
}

// --- VFS driver functions ----------------------------------------------------

#define TXFS_MAX_FDS 32
static txfs_fd_t open_files[TXFS_MAX_FDS];

static int txfs_mount_fn(const char* device)
{
    for (int i = 0; i < TXFS_MAX_FDS; i++)
        open_files[i].used = 0;

    if (txfs_read_super() < 0) {
        txfs_format(204800);
        txfs_read_super();
    }

    return 0;
}

static int txfs_open_fn(const char* path, int flags)
{
    const char* local = txfs_strip_mount(path);
    int inode_num = txfs_lookup(local);

    if (inode_num < 0) {
        if (!(flags & VFS_O_CREATE)) return -1;

        int new_inum = txfs_alloc_inode();
        if (new_inum < 0) return -1;

        txfs_inode_t inode;
        uint8_t* p = (uint8_t*)&inode;
        for (uint32_t i = 0; i < sizeof(inode); i++) p[i] = 0;
        inode.mode  = (TXFS_TYPE_FILE << 12) | TXFS_PERM_OWNER_R | TXFS_PERM_OWNER_W;
        inode.links = 1;
        inode.size  = 0;
        txfs_write_inode((uint32_t)new_inum, &inode);

        char parent_path[256], filename[256];
        txfs_split_path(local, parent_path, filename);

        int parent_inum = txfs_lookup(parent_path);
        if (parent_inum < 0) parent_inum = 0;

        txfs_inode_t parent;
        txfs_read_inode((uint32_t)parent_inum, &parent);
        txfs_dir_append(parent_inum, &parent, (uint32_t)new_inum, TXFS_TYPE_FILE, filename);

        inode_num = new_inum;
    }

    for (int i = 0; i < TXFS_MAX_FDS; i++) {
        if (!open_files[i].used) {
            open_files[i].used      = 1;
            open_files[i].inode_num = (uint32_t)inode_num;
            open_files[i].position  = 0;
            txfs_read_inode((uint32_t)inode_num, &open_files[i].inode);
            return i;
        }
    }

    return -1;
}

static int txfs_close_fn(int fd)
{
    if (fd < 0 || fd >= TXFS_MAX_FDS || !open_files[fd].used) return -1;
    open_files[fd].used = 0;
    return 0;
}

static int txfs_read_fn(int fd, uint8_t* buf, uint32_t size)
{
    if (fd < 0 || fd >= TXFS_MAX_FDS || !open_files[fd].used) return -1;

    txfs_fd_t*    f     = &open_files[fd];
    txfs_inode_t* inode = &f->inode;

    if (f->position >= inode->size) return 0;

    uint32_t to_read = size;
    if (f->position + to_read > inode->size)
        to_read = inode->size - f->position;

    uint32_t done = 0;
    uint8_t  data_buf[TXFS_BLOCK_SIZE];

    while (done < to_read) {
        uint32_t block_idx    = (f->position + done) / TXFS_BLOCK_SIZE;
        uint32_t block_offset = (f->position + done) % TXFS_BLOCK_SIZE;

        uint32_t blk = txfs_get_block(inode, block_idx, 0);
        if (!blk) break;

        txfs_read_block(blk, data_buf);

        uint32_t can_read = TXFS_BLOCK_SIZE - block_offset;
        if (can_read > to_read - done) can_read = to_read - done;

        for (uint32_t i = 0; i < can_read; i++)
            buf[done + i] = data_buf[block_offset + i];

        done += can_read;
    }

    f->position += done;
    return (int)done;
}

static int txfs_write_fn(int fd, const uint8_t* buf, uint32_t size)
{
    if (fd < 0 || fd >= TXFS_MAX_FDS || !open_files[fd].used) return -1;

    txfs_fd_t*    f     = &open_files[fd];
    txfs_inode_t* inode = &f->inode;

    uint32_t done  = 0;
    int      dirty = 0;
    uint8_t  data_buf[TXFS_BLOCK_SIZE];

    while (done < size) {
        uint32_t block_idx    = (f->position + done) / TXFS_BLOCK_SIZE;
        uint32_t block_offset = (f->position + done) % TXFS_BLOCK_SIZE;

        uint32_t blk = txfs_get_block(inode, block_idx, 1);
        if (!blk) break;

        txfs_read_block(blk, data_buf);

        uint32_t can_write = TXFS_BLOCK_SIZE - block_offset;
        if (can_write > size - done) can_write = size - done;

        for (uint32_t i = 0; i < can_write; i++)
            data_buf[block_offset + i] = buf[done + i];

        txfs_write_block(blk, data_buf);
        done += can_write;
        dirty = 1;
    }

    if (dirty) {
        f->position += done;
        if (f->position > inode->size)
            inode->size = f->position;
        txfs_write_inode(f->inode_num, inode);
    }

    return (int)done;
}

static int txfs_mkdir_fn(const char* path)
{
    const char* local = txfs_strip_mount(path);

    if (txfs_lookup(local) >= 0) return -1;  // already exists

    int new_inum = txfs_alloc_inode();
    if (new_inum < 0) return -1;

    txfs_inode_t inode;
    uint8_t* p = (uint8_t*)&inode;
    for (uint32_t i = 0; i < sizeof(inode); i++) p[i] = 0;

    inode.mode  = (TXFS_TYPE_DIR << 12) |
                  TXFS_PERM_OWNER_R | TXFS_PERM_OWNER_W | TXFS_PERM_OWNER_X;
    inode.links = 1;
    inode.size  = 0;
    txfs_write_inode((uint32_t)new_inum, &inode);

    char parent_path[256], dirname[256];
    txfs_split_path(local, parent_path, dirname);

    int parent_inum = txfs_lookup(parent_path);
    if (parent_inum < 0) parent_inum = 0;

    txfs_inode_t parent;
    txfs_read_inode((uint32_t)parent_inum, &parent);
    txfs_dir_append(parent_inum, &parent, (uint32_t)new_inum, TXFS_TYPE_DIR, dirname);

    return 0;
}

static int txfs_remove_fn(const char* path)
{
    const char* local = txfs_strip_mount(path);
    int inode_num = txfs_lookup(local);
    if (inode_num < 0) return -1;

    txfs_inode_t inode;
    txfs_read_inode((uint32_t)inode_num, &inode);

    // free direct blocks
    for (int i = 0; i < TXFS_DIRECT_BLOCKS; i++) {
        if (inode.blocks[i])
            txfs_free_block(inode.blocks[i]);
    }

    // free indirect block and all blocks it points to
    if (inode.indirect) {
        uint32_t ptrs[TXFS_PTRS_PER_BLOCK];
        txfs_read_block(inode.indirect, (uint8_t*)ptrs);
        for (uint32_t i = 0; i < TXFS_PTRS_PER_BLOCK; i++) {
            if (ptrs[i]) txfs_free_block(ptrs[i]);
        }
        txfs_free_block(inode.indirect);
    }

    txfs_free_inode((uint32_t)inode_num);

    // remove dirent from the correct parent
    char parent_path[256], name[256];
    txfs_split_path(local, parent_path, name);

    int parent_inum = txfs_lookup(parent_path);
    if (parent_inum < 0) parent_inum = 0;

    txfs_inode_t parent;
    txfs_read_inode((uint32_t)parent_inum, &parent);
    txfs_dir_remove_entry(parent_inum, &parent, name);

    return 0;
}

static int txfs_isdir_fn(const char* path)
{
    const char* local = txfs_strip_mount(path);
    int inode_num = txfs_lookup(local);
    if (inode_num < 0) return -1;

    txfs_inode_t inode;
    txfs_read_inode((uint32_t)inode_num, &inode);
    int type = (inode.mode >> 12) & 0xF;
    return (type == TXFS_TYPE_DIR) ? 1 : 0;
}

static int txfs_readdir_fn(const char* path, char* out, uint32_t index)
{
    const char* local = txfs_strip_mount(path);
    int inode_num = txfs_lookup(local);
    if (inode_num < 0) return -1;

    txfs_inode_t inode;
    if (txfs_read_inode((uint32_t)inode_num, &inode) < 0) return -1;

    int type = (inode.mode >> 12) & 0xF;
    if (type != TXFS_TYPE_DIR) return -1;

    uint8_t  data_buf[TXFS_BLOCK_SIZE];
    uint32_t entries_total = inode.size / sizeof(txfs_dirent_t);
    uint32_t count   = 0;
    uint32_t checked = 0;

    for (int b = 0; checked < entries_total; b++) {
        uint32_t blk = txfs_get_block(&inode, (uint32_t)b, 0);
        if (!blk) break;
        txfs_read_block(blk, data_buf);

        uint32_t per_block = TXFS_BLOCK_SIZE / sizeof(txfs_dirent_t);
        uint32_t in_this   = entries_total - checked;
        if (in_this > per_block) in_this = per_block;

        for (uint32_t i = 0; i < in_this; i++) {
            txfs_dirent_t* de = (txfs_dirent_t*)(data_buf + i * sizeof(txfs_dirent_t));
            if (de->inode) {
                if (count == index) {
                    txfs_strcpy(out, de->name, 256);
                    return 0;
                }
                count++;
            }
        }
        checked += in_this;
    }

    return -1;
}

static int txfs_stat_fn(const char* path, uint32_t* size)
{
    const char* local = txfs_strip_mount(path);
    int inode_num = txfs_lookup(local);
    if (inode_num < 0) return -1;

    txfs_inode_t inode;
    if (txfs_read_inode((uint32_t)inode_num, &inode) < 0) return -1;

    *size = inode.size;
    return 0;
}

// --- driver registration -----------------------------------------------------

static fs_driver_t txfs_driver = {
    .name    = "txfs",
    .mount   = txfs_mount_fn,
    .open    = txfs_open_fn,
    .close   = txfs_close_fn,
    .read    = txfs_read_fn,
    .write   = txfs_write_fn,
    .readdir = txfs_readdir_fn,
    .stat    = txfs_stat_fn,
    .mkdir   = txfs_mkdir_fn,
    .remove  = txfs_remove_fn,
    .isdir   = txfs_isdir_fn,
};

fs_driver_t* txfs_init()
{
    return &txfs_driver;
}
