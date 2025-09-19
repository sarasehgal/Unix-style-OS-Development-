// ktfs.c - KTFS implementation
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef KTFS_TRACE
#define TRACE
#endif

#ifdef KTFS_DEBUG
#define DEBUG
#endif

#include "heap.h"
#include "fs.h"
#include "ioimpl.h"
#include "ktfs.h"
#include "error.h"
#include "thread.h"
#include "string.h"
#include "console.h"
#include "cache.h"

// INTERNAL TYPE DEFINITIONS
//

#define DENTRIES_PER_DIR  (KTFS_BLKSZ/KTFS_DENSZ)  // dentries per direct block

#define DENTRIES_PER_IND ((KTFS_BLKSZ/4) * DENTRIES_PER_DIR)               // dentries per indirect block

#define DENTRIES_PER_DIND ((KTFS_BLKSZ/4) * DENTRIES_PER_IND)               // dentries per double indirect block

#define BLOCKS_PER_DIND ((KTFS_BLKSZ/sizeof(uint32_t)) * (KTFS_BLKSZ/sizeof(uint32_t)))     // number of blocks per double indirect block

#define INODES_PER_BLOCK (KTFS_BLKSZ/(sizeof(struct ktfs_inode)))

struct ktfs_file {
    // Fill to fulfill spec
    struct io  io;
    uint32_t size;
    struct ktfs_dir_entry dentry;
    uint32_t flags;
    // uint32_t first_block;
};

// INTERNAL FUNCTION DECLARATIONS
//

int ktfs_mount(struct io * io);

int ktfs_open(const char * name, struct io ** ioptr);
void ktfs_close(struct io* io);
long ktfs_readat(struct io* io, unsigned long long pos, void * buf, long len);
int ktfs_cntl(struct io *io, int cmd, void *arg);

int ktfs_getblksz(struct ktfs_file *fd);
int ktfs_getend(struct ktfs_file *fd, void *arg);

long ktfs_writeat(struct io * io, unsigned long long pos, const void * buf, long len );    

int ktfs_flush(void);

int set_file_size(struct io * io, const unsigned long long * arg);


int ktfs_create	(const char * name);
int ktfs_delete	(const char * name);

uint32_t find_available_block();
int clear_data_block(uint32_t b);

// FUNCTION ALIASES
//

int fsmount(struct io * io)
    __attribute__ ((alias("ktfs_mount")));

int fsopen(const char * name, struct io ** ioptr)
    __attribute__ ((alias("ktfs_open")));

int fsflush(void)
    __attribute__ ((alias("ktfs_flush")));

int fscreate(const char * name)
    __attribute__ ((alias("ktfs_create")));

int fsdelete(const char * name)
    __attribute__ ((alias("ktfs_delete")));

// EXPORTED FUNCTION DEFINITIONS
//

struct cache *c;

struct open_files{
    struct ktfs_file *f;
    struct open_files * next;
};

struct open_files * open_files = NULL;

static struct io * diskio;

// first 512 bytes
static struct ktfs_superblock superblock;

// static struct ktfs_inode * inode_block;

static struct ktfs_inode root_directory_inode;

// static struct ktfs_dir_entry * dentry_datablock;

// int ktfs_mount(struct io * io)
// parameters:
//                      
//              
//              io -io object from the backing io block device.
// 
//  Description: Configures file system with provided io
//    
//  Returns: 0 on success. negative values corresponding to the error type on error.

int ktfs_mount(struct io * io)
{
    struct ktfs_data_block blockbuf;
    int ret;

    diskio = ioaddref(io);
    kprintf("ktfs_mount: Added ref to diskio, diskio=%p\n", diskio);

    // Read superblock data
    ret = ioreadat(io, 0, &blockbuf, KTFS_BLKSZ);
    if(ret != KTFS_BLKSZ){
        kprintf("ktfs_mount: Error reading superblock, ret=%d\n", ret);
        return -EMFILE;
    }
    memcpy(&superblock, &blockbuf, sizeof(superblock));
    kprintf("ktfs_mount: Superblock read. bitmap_block_count=%u, inode_block_count=%u, root_directory_inode=%u\n",
            superblock.bitmap_block_count, superblock.inode_block_count, superblock.root_directory_inode);

    // Read the inode block that contains the root directory inode.
    uint32_t inode_blk_offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count +
                                (superblock.root_directory_inode / (KTFS_BLKSZ / sizeof(struct ktfs_inode))));
    ret = ioreadat(io, inode_blk_offset, &blockbuf, KTFS_BLKSZ);
    if(ret != KTFS_BLKSZ){
        kprintf("ktfs_mount: Error reading inode block at offset %u, ret=%d\n", inode_blk_offset, ret);
        return -EMFILE;
    }
    // Copy the root directory inode from the correct offset within blockbuf.
    uint32_t inode_offset = superblock.root_directory_inode % (KTFS_BLKSZ / sizeof(struct ktfs_inode));
    memcpy(&root_directory_inode, ((char *)&blockbuf) + inode_offset * sizeof(struct ktfs_inode),
           sizeof(struct ktfs_inode));
    kprintf("ktfs_mount: Root directory inode read. size=%u, first direct block=%u\n",
            root_directory_inode.size, root_directory_inode.block[0]);

    if(create_cache(diskio, &c)){
        kprintf("ktfs_mount: create_cache failed\n");
        return -1;
    }

    kprintf("ktfs_mount: Completed successfully.\n");
    return 0;
}

static const struct iointf intf = {     // interface for new io in open
    .close = &ktfs_close,
    .readat = &ktfs_readat,
    .writeat = &ktfs_writeat,
    .cntl = &ktfs_cntl
};

// int ktfs_open(const char * name, struct io ** ioptr)
// parameters:
//                                   
//              name - The name of the file to be opened.
//              ioptr - double pointer to the io object that will be modified to point to the file io object created
// 
//  Description: Opens a file by name
//    
//  Returns:  0 on success, negative values on error.

int ktfs_open(const char * name, struct io ** ioptr)
{
    //kprintf("ktfs_open: Opening file '%s'\n", name);
    if (name == NULL || *name == '\0')
        return -ENOENT; // file not found

    struct open_files * check = open_files;
    while(check != NULL){
        if(strncmp(check->f->dentry.name, name, KTFS_MAX_FILENAME_LEN) == 0){    // file already opened
            //kprintf("ktfs_open: File '%s' already open\n", name);
            return -EMFILE;
        }
        check = check->next;
    }

    struct ktfs_file * target;
    uint32_t dentries = root_directory_inode.size / (sizeof(struct ktfs_dir_entry));
    //kprintf("ktfs_open: Scanning %u directory entries\n", dentries);

    struct ktfs_dir_entry curr;
    struct ktfs_data_block d;
    struct ktfs_data_block * dir = &d;
    uint32_t dind_idx;
    uint32_t idx;
    struct ktfs_data_block ib;
    struct ktfs_data_block * inode_block = &ib;
    struct ktfs_inode in;

    for(uint32_t i = 0; i < dentries; i++){
        if(i < (KTFS_NUM_DIRECT_DATA_BLOCKS * (DENTRIES_PER_DIR))){  // Direct blocks
            uint32_t direct_blk = root_directory_inode.block[i / DENTRIES_PER_DIR];
            uint32_t offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count + direct_blk);
           // kprintf("ktfs_open: [Direct] i=%u, reading block at offset %u\n", i, offset);
            cache_get_block(c, offset, (void**)&dir);
            memcpy(&curr, dir->data + sizeof(curr) * (i % DENTRIES_PER_DIR), sizeof(struct ktfs_dir_entry));
            cache_release_block(c, dir, CACHE_CLEAN);
        }
        else if(i < ((DENTRIES_PER_IND) + (KTFS_NUM_DIRECT_DATA_BLOCKS * (DENTRIES_PER_DIR)))){  // Indirect blocks
            uint32_t offset = 512ULL * (1 + superblock.bitmap_block_count + superblock.inode_block_count + root_directory_inode.indirect);
           // kprintf("ktfs_open: [Indirect] i=%u, reading indirect block at offset %u\n", i, offset);
            cache_get_block(c, offset, (void**)&dir);
            memcpy(&idx, dir->data + sizeof(idx) * ((i / DENTRIES_PER_DIR) - KTFS_NUM_DIRECT_DATA_BLOCKS), sizeof(idx));
            cache_release_block(c, dir, CACHE_CLEAN);
            offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count + idx);
           // kprintf("ktfs_open: [Indirect] reading direct block at offset %u for entry %u\n", offset, i);
            cache_get_block(c, offset, (void**)&dir);
            memcpy(&curr, dir->data + sizeof(curr) * (i % DENTRIES_PER_DIR), sizeof(struct ktfs_dir_entry));
        }
        else {  // Double indirect blocks
            uint32_t i_dind = i - (KTFS_NUM_DIRECT_DATA_BLOCKS * DENTRIES_PER_DIR - DENTRIES_PER_IND);
            //kprintf("ktfs_open: [Double Indirect] i=%u, i_dind=%u\n", i, i_dind);
            uint32_t offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count +
                                 root_directory_inode.dindirect[i_dind / DENTRIES_PER_DIND]);
           // kprintf("ktfs_open: [Double Indirect] reading first indirect block at offset %u\n", offset);
            cache_get_block(c, offset, (void**)(&dir));
            memcpy(&dind_idx, dir->data + sizeof(dind_idx) * (i_dind % DENTRIES_PER_IND), sizeof(dind_idx));
            cache_release_block(c, dir, CACHE_CLEAN);
            offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count + dind_idx);
           // kprintf("ktfs_open: [Double Indirect] reading second indirect block at offset %u\n", offset);
            cache_get_block(c, offset, (void**)(&dir));
            memcpy(&idx, dir->data + sizeof(idx) * (((i_dind % DENTRIES_PER_IND) / DENTRIES_PER_DIR) - KTFS_NUM_DIRECT_DATA_BLOCKS), sizeof(idx));
            cache_release_block(c, dir, CACHE_CLEAN);
            offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count + idx);
            //kprintf("ktfs_open: [Double Indirect] reading direct block at offset %u\n", offset);
            cache_get_block(c, offset, (void**)&dir);
            memcpy(&curr, dir->data + sizeof(curr) * (i_dind % DENTRIES_PER_DIR), sizeof(struct ktfs_dir_entry));
            cache_release_block(c, dir, CACHE_CLEAN);
        }
       // kprintf("ktfs_open: dentry[%u]: inode=%u, name='%s'\n", i, curr.inode, curr.name);
        if(strncmp(name, curr.name, KTFS_MAX_FILENAME_LEN) == 0){
           // kprintf("ktfs_open: Found file '%s' at dentry[%u]\n", name, i);
            target = kmalloc(sizeof(struct ktfs_file));
            target->flags = KTFS_FILE_IN_USE;
            target->dentry = curr;

            uint32_t offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count +
                       curr.inode/(KTFS_BLKSZ/sizeof(struct ktfs_inode)));
            //kprintf("ktfs_open: Reading inode for file '%s' at offset %u\n", name, offset);
            cache_get_block(c, offset, (void **)&inode_block);
            memcpy(&in, inode_block->data + (sizeof(in) * (curr.inode % (KTFS_BLKSZ/sizeof(struct ktfs_inode)))), sizeof(in)); 
            target->size = in.size;   
            cache_release_block(c, inode_block, CACHE_CLEAN);

            *ioptr = ioinit0(&(target->io), &intf);
            *ioptr = create_seekable_io(*ioptr);

            struct open_files* o = kmalloc(sizeof(struct open_files));
            o->f = target;
            o->next = open_files;
            open_files = o;
            return 0;
        }
    }
    //kprintf("ktfs_open: File '%s' not found after scanning %u entries\n", name, dentries);
    return -EMFILE;  
}

// void ktfs_close(struct io* io)
// parameters:
//                                   
//              io	-  io object of the file to be closed
// 
//  Description: Mark the ktfs_file associated with ioptr as not in use.
//    
//  Returns:  0 on success, negative values on error.

void ktfs_close(struct io* io)
{
    struct ktfs_file * target = (void*)io - offsetof(struct ktfs_file, io);
    //kprintf("ktfs_close: Closing file '%s'\n", target->dentry.name);
    target->flags = KTFS_FILE_FREE;
    struct open_files * list = open_files;
    struct open_files * tmp;
    if(open_files->next == NULL){       // only one file
        tmp = open_files;
    }
    else if(open_files->f == target){   //at head of list
        tmp = open_files;
    }
    else{
        while((list->next->f) != target){
            list = list->next;
        }
        tmp = list->next;
        list->next = list->next->next;
    }
    if(open_files == tmp){
        open_files = tmp->next;
    }
    kfree(tmp);
    kfree(target);
    io = NULL;
}

// long ktfs_readat(struct io* io, unsigned long long pos, void * buf, long len)
// parameters:
//                                               
//          io	- This is the io object of the file that you want to read from.
//          pos	- This is the position in the file that you want to start reading from.
//          buf	- This is the buffer that will be filled with the data read from the file.
//          len	- This is the number of bytes to read from the file.
// 
//  Description: Read len bytes starting at pos from the file associated with io.
//    
//  Returns:  long that indicates the number of bytes read or a negative value if there's an error.

long ktfs_readat(struct io* io, unsigned long long pos, void * buf, long len)
{
    kprintf("reading from file \n");
    struct ktfs_file * file;
    struct open_files * list = open_files;
    while(list != NULL){
        if(&(list->f->io) == io){
            file = list->f;
            break;
        }
        list = list->next;
    }
    if(list == NULL){
        //kprintf("ktfs_readat: File not open\n");
        return -EMFILE;        // file not open
    }

    unsigned int blkno; // block in file containing pos
    unsigned int blkoff; // offset of pos inside block
    size_t cpycnt; // number of bytes to copy
    long numblks;       // number of blocks required for data
    uint32_t blockidx;  // data block index
    uint32_t ind_blockidx;
    struct ktfs_data_block inb;
    struct ktfs_data_block * inode_block = &inb;
    struct ktfs_data_block b;
    struct ktfs_data_block * blkbuf = &b;
    struct ktfs_inode in;

    if(file->size < pos) {
       // kprintf("ktfs_readat: pos %llu beyond file size %u\n", pos, file->size);
        return -EINVAL;
    }

    if (len < 0)
        return -EINVAL;
    if (len == 0)
        return 0;
    
    if(len + pos > file->size){
        len = file->size - pos;
        //kprintf("ktfs_readat: Adjusted len to %ld based on file size\n", len);
    }
        
    blkno = pos / KTFS_BLKSZ;
    blkoff = pos % KTFS_BLKSZ;

    numblks = len / KTFS_BLKSZ;
    
    if(blkoff != 0 || len % KTFS_BLKSZ != 0){ 
        numblks += 1;
    }

    if(len % KTFS_BLKSZ != 0){
        if(((len % KTFS_BLKSZ) + blkoff) > KTFS_BLKSZ){     //if pushed into next block
            numblks +=1;
        }
    }


    //kprintf("ktfs_readat: pos=%llu, len=%ld, blkno=%u, numblks=%ld, blkoff=%u\n", pos, len, blkno, numblks, blkoff);

    cache_get_block(c,
        KTFS_BLKSZ*(1 + superblock.bitmap_block_count + file->dentry.inode / (KTFS_BLKSZ/sizeof(struct ktfs_inode))),
        (void **)&inode_block
    );
    memcpy(&in, inode_block->data  + (sizeof(in) * (file->dentry.inode % (KTFS_BLKSZ/sizeof(struct ktfs_inode)))), sizeof(in));
    cache_release_block(c, inode_block, CACHE_CLEAN);

    size_t bytes = 0;
    for(long i = 0; i < numblks; i++){
        if(i + blkno < KTFS_NUM_DIRECT_DATA_BLOCKS){   // direct blocks
            blockidx = in.block[i + blkno];
           // kprintf("ktfs_readat: [Direct] Block %ld, blockidx=%u\n", i, blockidx);
        }
        else if((i + blkno) < (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_BLKSZ/sizeof(uint32_t)))){ // indirect blocks
            cache_get_block(c,
                KTFS_BLKSZ *(1 + superblock.bitmap_block_count + superblock.inode_block_count + in.indirect),       
                (void**)&blkbuf);
            memcpy(&blockidx, blkbuf->data + (sizeof(uint32_t) * (i + blkno - KTFS_NUM_DIRECT_DATA_BLOCKS)), sizeof(blockidx));
            cache_release_block(c, blkbuf, CACHE_CLEAN);
            //kprintf("ktfs_readat: [Indirect] Block %ld, blockidx=%u\n", i, blockidx);
        }
        else { // double indirect blocks
            uint32_t idx_dind = ((i + blkno) - (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_BLKSZ/sizeof(uint32_t))));
            cache_get_block(c,
                KTFS_BLKSZ *(1 + superblock.bitmap_block_count + superblock.inode_block_count + in.dindirect[idx_dind / BLOCKS_PER_DIND]),
                (void**)&blkbuf);
            memcpy(&ind_blockidx, blkbuf->data + (sizeof(uint32_t) * ((idx_dind % BLOCKS_PER_DIND) / (KTFS_BLKSZ/sizeof(uint32_t)))), sizeof(uint32_t));
            cache_release_block(c, blkbuf, CACHE_CLEAN);
            cache_get_block(c,
                KTFS_BLKSZ *(1 + superblock.bitmap_block_count + superblock.inode_block_count + ind_blockidx),
                (void**)&blkbuf);
            memcpy(&blockidx, blkbuf->data + sizeof(blockidx) * (idx_dind % (KTFS_BLKSZ/sizeof(uint32_t))), sizeof(blockidx));
            cache_release_block(c, blkbuf, CACHE_CLEAN);
           // kprintf("ktfs_readat: [Double Indirect] Block %ld, blockidx=%u\n", i, blockidx);
        }
        cache_get_block(c, KTFS_BLKSZ *(1 + superblock.bitmap_block_count + superblock.inode_block_count + blockidx), (void **)&blkbuf);
        if(numblks == 1){
            cpycnt = len;
        }
        else{
            if(i == 0){
                blkoff = pos % KTFS_BLKSZ;
                cpycnt = KTFS_BLKSZ - blkoff;
            }
            else if(i == numblks-1){
                blkoff = 0;
                cpycnt = len - bytes;
            }
            else{
                blkoff = 0;
                cpycnt = KTFS_BLKSZ;
            }
        }
       // kprintf("ktfs_readat: Block %ld, blkoff=%u, cpycnt=%zu\n", i, blkoff, cpycnt);
        memcpy((char *)buf + bytes, blkbuf->data + blkoff, cpycnt);
        bytes += cpycnt;
        cache_release_block(c, blkbuf, CACHE_CLEAN);
    }   

   // kprintf("ktfs_readat: Completed, total bytes copied = %zu\n", bytes);
    return len;
}

// int ktfs_cntl(struct io *io, int cmd, void *arg)
// parameters:
//                                               
//              io - This is the io object of the file that you want to perform the control function.
//              cmd	 - This is the command that you want to execute.
//              arg	- This is the argument that you want to pass in, maybe different for different control functions.
// 
//  Description: Do an special / ioctl function on the filesystem, determined by cmd
//    
//  Returns:  int that indicates success or failure, or something else dependent on cmd

int ktfs_cntl(struct io *io, int cmd, void *arg)
{
    switch (cmd) {
        case IOCTL_GETBLKSZ: 
            return 1; // block size
        case IOCTL_GETEND: {
            struct ktfs_file * file = (void*)io - offsetof(struct ktfs_file, io);
            *(uint32_t *)arg = file->size;
            return 0;
        }
        case IOCTL_SETEND:
            return set_file_size(io, arg);

        default: 
            return -ENOTSUP;
    }
}

// int ktfs_flush(void)
// parameters:
//                                               
//          none
// 
//  Description: Flush the cache to the backing device.
//    
//  0 if flush successful, negative values if there's an error.

int ktfs_flush(void)
{
    int ret = cache_flush(c);
    kprintf("ktfs_flush: cache_flush returned %d\n", ret);
    return ret;
}

// long ktfs_writeat(struct io * io, unsigned long long pos, const void * buf, long len )
// parameters:
//                                               
//          io - This is the io object of the file that you want to write to.
//          pos	- This is the position in the file that you want to start writing at.
//          buf	- This is the buffer that is filled with the data you should write to the file.
//          len	- This is the number of bytes to write from the buffer into the file.
// 
//  Description: Write len bytes starting at pos to the file associated with io
//    
//  Returns:  long that indicates the number of bytes written or a negative value if there's an error.

long ktfs_writeat(struct io * io, unsigned long long pos, const void * buf, long len ){
    kprintf("writing to file \n");
    struct ktfs_file * file;
    struct open_files * list = open_files;
    while(list != NULL){
        if(&(list->f->io) == io){
            file = list->f;
            break;
        }
        list = list->next;
    }
    if(list == NULL){
        kprintf("ktfs_readat: File not open\n");
        return -EMFILE;        // file not open
    }

    unsigned int blkno; // block in file containing pos
    unsigned int blkoff; // offset of pos inside block
    size_t cpycnt; // number of bytes to copy
    long numblks;       // number of blocks required for data
    uint32_t blockidx;  // data block index
    uint32_t ind_blockidx;
    struct ktfs_data_block inb;
    struct ktfs_data_block * inode_block = &inb;
    struct ktfs_data_block b;
    struct ktfs_data_block * blkbuf = &b;
    struct ktfs_inode in;

    if(file->size < pos) {
        //kprintf("ktfs_readat: pos %llu beyond file size %u\n", pos, file->size);
        return -EINVAL;
    }

    if (len < 0)
        return -EINVAL;
    if (len == 0)
        return 0;
    
    if(len + pos > file->size){
        len = file->size - pos;
        //kprintf("ktfs_readat: Adjusted len to %ld based on file size\n", len);
    }
        
    blkno = pos / KTFS_BLKSZ;
    blkoff = pos % KTFS_BLKSZ;

    numblks = len / KTFS_BLKSZ;
    // if(len % KTFS_BLKSZ != 0){ 
    //     numblks += 1;
    // }
    // if(blkoff != 0){ 
    //     numblks += 1;
    // }
    if(blkoff != 0 || len % KTFS_BLKSZ != 0){
        numblks += 1;
    }
    if(len % KTFS_BLKSZ != 0){
        if(((len % KTFS_BLKSZ) + blkoff) > KTFS_BLKSZ){     //if pushed into next block
            numblks +=1;
        }
    }
    kprintf("ktfs_writeat: pos=%llu, len=%ld, blkno=%u, numblks=%ld, blkoff=%u\n", pos, len, blkno, numblks, blkoff);

    cache_get_block(c,
        KTFS_BLKSZ*(1 + superblock.bitmap_block_count + file->dentry.inode / (KTFS_BLKSZ/sizeof(struct ktfs_inode))),
        (void **)&inode_block
    );
    memcpy(&in, inode_block->data  + (sizeof(in) * (file->dentry.inode % (KTFS_BLKSZ/sizeof(struct ktfs_inode)))), sizeof(in));
    cache_release_block(c, inode_block, CACHE_CLEAN);

    size_t bytes = 0;
    for(long i = 0; i < numblks; i++){
        if(i + blkno < KTFS_NUM_DIRECT_DATA_BLOCKS){   // direct blocks
            blockidx = in.block[i + blkno];
            kprintf("ktfs_writeat: [Direct] Block %ld, blockidx=%u\n", i, blockidx);
        }
        else if((i + blkno) < (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_BLKSZ/sizeof(uint32_t)))){ // indirect blocks
            cache_get_block(c,
                KTFS_BLKSZ *(1 + superblock.bitmap_block_count + superblock.inode_block_count + in.indirect),       
                (void**)&blkbuf);
            memcpy(&blockidx, blkbuf->data + (sizeof(uint32_t) * (i + blkno - KTFS_NUM_DIRECT_DATA_BLOCKS)), sizeof(blockidx));
            cache_release_block(c, blkbuf, CACHE_CLEAN);
            kprintf("ktfs_writeat: [Indirect] Block %ld, blockidx=%u\n", i, blockidx);
        }
        else { // double indirect blocks
            uint32_t idx_dind = ((i + blkno) - (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_BLKSZ/sizeof(uint32_t))));
            cache_get_block(c,
                KTFS_BLKSZ *(1 + superblock.bitmap_block_count + superblock.inode_block_count + in.dindirect[idx_dind / BLOCKS_PER_DIND]),
                (void**)&blkbuf);
            memcpy(&ind_blockidx, blkbuf->data + (sizeof(uint32_t) * ((idx_dind%BLOCKS_PER_DIND) / (KTFS_BLKSZ/sizeof(uint32_t)))), sizeof(uint32_t));
            cache_release_block(c, blkbuf, CACHE_CLEAN);
            cache_get_block(c,
                KTFS_BLKSZ *(1 + superblock.bitmap_block_count + superblock.inode_block_count + ind_blockidx),
                (void**)&blkbuf);
            memcpy(&blockidx, blkbuf->data + sizeof(blockidx) * (idx_dind % (KTFS_BLKSZ/sizeof(uint32_t))), sizeof(blockidx));
            cache_release_block(c, blkbuf, CACHE_CLEAN);
            kprintf("ktfs_writeat: [Double Indirect] Block %ld, blockidx=%u\n", i, blockidx);
        }
        cache_get_block(c, KTFS_BLKSZ *(1 + superblock.bitmap_block_count + superblock.inode_block_count + blockidx), (void **)&blkbuf);
        if(numblks == 1){
            cpycnt = len;
        }
        else{
            if(i == 0){
                blkoff = pos % KTFS_BLKSZ;
                cpycnt = KTFS_BLKSZ - blkoff;
            }
            else if(i == numblks-1){
                blkoff = 0;
                cpycnt = len - bytes;
            }
            else{
                blkoff = 0;
                cpycnt = KTFS_BLKSZ;
            }
        }
        kprintf("ktfs_writeat: Block %ld, blkoff=%u, cpycnt=%zu\n", i, blkoff, cpycnt);
        memcpy(blkbuf->data + blkoff,(char *)buf + bytes,cpycnt);
        bytes += cpycnt;
        cache_release_block(c, blkbuf, CACHE_DIRTY);
    }   

    kprintf("ktfs_writeat: Completed, total bytes copied = %zu\n", bytes);
    return len;
}

// int ktfs_create	(const char * name)
// parameters:
//                                               
//          name - a string that is the name of the new file
// 
//  Description: Creates a new file named name of length 0 in the filesystem.
//    
//  Returns:  0 on success, negative value on error

int ktfs_create	(const char * name){

    kprintf("creating new file \n");
    if (name == NULL || *name == '\0')
        return -ENOTSUP; // need valid name

    struct open_files * check = open_files;
    while(check != NULL){
        if(strncmp(check->f->dentry.name, name, KTFS_MAX_FILENAME_LEN) == 0){    // file already exists
            //kprintf("ktfs_open: File '%s' already open\n", name);
            return -EMFILE;
        }
        check = check->next;
    }

    //struct ktfs_file * target;
    uint32_t dentries = root_directory_inode.size / (sizeof(struct ktfs_dir_entry));
    //kprintf("ktfs_open: Scanning %u directory entries\n", dentries);

    struct ktfs_dir_entry curr;
    struct ktfs_data_block d;
    struct ktfs_data_block * dir = &d;
    uint32_t dind_idx;
    uint32_t idx;
   // struct ktfs_data_block ib;
    //struct ktfs_data_block * inode_block = &ib;
    //struct ktfs_inode in;

    uint32_t j;
    uint32_t i;
    for(j = 0; j < (INODES_PER_BLOCK*superblock.inode_block_count); j++){     //check every possible inode
        if(j == superblock.root_directory_inode){       //skip over root directoty inode (usually 0 but not guarenteed)
            continue;
        }
        for(i = 0; i < dentries; i++){         //search dentries, find out if inode already in use
            if(i < (KTFS_NUM_DIRECT_DATA_BLOCKS * (DENTRIES_PER_DIR))){  // Direct blocks
                uint32_t direct_blk = root_directory_inode.block[i / DENTRIES_PER_DIR];
                uint32_t offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count + direct_blk);
            // kprintf("ktfs_open: [Direct] i=%u, reading block at offset %u\n", i, offset);
                cache_get_block(c, offset, (void**)&dir);
                memcpy(&curr, dir->data + sizeof(curr) * (i % DENTRIES_PER_DIR), sizeof(struct ktfs_dir_entry));
                cache_release_block(c, dir, CACHE_CLEAN);
            }
            else if(i < ((DENTRIES_PER_IND) + (KTFS_NUM_DIRECT_DATA_BLOCKS * (DENTRIES_PER_DIR)))){  // Indirect blocks
                uint32_t offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count + root_directory_inode.indirect);
            // kprintf("ktfs_open: [Indirect] i=%u, reading indirect block at offset %u\n", i, offset);
                cache_get_block(c, offset, (void**)&dir);
                memcpy(&idx, dir->data + sizeof(idx) * ((i / DENTRIES_PER_DIR) - KTFS_NUM_DIRECT_DATA_BLOCKS), sizeof(idx));
                cache_release_block(c, dir, CACHE_CLEAN);
                offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count + idx);
            // kprintf("ktfs_open: [Indirect] reading direct block at offset %u for entry %u\n", offset, i);
                cache_get_block(c, offset, (void**)&dir);
                memcpy(&curr, dir->data + sizeof(curr) * (i % DENTRIES_PER_DIR), sizeof(struct ktfs_dir_entry));
            }
            else {  // Double indirect blocks
                uint32_t i_dind = i - (KTFS_NUM_DIRECT_DATA_BLOCKS * DENTRIES_PER_DIR - DENTRIES_PER_IND);
                //kprintf("ktfs_open: [Double Indirect] i=%u, i_dind=%u\n", i, i_dind);
                uint32_t offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count +
                                    root_directory_inode.dindirect[i_dind / DENTRIES_PER_DIND]);
            // kprintf("ktfs_open: [Double Indirect] reading first indirect block at offset %u\n", offset);
                cache_get_block(c, offset, (void**)(&dir));
                memcpy(&dind_idx, dir->data + sizeof(dind_idx) * (i_dind % DENTRIES_PER_IND), sizeof(dind_idx));
                cache_release_block(c, dir, CACHE_CLEAN);
                offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count + dind_idx);
            // kprintf("ktfs_open: [Double Indirect] reading second indirect block at offset %u\n", offset);
                cache_get_block(c, offset, (void**)(&dir));
                memcpy(&idx, dir->data + sizeof(idx) * (((i_dind % DENTRIES_PER_IND) / DENTRIES_PER_DIR) - KTFS_NUM_DIRECT_DATA_BLOCKS), sizeof(idx));
                cache_release_block(c, dir, CACHE_CLEAN);
                offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count + idx);
                //kprintf("ktfs_open: [Double Indirect] reading direct block at offset %u\n", offset);
                cache_get_block(c, offset, (void**)&dir);
                memcpy(&curr, dir->data + sizeof(curr) * (i_dind % DENTRIES_PER_DIR), sizeof(struct ktfs_dir_entry));
                cache_release_block(c, dir, CACHE_CLEAN);
            }
            if(curr.inode == j){    //inode number already in use, check another
                break;
            }
        }
        if(i == dentries){
            break;          //dentry search reached the end, valid inode found
        }
    }
    if(j == (INODES_PER_BLOCK*superblock.inode_block_count)){
        return -ENOINODEBLKS;                 //all inodes in use, return
    }
    //initialize dentry
    struct ktfs_dir_entry new_dentry;
    strncpy(new_dentry.name, name,strlen(name));
    new_dentry.inode = j;    
    //add to root directory inode's data block, update size
    if(dentries < KTFS_NUM_DIRECT_DATA_BLOCKS*DENTRIES_PER_DIR){
        if(dentries%DENTRIES_PER_DIR == 0){                         //find available data block, mark as in use
            uint32_t new_block = find_available_block();
            if(new_block == 0){
                return -ENODATABLKS;
            }
            new_block -= (1 + superblock.bitmap_block_count + superblock.inode_block_count);
            root_directory_inode.block[dentries/DENTRIES_PER_DIR] = new_block;      //update dentry
        }
    }
    else{
        return -EMFILE;             //only max of 96 dentries/files
    }

    //update root directory inode & write to disk
    root_directory_inode.size += sizeof(struct ktfs_dir_entry);
    cache_get_block(c, KTFS_BLKSZ*(1 + superblock.bitmap_block_count + superblock.root_directory_inode/(INODES_PER_BLOCK)), (void**)&dir);
    memcpy(dir->data + sizeof(struct ktfs_inode)*(superblock.root_directory_inode%(INODES_PER_BLOCK)) ,
        &root_directory_inode, sizeof(root_directory_inode));
    cache_release_block(c,dir,CACHE_DIRTY);

    //add dentry to dentry data block at end   -- maybe do before?
    cache_get_block(c, KTFS_BLKSZ*(1 + superblock.bitmap_block_count + superblock.inode_block_count + 
        root_directory_inode.block[dentries/DENTRIES_PER_DIR]), (void**)&dir);
    memcpy(dir->data + sizeof(new_dentry)*(dentries%DENTRIES_PER_DIR), &new_dentry,sizeof(new_dentry));
    cache_release_block(c, dir, CACHE_DIRTY);

    //initialize inode
    struct ktfs_inode new_inode;
    new_inode.size = 0;             //size of zero;
    //new_inode.block[0] = ...      //allocate block now? or do that later?
    cache_get_block(c,KTFS_BLKSZ*(1 + superblock.bitmap_block_count + (j/INODES_PER_BLOCK)), (void**)&dir);         //update inode block
    memcpy(dir->data + sizeof(struct ktfs_inode)*(j%INODES_PER_BLOCK), &new_inode,sizeof(struct ktfs_inode));
    cache_release_block(c,dir,CACHE_DIRTY);
    return 0;
}

// int ktfs_delete	(const char *name)
// parameters:
//                                               
//          name - a string that is the name of the file to delete
// 
//  Description: Deletes a file named name from the filesystem.
//    
//  Returns:  0 on success, negative value on error

int ktfs_delete	(const char *name){

    kprintf("deleting file file \n");
    if (name == NULL || *name == '\0')
    return -ENOENT; // file not found

    struct open_files * check = open_files;
    while(check != NULL){
        if(strncmp(check->f->dentry.name, name, KTFS_MAX_FILENAME_LEN) == 0){    // file already opened, close it
            //kprintf("ktfs_open: File '%s' already open\n", name);
            //return -EMFILE;
            ktfs_close(&check->f->io);
            break;
        }
        check = check->next;
    }

    //struct ktfs_file * target;
    uint32_t dentries = root_directory_inode.size / (sizeof(struct ktfs_dir_entry));
    //kprintf("ktfs_open: Scanning %u directory entries\n", dentries);

    struct ktfs_dir_entry curr;
    struct ktfs_data_block d;
    struct ktfs_data_block * dir = &d;
    uint32_t dind_idx;
    uint32_t idx;
    //struct ktfs_data_block ib;
    //struct ktfs_data_block * inode_block = &ib;
    struct ktfs_inode in;
    uint32_t offset;
    uint32_t blockidx;
    uint32_t ind_blockidx;
    uint32_t dentry_idx;

    for(uint32_t i = 0; i < dentries; i++){
        if(i < (KTFS_NUM_DIRECT_DATA_BLOCKS * (DENTRIES_PER_DIR))){  // Direct blocks -- only these should realistically be checked
            uint32_t direct_blk = root_directory_inode.block[i / DENTRIES_PER_DIR];
            offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count + direct_blk);
           // kprintf("ktfs_open: [Direct] i=%u, reading block at offset %u\n", i, offset);
            cache_get_block(c, offset, (void**)&dir);
            memcpy(&curr, dir->data + sizeof(curr) * (i % DENTRIES_PER_DIR), sizeof(struct ktfs_dir_entry));
            cache_release_block(c, dir, CACHE_CLEAN);
        }
        else if(i < ((DENTRIES_PER_IND) + (KTFS_NUM_DIRECT_DATA_BLOCKS * (DENTRIES_PER_DIR)))){  // Indirect blocks
            offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count + root_directory_inode.indirect);
           // kprintf("ktfs_open: [Indirect] i=%u, reading indirect block at offset %u\n", i, offset);
            cache_get_block(c, offset, (void**)&dir);
            memcpy(&idx, dir->data + sizeof(idx) * ((i / DENTRIES_PER_DIR) - KTFS_NUM_DIRECT_DATA_BLOCKS), sizeof(idx));
            cache_release_block(c, dir, CACHE_CLEAN);
            offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count + idx);
           // kprintf("ktfs_open: [Indirect] reading direct block at offset %u for entry %u\n", offset, i);
            cache_get_block(c, offset, (void**)&dir);
            memcpy(&curr, dir->data + sizeof(curr) * (i % DENTRIES_PER_DIR), sizeof(struct ktfs_dir_entry));
        }
        else {  // Double indirect blocks
            uint32_t i_dind = i - (KTFS_NUM_DIRECT_DATA_BLOCKS * DENTRIES_PER_DIR - DENTRIES_PER_IND);
            //kprintf("ktfs_open: [Double Indirect] i=%u, i_dind=%u\n", i, i_dind);
            offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count +
                                 root_directory_inode.dindirect[i_dind / DENTRIES_PER_DIND]);
           // kprintf("ktfs_open: [Double Indirect] reading first indirect block at offset %u\n", offset);
            cache_get_block(c, offset, (void**)(&dir));
            memcpy(&dind_idx, dir->data + sizeof(dind_idx) * (i_dind % DENTRIES_PER_IND), sizeof(dind_idx));
            cache_release_block(c, dir, CACHE_CLEAN);
            offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count + dind_idx);
           // kprintf("ktfs_open: [Double Indirect] reading second indirect block at offset %u\n", offset);
            cache_get_block(c, offset, (void**)(&dir));
            memcpy(&idx, dir->data + sizeof(idx) * (((i_dind % DENTRIES_PER_IND) / DENTRIES_PER_DIR) - KTFS_NUM_DIRECT_DATA_BLOCKS), sizeof(idx));
            cache_release_block(c, dir, CACHE_CLEAN);
            offset = KTFS_BLKSZ * (1 + superblock.bitmap_block_count + superblock.inode_block_count + idx);
            //kprintf("ktfs_open: [Double Indirect] reading direct block at offset %u\n", offset);
            cache_get_block(c, offset, (void**)&dir);
            memcpy(&curr, dir->data + sizeof(curr) * (i_dind % DENTRIES_PER_DIR), sizeof(struct ktfs_dir_entry));
            cache_release_block(c, dir, CACHE_CLEAN);
        }
       // kprintf("ktfs_open: dentry[%u]: inode=%u, name='%s'\n", i, curr.inode, curr.name);
        if(strncmp(name, curr.name, KTFS_MAX_FILENAME_LEN) == 0){           //file found -> delete it
            //get inode
            dentry_idx = offset;
            offset = KTFS_BLKSZ*(1+superblock.bitmap_block_count + (curr.inode/INODES_PER_BLOCK));
            cache_get_block(c, offset, (void**)&dir);
            memcpy(&in , dir->data + ((sizeof(in)*(curr.inode%INODES_PER_BLOCK))), sizeof(in));
            cache_release_block(c,dir,CACHE_CLEAN);             //don't need to modify inode

            //clear all data blocks associated with file in bitmap - direct, indirect, and double indirect
            unsigned numblks = in.size / KTFS_BLKSZ;        //number of blocks file takes up 
                if(in.size % KTFS_BLKSZ != 0){ 
                    numblks += 1;
                 }
            for(unsigned i = 0; i < numblks; i++){
                if(i < KTFS_NUM_DIRECT_DATA_BLOCKS){            //free direct data blocks
                    blockidx = (1 + superblock.bitmap_block_count + superblock.inode_block_count + in.block[i]);
                }
                else if(i < (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_BLKSZ/sizeof(uint32_t)))){     //indirect data blocks
                    cache_get_block(c,
                        KTFS_BLKSZ *(1 + superblock.bitmap_block_count + superblock.inode_block_count + in.indirect),       
                        (void**)&dir);
                    memcpy(&blockidx, dir->data + (sizeof(blockidx)*(i-KTFS_NUM_DIRECT_DATA_BLOCKS)), sizeof(blockidx));
                    blockidx += 1 + superblock.bitmap_block_count + superblock.inode_block_count;
                    cache_release_block(c,dir,CACHE_CLEAN);
                }
                else{       //doubly indirect data blocks
                    uint32_t i_dind = (i - (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_BLKSZ/sizeof(uint32_t))));
                    offset = KTFS_BLKSZ *(1 + superblock.bitmap_block_count + superblock.inode_block_count + in.dindirect[i_dind/BLOCKS_PER_DIND]);
                    cache_get_block(c, offset, (void**)&dir);
                    memcpy(&ind_blockidx, dir->data + (sizeof(uint32_t) * ((i_dind % BLOCKS_PER_DIND) / (KTFS_BLKSZ/sizeof(uint32_t)))), sizeof(uint32_t));
                    cache_release_block(c,dir,CACHE_CLEAN);

                    offset = KTFS_BLKSZ*(1 + superblock.bitmap_block_count + superblock.inode_block_count + ind_blockidx);
                    cache_get_block(c,offset,(void**)&dir);
                    memcpy(&blockidx, dir->data + sizeof(blockidx) * (i_dind % (KTFS_BLKSZ/sizeof(uint32_t))), sizeof(blockidx));
                    blockidx += 1 + superblock.bitmap_block_count + superblock.inode_block_count;

                    //free this indirect block too if used -- possible concurrency issue? maybe do this later instead
                    if((i_dind % (KTFS_BLKSZ/sizeof(uint32_t))) == 0){
                        if(clear_data_block(1 + superblock.bitmap_block_count + superblock.inode_block_count + ind_blockidx) < 0){
                            return -EACCESS;
                        }
                    }
                }
                if(clear_data_block(blockidx) < 0){
                    return -EACCESS;                //accessed invalid data block
                }
            }
            //free indirect and double indirect data blocks
            if(numblks > KTFS_NUM_DIRECT_DATA_BLOCKS){
                blockidx = 1 + superblock.bitmap_block_count + superblock.inode_block_count + in.indirect;
                if(clear_data_block(blockidx)){
                    return -EACCESS;
                }
            }
            if(numblks > (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_BLKSZ/sizeof(uint32_t)))){
                unsigned num_dind_blocks = ((numblks - (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_BLKSZ/sizeof(uint32_t))))/BLOCKS_PER_DIND);
                if(num_dind_blocks%BLOCKS_PER_DIND != 0){
                    num_dind_blocks++;
                }
                for(unsigned i = 0; i < num_dind_blocks; i++){
                    blockidx = 1 + superblock.bitmap_block_count + superblock.inode_block_count + in.dindirect[i];
                    if(clear_data_block(blockidx)){
                        return -EACCESS;
                    }
                }
            }

            //clear dentry -> replace with last entry & decrease size, clear dentry data block if only dentry present
            //get last dentry
            struct ktfs_dir_entry last_dentry;
            offset = KTFS_BLKSZ*(1 + superblock.bitmap_block_count + superblock.inode_block_count + root_directory_inode.block[(dentries-1)/DENTRIES_PER_DIR]);
            cache_get_block(c,offset,(void**)&dir);
            memcpy(&last_dentry, dir->data + (sizeof(last_dentry)*((dentries-1)%DENTRIES_PER_DIR)), sizeof(last_dentry));
            cache_release_block(c, dir, CACHE_CLEAN);

            //get overwrite current dentry w/ last dentry
            offset = KTFS_BLKSZ*(1 + superblock.bitmap_block_count + superblock.inode_block_count + dentry_idx);
            cache_get_block(c,offset,(void**)&dir);
            memcpy(dir->data + (sizeof(last_dentry)*(i % DENTRIES_PER_DIR)), &last_dentry, sizeof(last_dentry));
            cache_release_block(c, dir, CACHE_DIRTY);
            
            //if last dentry is only dentry in data block, free that block
            if(((dentries-1)%DENTRIES_PER_DIR) == 0){
                blockidx = 1 + superblock.bitmap_block_count + superblock.inode_block_count + root_directory_inode.block[(dentries-1)/DENTRIES_PER_DIR];
                if(clear_data_block(blockidx) < 0){
                    return -EACCESS;
                }
            }
            root_directory_inode.size -= sizeof(struct ktfs_dir_entry);     //update size of root directory inode & write
            offset = KTFS_BLKSZ*(1 + superblock.bitmap_block_count + superblock.root_directory_inode/(INODES_PER_BLOCK));
            cache_get_block(c,offset, (void **)&dir);
            memcpy(dir->data + sizeof(struct ktfs_inode)*(superblock.root_directory_inode%(INODES_PER_BLOCK)), &root_directory_inode, sizeof(root_directory_inode));
            cache_release_block(c, dir, CACHE_DIRTY);

            return 0;
        }
    }
    return -EMFILE;         //file not found
}

// int set_file_size(struct io * io, const unsigned long long * arg)
// parameters:
//                                               
//          io - io associated with the file
//          arg - ptr containing new size of the file
// 
//  Description: Increases the size of input io's file, allocated necessary blocks
//    
//  Returns:  0 on success, negative value on error

int set_file_size(struct io * io, const unsigned long long * arg){

    kprintf("resizing file \n");
    struct ktfs_data_block block;
    struct ktfs_data_block * blockbuf = &block;
    struct ktfs_inode in;
    uint32_t offset;

    struct ktfs_file * file = (void*)io - offsetof(struct ktfs_file, io);

    unsigned long long max_file_size = KTFS_BLKSZ*(KTFS_NUM_DIRECT_DATA_BLOCKS + 
        KTFS_NUM_INDIRECT_BLOCKS*(KTFS_BLKSZ/sizeof(uint32_t))
        + KTFS_NUM_DINDIRECT_BLOCKS*BLOCKS_PER_DIND);

    if(*arg > max_file_size){
        return -EINVAL;
    }

    uint32_t old_numblks = file->size / KTFS_BLKSZ;     //number of blocks old size takes up
    if(file->size % KTFS_BLKSZ != 0){ 
        old_numblks += 1;
    }

    uint32_t new_numblks = *arg/KTFS_BLKSZ;             //number of blocks new size will take up
    if(*arg % KTFS_BLKSZ != 0){ 
        new_numblks += 1;
    }


    uint32_t newblock;
    uint32_t idx_dind;
    uint32_t ind_blk_idx;

    //read from inode - write to later
    offset = KTFS_BLKSZ*(1 + superblock.bitmap_block_count + (file->dentry.inode/INODES_PER_BLOCK));
    cache_get_block(c, offset, (void**)&blockbuf);
    memcpy(&in, blockbuf->data + (sizeof(in)*(file->dentry.inode%INODES_PER_BLOCK)), sizeof(in));
    cache_release_block(c,blockbuf,CACHE_CLEAN);

    //if new blocks required, allocate new data blocks
    for(unsigned i = old_numblks; i < new_numblks; i++){
        if(i < KTFS_NUM_DIRECT_DATA_BLOCKS){                //allocate direct data block
            kprintf("allocating direct data block:\n");
            in.block[i] = find_available_block();
            if(in.block[i] == 0){
                return -EACCESS;
            }
            in.block[i] -= (1 + superblock.bitmap_block_count + superblock.inode_block_count);
        }
        else if(i < (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_BLKSZ/sizeof(uint32_t)))){
            if(i == KTFS_NUM_DIRECT_DATA_BLOCKS){       //allocate new indirect data block
                kprintf("allocating indirect data block:\n");
                in.indirect = find_available_block();
                if(in.indirect == 0){
                    return -EACCESS;
                }
                in.indirect -= (1 + superblock.bitmap_block_count + superblock.inode_block_count);
            }
            offset = KTFS_BLKSZ*(1 + superblock.bitmap_block_count + superblock.inode_block_count + in.indirect);
            kprintf("allocating direct data block:\n");
            newblock = find_available_block();
            if(newblock == 0){
                return -EACCESS;
            }
            newblock -= (1 + superblock.bitmap_block_count + superblock.inode_block_count);
            cache_get_block(c, offset, (void**)&blockbuf);
            memcpy(blockbuf->data + sizeof(newblock)*(i-KTFS_NUM_DIRECT_DATA_BLOCKS), &newblock, sizeof(newblock));
            cache_release_block(c, blockbuf, CACHE_DIRTY);
        }
        else{       //doulbe check this***
            idx_dind = i - (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_BLKSZ/sizeof(uint32_t)));   //block relative to indirect

            if(idx_dind % BLOCKS_PER_DIND == 0){        //allocate new double indirect block
                kprintf("allocating double indirect data block:\n");
                in.dindirect[idx_dind/BLOCKS_PER_DIND] = find_available_block();
                if(in.dindirect[idx_dind/BLOCKS_PER_DIND] == 0){
                    return -EACCESS;
                }
                in.dindirect[idx_dind/BLOCKS_PER_DIND] -= (1 + superblock.bitmap_block_count + superblock.inode_block_count);
            }

            //access double indirect data block
            offset = KTFS_BLKSZ*(1 + superblock.bitmap_block_count + superblock.inode_block_count + in.dindirect[idx_dind/BLOCKS_PER_DIND]);
            if(idx_dind%(KTFS_BLKSZ/sizeof(uint32_t)) == 0){        //if new indirect block needed, allocate
                kprintf("allocating indirect data block:\n");
                newblock = find_available_block();
                if(newblock == 0){
                    return -EACCESS;
                }
                newblock -= (1 + superblock.bitmap_block_count + superblock.inode_block_count);
                cache_get_block(c, offset, (void**)&blockbuf);
                memcpy(blockbuf->data + sizeof(newblock)*((idx_dind%BLOCKS_PER_DIND) / (KTFS_BLKSZ/sizeof(uint32_t))), &newblock, sizeof(newblock));
                cache_release_block(c,blockbuf,CACHE_DIRTY);
            }

            cache_get_block(c, offset, (void**)&blockbuf);
            memcpy(&ind_blk_idx, blockbuf->data + sizeof(idx_dind)*(((idx_dind%BLOCKS_PER_DIND) / (KTFS_BLKSZ/sizeof(uint32_t)))), sizeof(ind_blk_idx));
            cache_release_block(c,blockbuf,CACHE_CLEAN);

            offset = KTFS_BLKSZ*(1 + superblock.bitmap_block_count + superblock.inode_block_count + ind_blk_idx);
            //access indirect data block
            kprintf("allocating direct data block:\n");
            newblock = find_available_block();
            if(newblock == 0){
                return -EACCESS;
            }
            newblock -= (1 + superblock.bitmap_block_count + superblock.inode_block_count);
            cache_get_block(c, offset, (void**)&blockbuf);
            memcpy(blockbuf->data + sizeof(newblock)*(idx_dind%(KTFS_BLKSZ/sizeof(uint32_t))), &newblock, sizeof(newblock));
            cache_release_block(c,blockbuf,CACHE_DIRTY);
        }
    }

    file->size = *arg;
    //update inode structure on disk
    in.size = *arg;
    offset = KTFS_BLKSZ*(1 + superblock.bitmap_block_count + (file->dentry.inode/INODES_PER_BLOCK));
    cache_get_block(c, offset, (void **)&blockbuf);
    memcpy(blockbuf->data + (sizeof(in)*(file->dentry.inode%INODES_PER_BLOCK)), &in, sizeof(in));
    cache_release_block(c,blockbuf,CACHE_DIRTY);

    return 0;
}

// uint32_t find_available_block()
// parameters:
//                                               
//              none
// 
//  Description: Finds data block not used in file system
//    
//  Returns:  0 on failure, block index on success

uint32_t find_available_block(){                //returns block index of available block,
    struct ktfs_bitmap bitmap;
    struct ktfs_bitmap * b = &bitmap;
    for(unsigned i = 0; i < superblock.bitmap_block_count; i++){
        cache_get_block(c, KTFS_BLKSZ*(1 + i), (void**)&b);
        for(unsigned j = 0; j < KTFS_BLKSZ*8; j++){
            if(superblock.block_count < (j + (i*KTFS_BLKSZ*8))){  //beyond block count
                cache_release_block(c,b, CACHE_CLEAN);
                return 0;
            }
            if(((b->bytes[j/8] >> (j%8)) & 1) == 0){    //found block not in use
                b->bytes[j/8] |= (1 << (j%8));          //8 bits per byte
                cache_release_block(c, &bitmap, CACHE_DIRTY);
                uint32_t ret = ((i*KTFS_BLKSZ*8) + j);// - 1 - superblock.bitmap_block_count - superblock.inode_block_count;
                kprintf("allocating data block %d \n",(ret- 1 - superblock.bitmap_block_count - superblock.inode_block_count));
                return ret;
            }
        }
        cache_release_block(c,b, CACHE_CLEAN);
    }
    return 0;               //0 -> superblock -- never available -- could not find available block
}

// uint32_t find_available_block()
// parameters:
//                                               
//              b - data block index to be cleared
// 
//  Description: Clears a data block in the bitmap, making it available for use again
//    
//  Returns:  0 on success, negative on failure

int clear_data_block(uint32_t b){       
    struct ktfs_bitmap bitmap;
    struct ktfs_bitmap * bit = &bitmap;
    if(b < (1 + superblock.bitmap_block_count + superblock.inode_block_count) ){
        return -ENOTSUP;        //trying to free non-data block
    }
    if(superblock.block_count < b){
        return -ENOTSUP;        //block index greater than number of blocks
    }
    
    cache_get_block(c, KTFS_BLKSZ*(1 + (b/(KTFS_BLKSZ*8))), (void **)&bit); //get bitmap block that block is located in
    bit->bytes[b/8] &= ~(1 << (b%8));            //8 bits per byte
    kprintf("clearing block %d \n", b);
    cache_release_block(c,bit,CACHE_DIRTY);

    return 0;
}