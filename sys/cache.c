#include "cache.h"
#include <stdlib.h>  
#include <string.h>
#include "heap.h"
#include "ktfs.h"
#include "thread.h"

#define CACHE_SZ 64         //amount of blocks that can be stored in cache

struct block_node {
    struct block_node * next;
    struct ktfs_data_block block;
    unsigned long long idx;
    unsigned long long release;
    void * ptr;
    struct lock lock;
};

struct cache {
    struct io *bkgio;
    //void *blocks;  // Simulated storage blocks
    struct block_node * head;
    unsigned long long last_release;
    unsigned size;
};

// int create_cache(struct io *bkgio, struct cache **cptr)
// parameters:
//                                                            
//              bkgio - The backing I/O endpoint.
//              cptr - Pointer to created cache (output parameter).
// 
//  Description: Creates / initializes a cache with the passed backing interface (bkgio) and makes it available through cptr.
//    
//  Returns:   0 on success, or a negative error code.

int create_cache(struct io *bkgio, struct cache **cptr) {
    if (!cptr) return -1;

    *cptr = (struct cache *)kmalloc(sizeof(struct cache));
    if (!*cptr) return -1;

    (*cptr)->bkgio = bkgio;
    (*cptr)->size = 0;
    (*cptr)->last_release = 0;
    (*cptr)->head = NULL;

    // (*cptr)->blocks = kmalloc(CACHE_BLKSZ);  // Allocate space for a block
    // if (!(*cptr)->blocks) {
    //     kfree(*cptr);
    //     return -1;
    // }

    //memset((*cptr)->blocks, 0, CACHE_BLKSZ);  // Initialize to zero
    return 0;
}

// int cache_get_block(struct cache *cache, unsigned long long pos, void **pptr)
// parameters:
//                                                            
//          cache - The cache from which to fetch the block.
//          pos	- The block position on the backing I/O endpoint.
//          pptr - Pointer to block data (output parameter).
// 
//  Description: Reads a CACHE_BLKSZ sized block from the backing interface into the cache.
//    
//  Returns:   0 on success, or a negative error code.

int cache_get_block(struct cache *cache, unsigned long long pos, void **pptr) {
    if (!cache || !pptr) return -1;
    //*pptr = cache->blocks;  // For now, return a fixed block
    struct block_node * node;
    if(cache->head == NULL){                                 //cache is empty, create new block
        cache->head = kmalloc(sizeof(struct block_node));
        cache->head->next = NULL;
        lock_init(&cache->head->lock);
        node = cache->head;
        cache->size++;
    }
    else{
        node = cache->head;                     //search cache if block is already in it
        struct block_node * LRU_node = node;
        for(unsigned i = 0; (node->next != NULL) && (i < CACHE_SZ); node = node->next){
            if(node->idx == pos){
                lock_acquire(&node->lock);
                *pptr = &node->block;
                node->ptr = *pptr;
                return 0;
            }
            if(node->release < LRU_node->release){
                LRU_node = node;
            }
            i++;
        }
        if(node->idx == pos){
            lock_acquire(&node->lock);
            *pptr = &node->block;
            node->ptr = *pptr;
            return 0;
        }
        if(cache->size == CACHE_SZ){        //if cache full, choose node to evict
            node = LRU_node;
        }
        else{
            node->next = kmalloc(sizeof(struct block_node));    
            cache->size++;
            node = node->next;
            node->next = NULL;
            lock_init(&node->lock);
        }
    }
    ioreadat(cache->bkgio, pos, &node->block, KTFS_BLKSZ);
    node->idx = pos;
    node->release = UINT64_MAX;             //block still in use until release, set to max release time
    *pptr = &node->block;
    node->ptr = *pptr;
    lock_acquire(&node->lock);
    return 0;
}

// void cache_release_block(struct cache *cache, void *pblk, int dirty)
// parameters:
//                                                            
//          
//          cache - The cache containing the block.
//          pblk - A (physical) pointer to the block data returned by cache_get_block().
//          dirty - 1 if the block should be written back to backing device, 0 otherwise.
// 
//  Description: releases lock on block, writes back to cache if block is dirty
//    
//  Returns:   none

void cache_release_block(struct cache *cache, void *pblk, int dirty) {
    // Placeholder: In a real implementation, mark the block as dirty/clean
    struct block_node * node = cache->head;
    for(unsigned i = 0; (node != NULL) && (i < CACHE_SZ); node = node->next){
        if(node->ptr == pblk){
            if(dirty == CACHE_DIRTY){
                iowriteat(cache->bkgio, node->idx, pblk, KTFS_BLKSZ);
                memcpy(node->block.data, pblk, KTFS_BLKSZ);     //save block in cache
            }
            node->release = cache->last_release++;
            lock_release(&node->lock);
        }
        i++;
    }
}

// int cache_flush(struct cache *cache)
// parameters:
//                                                            
//          
//              cache - The cache to write back.
// 
//  Description: flushes the cache - write through cache -- does nothing
//    
//  Returns:   0 on success, or a negative error code

int cache_flush(struct cache *cache) {
    // Placeholder: Would write back dirty blocks to storage -- write through cache: this wont do anything
    return 0;
}

// #include "cache.h"
// #include <stdlib.h>  
// #include <string.h>
// #include "heap.h"
// #include "ktfs.h"


// //transparent cache for unit testing ktfs
// struct cache {
//     struct io *bkgio;
//     void *blocks;  // Simulated storage blocks
// };

// int create_cache(struct io *bkgio, struct cache **cptr) {
//     if (!cptr) return -1;

//     *cptr = (struct cache *)kmalloc(sizeof(struct cache));
//     if (!*cptr) return -1;

//     (*cptr)->bkgio = bkgio;
//     (*cptr)->blocks = kmalloc(CACHE_BLKSZ);  // Allocate space for a block
//     if (!(*cptr)->blocks) {
//         kfree(*cptr);
//         return -1;
//     }

//     memset((*cptr)->blocks, 0, CACHE_BLKSZ);  // Initialize to zero
//     return 0;
// }

// //should only be called on 512B buffers
// int cache_get_block(struct cache *cache, unsigned long long pos, void **pptr) {
//     if (!cache || !pptr) return -1;
//     //*pptr = cache->blocks;  // For now, return a fixed block
//     ioreadat(cache->bkgio, pos, *pptr, KTFS_BLKSZ);
//     return 0;
// }

// void cache_release_block(struct cache *cache, void *pblk, int dirty) {
//     // Placeholder: In a real implementation, mark the block as dirty/clean
// }

// int cache_flush(struct cache *cache) {
//     // Placeholder: Would write back dirty blocks to storage
//     return 0;
// }



