#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <fuse3/fuse.h>

#define BLOCKSIZE 16384 // bytes - 16 KB
#define MAXFILENAME 32  // maximum filename that can be stored by mfs

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

int mfs_init();


/* Utility functions */
int read_block (void *block, int k);
int write_block (void *block, int k);

static struct fuse_operations mfs_oper = {
    .getattr    = mfs_getattr,
    .readdir    = mfs_readdir,
    .mknod = NULL,
    
    .mkdir = NULL,
    .unlink = NULL,
    .rmdir = NULL,
    .truncate = NULL,
    .open    = mfs_open,
    .read    = mfs_read,
    .release = mfs_release,
    .write = NULL,
    .rename = NULL,
    .utimens = NULL
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


int mfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    int res = 0;
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

    // Read the root directory (Block 5)
    char block[BLOCKSIZE];
    read_block(block, 5);
    struct direntry *dir = (struct direntry *)block;

    // Search for the filename in the directory entries
    int target_inode_num = -1;
    for (int i = 0; i < 256; i++) {
        // If an entry is used (inode > 0) and the name matches
        if (dir[i].inode_number > 0 && strcmp(dir[i].filename, filename) == 0) {
            target_inode_num = dir[i].inode_number;
            break;
        }
    }

    // If we didn't find the file, return Error NO ENTry (Standard Unix error)
    if (target_inode_num == -1) {
        return -ENOENT;
    }

    // 3. We found the file! Now read its Inode.
    // Inodes 0-127 are in Block 3. Inodes 128-255 are in Block 4.
    int inode_block_num = (target_inode_num < 128) ? 3 : 4;
    read_block(block, inode_block_num);
    struct inode *inodes = (struct inode *)block;

    // Figure out exactly which inode it is inside that block
    int index_in_block = (target_inode_num < 128) ? target_inode_num : (target_inode_num - 128);
    struct inode target_inode = inodes[index_in_block];

    // 4. Populate the stat buffer with the file's metadata
    stbuf->st_mode = S_IFREG | 0666; // It's a regular file (S_IFREG) with rw-rw-rw-
    stbuf->st_nlink = 1;             // Files usually have 1 link
    stbuf->st_size = target_inode.file_size; // Get the size from our on-disk inode!

    return res;
}



int mfs_readdir( const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                struct fuse_file_info *fi, enum fuse_readdir_flags fl) {
    (void) offset;
    (void) fi;
    printf("readdir: (path=%s)\n", path);

    if(strcmp(path, "/") != 0)
        return -ENOENT;

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, "hello ...", NULL, 0, 0);

    return 0;
}

int mfs_open( const char *path, struct fuse_file_info *fi ) {
    printf("open: (path=%s)\n", path);
    return 0;
}

int mfs_read( const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi ) {
    printf("read: (path=%s)\n", path);
    memcpy( buf, "Hello\n", 6 );
    return 6;
}

int mfs_release(const char *path, struct fuse_file_info *fi) {
    printf("release: (path=%s)\n", path);
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
