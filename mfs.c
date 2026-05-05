#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <fuse3/fuse.h>

#define BLOCKSIZE 16384 // bytes - 16 KB
#define MAXFILENAME 32  // maximum filename that can be stored by mfs

#define SUPERBLOCK_BLOCK 0
#define BITMAP_BLOCK     1
#define INODEMAP_BLOCK   2
#define INODE_BLOCK_1    3
#define INODE_BLOCK_2    4
#define ROOTDIR_BLOCK    5

#define NUM_INODES       256
#define NUM_DIRENTRIES   256
#define INODES_PER_BLOCK 128
#define MAX_BLOCK_PTRS   (BLOCKSIZE / 4)  // 4096 pointers in an index block

// Bit layout: [ V | M | R | P2 | P1 | P0 | X | X ]
#define INODE_VALID_BIT      0x80  // 1000 0000 (Top bit)
#define INODE_MODIFIED_BIT   0x40  // 0100 0000 (2nd bit)
#define INODE_REFERENCE_BIT  0x20  // 0010 0000 (3rd bit)
#define INODE_PERM_MASK      0x1C  // 0001 1100 (4th-6th bits for permissions: R, W, X)
#define INODE_PERM_READ      0x10  // 0001 0000 (Read permission)
#define INODE_PERM_WRITE     0x08  // 0000 1000 (Write permission)
#define INODE_PERM_EXECUTE   0x04  // 0000 0100 (Execute permission)

int mfs_getattr( const char *, struct stat *, struct fuse_file_info * );
int mfs_readdir( const char *, void *, fuse_fill_dir_t, off_t,
                 struct fuse_file_info *, enum fuse_readdir_flags);
int mfs_open( const char *, struct fuse_file_info * );
int mfs_read( const char *, char *, size_t, off_t,
              struct fuse_file_info * );
int mfs_release(const char *path, struct fuse_file_info *fi);
int mfs_write(const char *path, const char *buf, size_t size, off_t offset,
              struct fuse_file_info *fi);
int mfs_create(const char *path, mode_t mode, struct fuse_file_info *fi);
int mfs_mknod(const char *path, mode_t mode, dev_t rdev);
int mfs_unlink(const char *path);
int mfs_truncate(const char *path, off_t size, struct fuse_file_info *fi);
int mfs_utimens(const char *path, const struct timespec ts[2],
                struct fuse_file_info *fi);

int mfs_init();

/* Utility functions */
int read_block (void *block, int k);
int write_block (void *block, int k);

static struct fuse_operations mfs_oper = {
    .getattr    = mfs_getattr,
    .readdir    = mfs_readdir,
    .mknod      = mfs_mknod,
    .create     = mfs_create,
    .mkdir      = NULL,
    .unlink     = mfs_unlink,
    .rmdir      = NULL,
    .truncate   = mfs_truncate,
    .open       = mfs_open,
    .read       = mfs_read,
    .release    = mfs_release,
    .write      = mfs_write,
    .rename     = NULL,
    .utimens    = mfs_utimens
};

struct superblock // Resides in Block 0
{
    int num_blocks;     // total number of blocks in the disk
    int block_size;     // 16KB
    int max_files;      // 254 max files (256 - 2 for . and ..)
    int num_inodes;     // 256 total inodes
};

struct direntry // Resides Block 5
{
    char filename[MAXFILENAME];     // 32 bytes for the file name
    int inode_number;               // 4 bytes for the inode number
    char padding[28];                // padding to make the size of direntrys 64 bytes
}; // A single block of size 16KB can store 256 direntrys (16KB / 64 bytes = 256)

struct inode {
    int inode_number;     // 4 bytes
    int file_size;        // 4 bytes (in bytes)
    int block_count;      // 4 bytes (number of data blocks allocated)
    int index_block;      // 4 bytes (logical block number of the index block)

    uint8_t status;       // 1 byte (8 status bits: [ V | M | R | P2 | P1 | P0 | X | X ])

    char padding[111];    // Padding to ensure the struct is exactly 128 bytes
}; // A single block of size 16KB can store 128 inodes (16KB / 128 bytes = 128) so 2 blocks give the total 256 inodes


// ********** Global Variables ***************************************
int fd_disk;


// ===================== Bitmap / inode-map helpers =====================

static int bitmap_get(const unsigned char *bm, int idx) {
    return (bm[idx / 8] >> (idx % 8)) & 1;
}

static void bitmap_set(unsigned char *bm, int idx) {
    bm[idx / 8] |= (1 << (idx % 8));
}

static void bitmap_clear(unsigned char *bm, int idx) {
    bm[idx / 8] &= ~(1 << (idx % 8));
}

// Allocate a free data block. Returns block number (>=6) or -1 if full.
static int alloc_block(void) {
    unsigned char buf[BLOCKSIZE];
    if (read_block(buf, BITMAP_BLOCK) < 0) return -1;

    // Read superblock to know how many blocks the disk has
    char sbbuf[BLOCKSIZE];
    if (read_block(sbbuf, SUPERBLOCK_BLOCK) < 0) return -1;
    struct superblock *sb = (struct superblock *)sbbuf;
    int total = sb->num_blocks;

    for (int b = ROOTDIR_BLOCK + 1; b < total; b++) {
        if (!bitmap_get(buf, b)) {
            bitmap_set(buf, b);
            if (write_block(buf, BITMAP_BLOCK) < 0) return -1;
            return b;
        }
    }
    return -1;
}

static int free_block(int blocknum) {
    unsigned char buf[BLOCKSIZE];
    if (read_block(buf, BITMAP_BLOCK) < 0) return -1;
    bitmap_clear(buf, blocknum);
    return write_block(buf, BITMAP_BLOCK);
}

// Allocate a free inode. Returns inode number (>=2) or -1 if full.
static int alloc_inode(void) {
    unsigned char buf[BLOCKSIZE];
    if (read_block(buf, INODEMAP_BLOCK) < 0) return -1;

    for (int i = 2; i < NUM_INODES; i++) {
        if (!bitmap_get(buf, i)) {
            bitmap_set(buf, i);
            if (write_block(buf, INODEMAP_BLOCK) < 0) return -1;
            return i;
        }
    }
    return -1;
}

static int free_inode(int inum) {
    unsigned char buf[BLOCKSIZE];
    if (read_block(buf, INODEMAP_BLOCK) < 0) return -1;
    bitmap_clear(buf, inum);
    return write_block(buf, INODEMAP_BLOCK);
}

// ===================== Inode read/write helpers =====================

static int read_inode(int inum, struct inode *out) {
    if (inum < 0 || inum >= NUM_INODES) return -1;
    int blk = (inum < INODES_PER_BLOCK) ? INODE_BLOCK_1 : INODE_BLOCK_2;
    int idx = (inum < INODES_PER_BLOCK) ? inum : (inum - INODES_PER_BLOCK);
    char buf[BLOCKSIZE];
    if (read_block(buf, blk) < 0) return -1;
    struct inode *table = (struct inode *)buf;
    *out = table[idx];
    return 0;
}

static int write_inode(int inum, const struct inode *in) {
    if (inum < 0 || inum >= NUM_INODES) return -1;
    int blk = (inum < INODES_PER_BLOCK) ? INODE_BLOCK_1 : INODE_BLOCK_2;
    int idx = (inum < INODES_PER_BLOCK) ? inum : (inum - INODES_PER_BLOCK);
    char buf[BLOCKSIZE];
    if (read_block(buf, blk) < 0) return -1;
    struct inode *table = (struct inode *)buf;
    table[idx] = *in;
    return write_block(buf, blk);
}

// ===================== Directory helpers =====================

// Look up a filename in the root directory. Returns inode number or -1.
static int dir_lookup(const char *filename, int *out_entry_idx) {
    char buf[BLOCKSIZE];
    if (read_block(buf, ROOTDIR_BLOCK) < 0) return -1;
    struct direntry *dir = (struct direntry *)buf;
    for (int i = 0; i < NUM_DIRENTRIES; i++) {
        if (dir[i].inode_number > 0 &&
            strncmp(dir[i].filename, filename, MAXFILENAME) == 0) {
            if (out_entry_idx) *out_entry_idx = i;
            return dir[i].inode_number;
        }
    }
    return -1;
}

// Add a new directory entry. Returns 0 on success, -errno on failure.
static int dir_add(const char *filename, int inum) {
    char buf[BLOCKSIZE];
    if (read_block(buf, ROOTDIR_BLOCK) < 0) return -EIO;
    struct direntry *dir = (struct direntry *)buf;

    // Check for duplicate
    for (int i = 0; i < NUM_DIRENTRIES; i++) {
        if (dir[i].inode_number > 0 &&
            strncmp(dir[i].filename, filename, MAXFILENAME) == 0) {
            return -EEXIST;
        }
    }

    // Find a free slot
    for (int i = 0; i < NUM_DIRENTRIES; i++) {
        if (dir[i].inode_number == 0) {
            memset(&dir[i], 0, sizeof(struct direntry));
            strncpy(dir[i].filename, filename, MAXFILENAME - 1);
            dir[i].filename[MAXFILENAME - 1] = '\0';
            dir[i].inode_number = inum;
            if (write_block(buf, ROOTDIR_BLOCK) < 0) return -EIO;
            return 0;
        }
    }
    return -ENOSPC;
}

// Remove a directory entry by filename. Returns inode number on success, -1.
static int dir_remove(const char *filename) {
    char buf[BLOCKSIZE];
    if (read_block(buf, ROOTDIR_BLOCK) < 0) return -1;
    struct direntry *dir = (struct direntry *)buf;
    for (int i = 0; i < NUM_DIRENTRIES; i++) {
        if (dir[i].inode_number > 0 &&
            strncmp(dir[i].filename, filename, MAXFILENAME) == 0) {
            int inum = dir[i].inode_number;
            memset(&dir[i], 0, sizeof(struct direntry));
            if (write_block(buf, ROOTDIR_BLOCK) < 0) return -1;
            return inum;
        }
    }
    return -1;
}

// Validate filename per spec: letters, digits, '.', '-', '_', length<=31.
static int valid_filename(const char *name) {
    if (!name || !*name) return 0;
    size_t n = strlen(name);
    if (n >= MAXFILENAME) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

// Free all data blocks of a file (including the index block).
static int free_file_blocks(struct inode *ino) {
    if (ino->index_block <= 0 || ino->block_count <= 0) {
        ino->index_block = 0;
        ino->block_count = 0;
        ino->file_size = 0;
        return 0;
    }
    char ibuf[BLOCKSIZE];
    if (read_block(ibuf, ino->index_block) < 0) return -EIO;
    int *ptrs = (int *)ibuf;
    for (int i = 0; i < ino->block_count && i < MAX_BLOCK_PTRS; i++) {
        if (ptrs[i] > 0) free_block(ptrs[i]);
    }
    free_block(ino->index_block);
    ino->index_block = 0;
    ino->block_count = 0;
    ino->file_size = 0;
    return 0;
}


// ===================== FUSE callbacks =====================

int mfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void) fi;
    printf("getattr: (path=%s)\n", path);

    memset(stbuf, 0, sizeof(struct stat));

    // 1. Handle the Root Directory
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755; // It's a directory (S_IFDIR) with rwxr-xr-x permissions
        stbuf->st_nlink = 2;             // Standard for directories
        return 0;
    }

    // 2. Handle Files in the Root Directory
    // Paths come in as "/filename". We want to skip the '/' to just get "filename".
    const char *filename = path + 1;

    int target_inode_num = dir_lookup(filename, NULL);
    if (target_inode_num < 0) return -ENOENT;

    // 3. We found the file! Now read its Inode.
    struct inode ino;
    if (read_inode(target_inode_num, &ino) < 0) return -EIO;

    // 4. Populate the stat buffer with the file's metadata
    stbuf->st_mode = S_IFREG | 0666; // It's a regular file (S_IFREG) with rw-rw-rw-
    stbuf->st_nlink = 1;             // Files usually have 1 link
    stbuf->st_size = ino.file_size;
    stbuf->st_blocks = ino.block_count * (BLOCKSIZE / 512);

    return 0;
}


int mfs_readdir( const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                struct fuse_file_info *fi, enum fuse_readdir_flags fl) {
    (void) offset;
    (void) fi;
    (void) fl;
    printf("readdir: (path=%s)\n", path);

    if (strcmp(path, "/") != 0)
        return -ENOENT;

    char block[BLOCKSIZE];
    if (read_block(block, ROOTDIR_BLOCK) < 0) return -EIO;
    struct direntry *dir = (struct direntry *)block;

    for (int i = 0; i < NUM_DIRENTRIES; i++) {
        if (dir[i].inode_number > 0) {
            filler(buf, dir[i].filename, NULL, 0, 0);
        }
    }

    return 0;
}

int mfs_open( const char *path, struct fuse_file_info *fi ) {
    (void) fi;
    printf("open: (path=%s)\n", path);

    if (strcmp(path, "/") == 0) return 0;
    const char *filename = path + 1;
    if (dir_lookup(filename, NULL) < 0) return -ENOENT;
    return 0;
}

int mfs_read( const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi ) {
    (void) fi;
    printf("read: (path=%s, size=%zu, offset=%ld)\n", path, size, (long)offset);

    const char *filename = path + 1;
    int inum = dir_lookup(filename, NULL);
    if (inum < 0) return -ENOENT;

    struct inode ino;
    if (read_inode(inum, &ino) < 0) return -EIO;

    if (offset >= ino.file_size) return 0;
    if (offset + (off_t)size > ino.file_size)
        size = ino.file_size - offset;
    if (size == 0) return 0;
    if (ino.index_block <= 0 || ino.block_count <= 0) return 0;

    // Load index block
    char ibuf[BLOCKSIZE];
    if (read_block(ibuf, ino.index_block) < 0) return -EIO;
    int *ptrs = (int *)ibuf;

    size_t total = 0;
    char dbuf[BLOCKSIZE];
    while (total < size) {
        off_t cur = offset + total;
        int blk_idx = cur / BLOCKSIZE;
        int blk_off = cur % BLOCKSIZE;
        if (blk_idx >= ino.block_count) break;
        int dblk = ptrs[blk_idx];
        if (dblk <= 0) break;
        if (read_block(dbuf, dblk) < 0) return -EIO;
        size_t chunk = BLOCKSIZE - blk_off;
        if (chunk > size - total) chunk = size - total;
        memcpy(buf + total, dbuf + blk_off, chunk);
        total += chunk;
    }
    return total;
}

int mfs_release(const char *path, struct fuse_file_info *fi) {
    (void) fi;
    printf("release: (path=%s)\n", path);
    return 0;
}

int mfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void) mode;
    (void) fi;
    printf("create: (path=%s)\n", path);

    if (strcmp(path, "/") == 0) return -EEXIST;
    const char *filename = path + 1;
    if (!valid_filename(filename)) return -EINVAL;
    if (dir_lookup(filename, NULL) >= 0) return -EEXIST;

    int inum = alloc_inode();
    if (inum < 0) return -ENOSPC;

    struct inode ino;
    memset(&ino, 0, sizeof(ino));
    ino.inode_number = inum;
    ino.file_size = 0;
    ino.block_count = 0;
    ino.index_block = 0;
    ino.status = INODE_VALID_BIT | INODE_PERM_READ | INODE_PERM_WRITE;
    if (write_inode(inum, &ino) < 0) {
        free_inode(inum);
        return -EIO;
    }

    int rc = dir_add(filename, inum);
    if (rc < 0) {
        free_inode(inum);
        return rc;
    }
    return 0;
}

int mfs_mknod(const char *path, mode_t mode, dev_t rdev) {
    (void) rdev;
    return mfs_create(path, mode, NULL);
}

int mfs_unlink(const char *path) {
    printf("unlink: (path=%s)\n", path);
    if (strcmp(path, "/") == 0) return -EISDIR;
    const char *filename = path + 1;

    int entry_idx;
    int inum = dir_lookup(filename, &entry_idx);
    if (inum < 0) return -ENOENT;

    struct inode ino;
    if (read_inode(inum, &ino) < 0) return -EIO;

    free_file_blocks(&ino);

    // Clear the inode
    memset(&ino, 0, sizeof(ino));
    write_inode(inum, &ino);
    free_inode(inum);

    if (dir_remove(filename) < 0) return -EIO;
    return 0;
}

int mfs_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void) fi;
    printf("truncate: (path=%s, size=%ld)\n", path, (long)size);
    if (strcmp(path, "/") == 0) return -EISDIR;
    const char *filename = path + 1;
    int inum = dir_lookup(filename, NULL);
    if (inum < 0) return -ENOENT;

    struct inode ino;
    if (read_inode(inum, &ino) < 0) return -EIO;

    if (size == 0) {
        free_file_blocks(&ino);
        ino.status |= INODE_MODIFIED_BIT;
        return write_inode(inum, &ino) < 0 ? -EIO : 0;
    }

    // Only support shrinking to existing size or truncate-to-0 cleanly.
    if (size == ino.file_size) return 0;
    if (size > ino.file_size) {
        // Extending via truncate is not supported by this MFS (append-only).
        return -EFBIG;
    }
    // Shrink: free trailing data blocks, keep file_size = size.
    int new_blocks = (size + BLOCKSIZE - 1) / BLOCKSIZE;
    if (ino.index_block > 0 && ino.block_count > new_blocks) {
        char ibuf[BLOCKSIZE];
        if (read_block(ibuf, ino.index_block) < 0) return -EIO;
        int *ptrs = (int *)ibuf;
        for (int i = new_blocks; i < ino.block_count; i++) {
            if (ptrs[i] > 0) {
                free_block(ptrs[i]);
                ptrs[i] = 0;
            }
        }
        if (new_blocks == 0) {
            free_block(ino.index_block);
            ino.index_block = 0;
        } else {
            if (write_block(ibuf, ino.index_block) < 0) return -EIO;
        }
        ino.block_count = new_blocks;
    }
    ino.file_size = size;
    ino.status |= INODE_MODIFIED_BIT;
    return write_inode(inum, &ino) < 0 ? -EIO : 0;
}

int mfs_write(const char *path, const char *buf, size_t size, off_t offset,
              struct fuse_file_info *fi) {
    (void) fi;
    printf("write: (path=%s, size=%zu, offset=%ld)\n", path, size, (long)offset);

    const char *filename = path + 1;
    int inum = dir_lookup(filename, NULL);
    if (inum < 0) return -ENOENT;

    struct inode ino;
    if (read_inode(inum, &ino) < 0) return -EIO;

    // MFS supports append-only writes per spec. Accept writes at end of file.
    if (offset != ino.file_size) {
        // For convenience, also accept offset==0 on an empty file (covered by ==file_size).
        return -EINVAL;
    }

    if (size == 0) return 0;

    // Check max file size: 64 MB == MAX_BLOCK_PTRS * BLOCKSIZE
    if (offset + (off_t)size > (off_t)MAX_BLOCK_PTRS * BLOCKSIZE)
        return -EFBIG;

    // Ensure index block exists
    char ibuf[BLOCKSIZE];
    if (ino.index_block <= 0) {
        int ib = alloc_block();
        if (ib < 0) return -ENOSPC;
        memset(ibuf, 0, BLOCKSIZE);
        if (write_block(ibuf, ib) < 0) { free_block(ib); return -EIO; }
        ino.index_block = ib;
    } else {
        if (read_block(ibuf, ino.index_block) < 0) return -EIO;
    }
    int *ptrs = (int *)ibuf;

    size_t written = 0;
    char dbuf[BLOCKSIZE];
    while (written < size) {
        off_t cur = offset + written;
        int blk_idx = cur / BLOCKSIZE;
        int blk_off = cur % BLOCKSIZE;

        if (blk_idx >= MAX_BLOCK_PTRS) break;

        int dblk;
        if (blk_idx >= ino.block_count) {
            // Allocate new data block
            dblk = alloc_block();
            if (dblk < 0) {
                // Out of space: persist what we've done and return.
                if (written == 0) return -ENOSPC;
                break;
            }
            memset(dbuf, 0, BLOCKSIZE);
            ptrs[blk_idx] = dblk;
            ino.block_count = blk_idx + 1;
        } else {
            dblk = ptrs[blk_idx];
            if (dblk <= 0) {
                dblk = alloc_block();
                if (dblk < 0) {
                    if (written == 0) return -ENOSPC;
                    break;
                }
                ptrs[blk_idx] = dblk;
                memset(dbuf, 0, BLOCKSIZE);
            } else {
                if (read_block(dbuf, dblk) < 0) return -EIO;
            }
        }

        size_t chunk = BLOCKSIZE - blk_off;
        if (chunk > size - written) chunk = size - written;
        memcpy(dbuf + blk_off, buf + written, chunk);
        if (write_block(dbuf, dblk) < 0) return -EIO;
        written += chunk;
    }

    // Persist index block
    if (write_block(ibuf, ino.index_block) < 0) return -EIO;

    // Update inode
    if (offset + (off_t)written > ino.file_size)
        ino.file_size = offset + written;
    ino.status |= INODE_MODIFIED_BIT;
    if (write_inode(inum, &ino) < 0) return -EIO;

    return (int)written;
}

int mfs_utimens(const char *path, const struct timespec ts[2],
                struct fuse_file_info *fi) {
    (void) path; (void) ts; (void) fi;
    return 0;
}


int mfs_init() {
    // You don't need to format here, make_mfs already did that!
    // Just verify the disk is open.
    if (fd_disk < 0) {
        printf("Error: Disk file is not open.\n");
        exit(1);
    }
    printf("MFS File system initialized and connected to disk.\n");
    return 0;
}

int main(int argc, char *argv[]) {
    // We expect at least: ./mfs <mountpoint> <diskname>
    if (argc < 3) {
        printf("Usage: %s <mountpoint> <diskfilename>\n", argv[0]);
        return 1;
    }

    // The disk name should be the last argument
    char *diskname = argv[argc - 1];

    // Open the disk file for reading and writing
    fd_disk = open(diskname, O_RDWR);
    if (fd_disk < 0) {
        perror("Failed to open disk file");
        return 1;
    }

    // IMPORTANT: Remove the diskname from argv so fuse_main doesn't get confused
    argv[argc - 1] = NULL;
    argc--;

    mfs_init();

    printf("Starting FUSE...\n");
    fflush(stdout);

    // fuse_main mounts the file system and blocks here, waiting for requests
    int fuse_stat = fuse_main(argc, argv, &mfs_oper, NULL);

    close(fd_disk); // Clean up when we unmount
    return fuse_stat;
}


// Read block k from disk (virtual disk) into buffer block.
// Size of the block is BLOCKSIZE.
// Memory space for block must be allocated outside of this function.
// Block numbers start from 0 in the virtual disk.
int read_block (void *block, int k)
{
    int n;
    int offset;

    offset = k * BLOCKSIZE;
    lseek(fd_disk, (off_t) offset, SEEK_SET);
    n = read (fd_disk, block, BLOCKSIZE);
    if (n != BLOCKSIZE) {
        printf ("read error\n");
        return -1;
    }
    return (0);
}

// write block k into the virtual disk.
int write_block (void *block, int k)
{
    int n;
    int offset;

    offset = k * BLOCKSIZE;
    lseek(fd_disk, (off_t) offset, SEEK_SET);
    n = write (fd_disk, block, BLOCKSIZE);
    if (n != BLOCKSIZE) {
        printf ("write error\n");
        return (-1);
    }
    return 0;
}
