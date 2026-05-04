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
#include <fuse3/fuse.h>



#define BLOCKSIZE 16384 // bytes - 16 KB
#define MAXFILENAME 32  // maximum filename that can be stored by SFS

// Bit layout: [ V | M | R | P2 | P1 | P0 | X | X ]
#define INODE_VALID_BIT      0x80  // 1000 0000 (Top bit)
#define INODE_MODIFIED_BIT   0x40  // 0100 0000 (2nd bit)
#define INODE_REFERENCE_BIT  0x20  // 0010 0000 (3rd bit)
#define INODE_PERM_MASK      0x1C  // 0001 1100 (4th-6th bits for permissions: R, W, X)
#define INODE_PERM_READ      0x10  // 0001 0000 (Read permission)
#define INODE_PERM_WRITE     0x08  // 0000 1000 (Write permission)
#define INODE_PERM_EXECUTE   0x04  // 0000 0100 (Execute permission)


int read_block (int fd_disk, void *block, int k);
int write_block (int fd_disk, void *block, int k);

struct superblock // Resides in Block 0
{
    int num_blocks;     // total number of blocks in the disk
    int block_size;     // 16KB
    int max_files;      // 254 max files (256 - 2 for . and ..)
    int num_inodes;     // 256 total inodes
};

struct direnrty // Resides Block 5
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
int  disk;
char diskname[64];


int mfs_format() {
    char buffer[BLOCKSIZE];
    struct superblock *sb;

    printf("Formatting disk %s with the MFS file system...\n", diskname);

    // 1. Determine total number of blocks on the disk
    // We seek to the end of the file to get its size in bytes, then divide by 16KB.
    off_t file_size = lseek(disk, 0, SEEK_END);
    int num_blocks = file_size / BLOCKSIZE;
    lseek(disk, 0, SEEK_SET); // Reset file pointer to the beginning

    // 2. Initialize and Write Superblock (Block 0)
    bzero(buffer, BLOCKSIZE);
    sb = (struct superblock *)buffer;
    sb->num_blocks = num_blocks;
    sb->block_size = BLOCKSIZE;
    sb->max_files = 254;
    sb->num_inodes = 256;
    write_block(disk, buffer, 0);

    // 3. Initialize and Write Free Space Bitmap (Block 1)
    bzero(buffer, BLOCKSIZE);
    // Blocks 0 through 5 are reserved for MFS metadata (Superblock, bitmaps, inodes, root dir).
    // We mark the first 6 bits as '1' (used). 0x3F in binary is 0011 1111.
    buffer[0] = 0x3F;
    write_block(disk, buffer, 1);

    // 4. Initialize and Write Inode Map (Block 2)
    bzero(buffer, BLOCKSIZE);
    // Inodes are numbered starting at 1, and Inode 1 is the root directory.
    // We'll mark bit 0 (reserved/unused) and bit 1 (root directory) as used. 0x03 is 0000 0011.
    buffer[0] = 0x03; 
    write_block(disk, buffer, 2);

    // 5. Initialize Inode Table (Blocks 3 & 4)
    bzero(buffer, BLOCKSIZE);
    struct inode *inodes = (struct inode *)buffer;
    
    // Set up the Root Directory Inode (Inode 1)
    inodes[1].inode_number = 1;
    inodes[1].file_size = 2 * sizeof(struct direntry); // Holds two entries initially: "." and ".."
    inodes[1].block_count = 1;
    
    // Note: The spec states files use an index block, but explicitly reserves Block 5 for the root directory.
    // So for the root directory ONLY, we can point directly to Block 5. 
    inodes[1].index_block = 5; 
    
    // Set status bits: Valid, Modified, Referenced, and full R/W/X permissions (0x1C)
    inodes[1].status = INODE_VALID_BIT | INODE_MODIFIED_BIT | INODE_REFERENCE_BIT | INODE_PERM_MASK;
    
    write_block(disk, buffer, 3); // Write Block 3 (first 128 inodes)
    
    bzero(buffer, BLOCKSIZE);
    write_block(disk, buffer, 4); // Write Block 4 (remaining 128 inodes, all empty)

    // 6. Initialize Root Directory (Block 5)
    bzero(buffer, BLOCKSIZE);
    struct direntry *dir = (struct direntry *)buffer;
    
    // Entry 0: "." (Current directory)
    strcpy(dir[0].filename, ".");
    dir[0].inode_number = 1;
    
    // Entry 1: ".." (Parent directory, which is also root in this case)
    strcpy(dir[1].filename, "..");
    dir[1].inode_number = 1;
    
    write_block(disk, buffer, 5); // Write Block 5
    
    // Ensure all changes are flushed to the physical Linux file
    fsync(disk);
    
    printf("Disk successfully formatted!\n");
    return 0;
}

int main( int argc, char *argv[] ) {
    
    
    // initialize the  disk
    if (argc != 2) {
        printf ("usage: make_sfs <diskname>\n");
        exit(0);
    }
    
    strcpy (diskname, argv[1]);
    
    disk = open (diskname, O_RDWR);
    printf ("opened disk...\n");
    
    mfs_format();
    
    printf ("closing disk...\n");
    close (disk);
    
	return 0;
}





// read block k from disk (virtual disk) into buffer block.
// size of the block is BLOCKSIZE.
// space for block must be allocated outside of this function.
// block numbers start from 0 in the virtual disk.
int read_block (int fd_disk, void *block, int k)
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
int write_block (int fd_disk, void *block, int k)
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
