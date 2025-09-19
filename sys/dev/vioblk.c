// vioblk.c - VirtIO serial port (console)
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef VIOBLK_TRACE
#define TRACE
#endif

#ifdef VIOBLK_DEBUG
#define DEBUG
#endif

#include "virtio.h"
#include "intr.h"
#include "assert.h"
#include "heap.h"
#include "io.h"
#include "device.h"
#include "thread.h"
#include "../error.h"
#include "string.h"
#include "assert.h"
#include "ioimpl.h"
#include "io.h"
#include "conf.h"
#include "vioblk.h"
#include <limits.h>

// COMPILE-TIME PARAMETERS
//

#ifndef VIOBLK_INTR_PRIO
#define VIOBLK_INTR_PRIO 1
#endif

#ifndef VIOBLK_NAME
#define VIOBLK_NAME "vioblk"
#endif

// INTERNAL CONSTANT DEFINITIONS
//
#define VIOBLK_DESC_COUNT  8  // or 16/32/etc. Must be <= queue_num_max

struct virtio_blk_req {
    uint32_t type;      
    uint32_t reserved;  
    uint64_t sector;    
};

struct vioblk_reqinfo {
    int in_use;                  
    long result;                 
    //struct condition done_cond;  
    uint8_t status;             
};

struct vioblk_device {
    volatile struct virtio_mmio_regs *regs;
    int irqno;
    int instno;

    struct io io;  // The I/O interface we’ll register.

    // Some basic info from the virtio-blk config
    uint64_t capacity;  // total number of sectors
    uint32_t blk_size;  // sector size in bytes (defaults to 512 if not provided)
    
    // Possibly store any driver features you accepted:
    virtio_featset_t features;

    // For concurrency, if you want a lock/cond for I/O requests:
    struct lock lock;  
    struct condition io_done;

    // A simple virtqueue. You can adapt the pattern from viorng but with more descriptors:
    struct {
        uint16_t last_used_idx;

        // The descriptor array:
        struct virtq_desc desc[VIOBLK_DESC_COUNT];

        // The "avail" ring. We wrap it in a union so we can size it properly:
        union {
            struct virtq_avail avail;
            char _avail_filler[VIRTQ_AVAIL_SIZE(VIOBLK_DESC_COUNT)];
        };

        // The "used" ring. Similarly wrapped in a union:
        union {
            volatile struct virtq_used used;
            char _used_filler[VIRTQ_USED_SIZE(VIOBLK_DESC_COUNT)];
        };
    } vq;

    // If you need scratch buffers or request headers, you might store them here:
    // e.g., struct virtio_blk_req reqs[VIOBLK_DESC_COUNT];
    // char data_buffers[VIOBLK_DESC_COUNT][...];
    struct virtio_blk_req reqhdrs[VIOBLK_DESC_COUNT];
    uint8_t status_bytes[VIOBLK_DESC_COUNT];
    struct vioblk_reqinfo requests[VIOBLK_DESC_COUNT];
    int desc_free[VIOBLK_DESC_COUNT];
};



// VirtIO block device feature bits (number, *not* mask)

#define VIRTIO_BLK_F_SIZE_MAX       1
#define VIRTIO_BLK_F_SEG_MAX        2
#define VIRTIO_BLK_F_GEOMETRY       4
#define VIRTIO_BLK_F_RO             5
#define VIRTIO_BLK_F_BLK_SIZE       6
#define VIRTIO_BLK_F_FLUSH          9
#define VIRTIO_BLK_F_TOPOLOGY       10
#define VIRTIO_BLK_F_CONFIG_WCE     11
#define VIRTIO_BLK_F_MQ             12
#define VIRTIO_BLK_F_DISCARD        13
#define VIRTIO_BLK_F_WRITE_ZEROES   14

// INTERNAL FUNCTION DECLARATIONS
//

// static int vioblk_open(struct io ** ioptr, void * aux);
// static void vioblk_close(struct io * io);

// static long vioblk_readat (
//     struct io * io,
//     unsigned long long pos,
//     void * buf,
//     long bufsz);

// static long vioblk_writeat (
//     struct io * io,
//     unsigned long long pos,
//     const void * buf,
//     long len);

// static int vioblk_cntl (
//     struct io * io, int cmd, void * arg);

// static void vioblk_isr(int srcno, void * aux);

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO block device. Declared and called directly from virtio.c.

static struct vioblk_device global_vioblk;

void vioblk_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    if (!regs || regs->device_id != VIRTIO_ID_BLOCK) {
        return;
    }

    //struct vioblk_device *dev = kmalloc(sizeof(*dev));
    struct vioblk_device *dev = &global_vioblk;
    if (!dev) return;
    memset(dev, 0, sizeof(*dev));
    dev->regs = regs;
    dev->irqno = irqno;
    lock_init(&dev->lock);
    condition_init(&dev->io_done, "vioblk_io_done");

    for (int i = 0; i < VIOBLK_DESC_COUNT; i++) {
        dev->requests[i].in_use = 0;
        dev->requests[i].status = 0xFF;
        dev->requests[i].result = 0;
        dev->desc_free[i] = 1;
    }
    regs->status |= (VIRTIO_STAT_ACKNOWLEDGE | VIRTIO_STAT_DRIVER);
    __sync_synchronize();

    // Negotiate features. We need:
    //  - VIRTIO_F_RING_RESET and
    //  - VIRTIO_F_INDIRECT_DESC
    // We want:
    //  - VIRTIO_BLK_F_BLK_SIZE and
    //  - VIRTIO_BLK_F_TOPOLOGY.
    virtio_featset_t needed_features = {0}, wanted_features = {0}, enabled_features = {0};
    int result;
    unsigned long blksz;

    virtio_featset_init(needed_features);
    virtio_featset_add(needed_features, VIRTIO_F_RING_RESET);
    virtio_featset_add(needed_features, VIRTIO_F_INDIRECT_DESC);
    virtio_featset_init(wanted_features);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_TOPOLOGY);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        kfree(dev);
        return;
    }
    memcpy(dev->features, enabled_features, sizeof(dev->features));

    regs->status |= VIRTIO_STAT_FEATURES_OK;
    __sync_synchronize();
    if (!(regs->status & VIRTIO_STAT_FEATURES_OK)) {
        kprintf("vioblk: device didn't set FEATURES_OK\n");
        //kfree(dev);
        return;
    }
        
    // If the device provides a block size, use it. Otherwise, use 512.

    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;

    // blksz must be a power of two
    assert (((blksz - 1) & blksz) == 0);

    dev->blk_size = blksz;
    dev->capacity = regs->config.blk.capacity;

    regs->queue_sel = 0;
    uint32_t max = regs->queue_num_max;
    if (max < VIOBLK_DESC_COUNT) {
        kprintf("%x, max bigger than %x", max, VIOBLK_DESC_COUNT);
        //kfree(dev);
        return;
    }
    regs->queue_num = VIOBLK_DESC_COUNT;

    for (int i = 0; i < VIOBLK_DESC_COUNT; i++) {
        dev->vq.desc[i].addr  = 0;
        dev->vq.desc[i].len   = 0;
        dev->vq.desc[i].flags = 0;
        dev->vq.desc[i].next  = 0;
    }
    dev->vq.avail.flags = 0;
    dev->vq.avail.idx   = 0;
    for (int i = 0; i < VIOBLK_DESC_COUNT; i++) {
        dev->vq.avail.ring[i] = 0;
    }
    dev->vq.used.flags = 0;
    dev->vq.used.idx   = 0;
    for (int i = 0; i < VIOBLK_DESC_COUNT; i++) {
        dev->vq.used.ring[i].id  = 0;
        dev->vq.used.ring[i].len = 0;
    }
    dev->vq.last_used_idx = 0;

    // Attach the rings to the device
    virtio_attach_virtq(
        regs,
        0, // queue index
        VIOBLK_DESC_COUNT,
        (uintptr_t)dev->vq.desc,
        (uintptr_t)&dev->vq.used,
        (uintptr_t)&dev->vq.avail
    );
    virtio_enable_virtq(regs, 0);

    // 8) Enable interrupt
    enable_intr_source(irqno, VIOBLK_INTR_PRIO, vioblk_isr, dev);

    // 9) Register device 
    dev->instno = register_device("vioblk", vioblk_open, dev);
    if (dev->instno < 0) {
        disable_intr_source(irqno);
        //kfree(dev);
        return;
    }

    // 10) DRIVER_OK
    regs->status |= VIRTIO_STAT_DRIVER_OK;
    __sync_synchronize();
    // FIX ME
}

static const struct iointf vioblk_iointf = {
    .readat = &vioblk_readat,
    .writeat = &vioblk_writeat,
    .close = &vioblk_close,
    .cntl = &vioblk_cntl
};

int vioblk_open(struct io ** ioptr, void * aux) {
    struct vioblk_device *dev = aux;
    if (!dev) return -ENODEV;
    ioinit0(&dev->io, &vioblk_iointf);
    *ioptr = ioaddref(&dev->io);
    return 0;
}

void vioblk_close(struct io * io) {
    struct vioblk_device * const dev =
        (void*)io - offsetof(struct vioblk_device, io);
    if (!dev) return;
    disable_intr_source(dev->irqno);
    virtio_reset_virtq(dev->regs, 0);
    // kfree(dev);
}

void vioblk_isr(int srcno, void * aux) {
    struct vioblk_device * const dev = (struct vioblk_device *)aux;
    if(!dev) return;

    //lock_acquire(&dev->lock);

    // Check if the interrupt is for this device   
    uint32_t status = dev->regs->interrupt_status;
    if(!status) {
        lock_release(&dev->lock);
        return; // No interrupt for this device
    }

    trace("vioblk_isr - irqno=%d status=0x%x", srcno, status);

    while(dev->vq.used.idx != dev->vq.last_used_idx) {
        uint16_t used_pos = dev->vq.last_used_idx % VIOBLK_DESC_COUNT;
        struct virtq_used_elem used_elem = dev->vq.used.ring[used_pos];

        uint16_t desc_idx = used_elem.id;
        uint16_t len = used_elem.len;
        // struct virtq_desc *desc = &dev->vq.desc[desc_idx];

        dev->requests[desc_idx].in_use = 0;
        dev->requests[desc_idx].result = len;
        dev->requests[desc_idx].status = dev->status_bytes[desc_idx];

        uint16_t d = desc_idx;

        while (1){
            dev->desc_free[d] = 1;
            if (dev->vq.desc[d].flags & VIRTQ_DESC_F_NEXT) {
                d = dev->vq.desc[d].next;
            } else {
                break;
            }
        }
        trace("Processed request: desc_idx=%d len=%d", desc_idx, len);
        dev->vq.last_used_idx++;
    }

    condition_broadcast(&dev->io_done); // Notify any waiting threads
    dev->regs->interrupt_ack = status; // Acknowledge the interrupt
    __sync_synchronize(); // Ensure memory operations are completed
    //lock_release(&dev->lock);
}

int vioblk_cntl (struct io * io, int cmd, void * arg){
    struct vioblk_device * const dev = (struct vioblk_device *)((char *)io - offsetof(struct vioblk_device, io));
    if (!dev) return -ENODEV;

    lock_acquire(&dev->lock);

    switch (cmd) {
        case IOCTL_GETBLKSZ:
            if(!arg) {
                lock_release(&dev->lock);
                return -EINVAL;
            }
            *(uint32_t *)arg = dev->blk_size;
            lock_release(&dev->lock);
            return 0;
        case IOCTL_GETEND:
            if(!arg) {
                lock_release(&dev->lock);
                return -EINVAL;
            }
            *(uint64_t *)arg = dev->capacity * dev->blk_size;
            lock_release(&dev->lock);
            return 0;
        default:
            lock_release(&dev->lock);
            return -ENOTSUP;
    }
}

#define VIRTIO_BLK_T_IN   0
#define VIRTIO_BLK_T_OUT  1
#define VIRTIO_BLK_T_FLUSH 4

long vioblk_readat(struct io *io, unsigned long long pos, void *buf, long bufsz)
{
    struct vioblk_device *dev =
        (struct vioblk_device *)((char *)io - offsetof(struct vioblk_device, io));
    if (!dev || !buf || bufsz <= 0)
        return -EINVAL;

    lock_acquire(&dev->lock);

    // Enforce block alignment for pos and length.
    if ((pos % dev->blk_size) != 0 || (bufsz % dev->blk_size) != 0) {
        lock_release(&dev->lock);
        return -EINVAL;
    }

    if (pos > dev->capacity * dev->blk_size) {
        lock_release(&dev->lock);
        return -EINVAL;
    }
    
    uint64_t start_sector = pos / dev->blk_size;
    long total_blocks = bufsz / dev->blk_size;
    if (start_sector + total_blocks > dev->capacity) {
        total_blocks = dev->capacity - start_sector;
        bufsz = total_blocks * dev->blk_size;
    }

    // Determine maximum segment size from device config.
    uint32_t max_seg = dev->regs->config.blk.seg_max;
    if (max_seg == 0)
        max_seg = bufsz;  // if not provided, treat entire I/O as one segment

    // Compute number of data descriptors needed.
    int num_data_desc = (bufsz + max_seg - 1) / max_seg;
    int total_desc_needed = 1 + num_data_desc + 1; // header + data descriptors + status

    // Collect free descriptor indices from the pool.
    int chain[total_desc_needed];
    int count = 0;
    for (int i = 0; i < VIOBLK_DESC_COUNT && count < total_desc_needed; i++) {
        if (dev->desc_free[i]) {
            chain[count++] = i;
        }
    }
    if (count < total_desc_needed) {
        lock_release(&dev->lock);
        return -EBUSY; // not enough free descriptors
    }

    // Use chain[0] as the request slot.
    int slot = chain[0];
    dev->requests[slot].in_use = 1;
    dev->requests[slot].result = 0;
    dev->requests[slot].status = 0xFF;

    // Fill the request header for a read request.
    dev->reqhdrs[slot].type = VIRTIO_BLK_T_IN;
    dev->reqhdrs[slot].reserved = 0;
    dev->reqhdrs[slot].sector = start_sector;

    // Build the chain.
    // Header descriptor (chain[0])
    dev->desc_free[chain[0]] = 0;
    dev->vq.desc[chain[0]].addr = (uint64_t)(uintptr_t)&dev->reqhdrs[slot];
    dev->vq.desc[chain[0]].len = sizeof(struct virtio_blk_req);
    if (total_desc_needed > 1) {
        dev->vq.desc[chain[0]].flags = VIRTQ_DESC_F_NEXT;
        dev->vq.desc[chain[0]].next = chain[1];
    } else {
        dev->vq.desc[chain[0]].flags = 0;
        dev->vq.desc[chain[0]].next = 0;
    }

    // Data descriptors: break the data buffer into segments.
    long remaining = bufsz;
    char *data_ptr = (char *)buf;
    for (int j = 1; j <= num_data_desc; j++) {
        int idx = chain[j];
        dev->desc_free[idx] = 0;
        uint32_t seg_len = (remaining > max_seg) ? max_seg : remaining;
        dev->vq.desc[idx].addr = (uint64_t)(uintptr_t)data_ptr;
        dev->vq.desc[idx].len = seg_len;
        // For read requests, device writes into the buffer.
        if (j < num_data_desc) {
            dev->vq.desc[idx].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
            dev->vq.desc[idx].next = chain[j+1];
        } else {
            // Last data descriptor: chain to status descriptor.
            dev->vq.desc[idx].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
            dev->vq.desc[idx].next = chain[total_desc_needed - 1];
        }
        data_ptr += seg_len;
        remaining -= seg_len;
    }

    // Status descriptor (last in chain)
    int stat_idx = chain[total_desc_needed - 1];
    dev->desc_free[stat_idx] = 0;
    dev->vq.desc[stat_idx].addr = (uint64_t)(uintptr_t)&dev->status_bytes[slot];
    dev->vq.desc[stat_idx].len = 1;
    dev->vq.desc[stat_idx].flags = VIRTQ_DESC_F_WRITE; // End of chain.
    dev->vq.desc[stat_idx].next = 0;

    // Enqueue: Add the header descriptor (chain[0]) to the avail ring.
    uint16_t avail_idx = dev->vq.avail.idx % VIOBLK_DESC_COUNT;
    dev->vq.avail.ring[avail_idx] = chain[0];
    dev->vq.avail.idx++;
    __sync_synchronize();

    // Notify device.
    dev->regs->queue_notify = 0;

    // Wait until the ISR marks this request complete.
    while (dev->requests[slot].in_use) {
        condition_wait(&dev->io_done);
    }

    long ret = dev->requests[slot].result;
    ret -= 1;
    lock_release(&dev->lock);
    return ret;
}

long vioblk_writeat(struct io *io, unsigned long long pos, const void *buf, long len)
{
    struct vioblk_device *dev =
        (struct vioblk_device *)((char *)io - offsetof(struct vioblk_device, io));
    if (!dev || !buf || len <= 0)
        return -EINVAL;

    lock_acquire(&dev->lock);

    if ((pos % dev->blk_size) != 0 || (len % dev->blk_size) != 0) {
        lock_release(&dev->lock);
        return -EINVAL;
    }

    if (pos > dev->capacity * dev->blk_size) {
        lock_release(&dev->lock);
        return -EINVAL;
    }

    uint64_t start_sector = pos / dev->blk_size;
    long total_blocks = len / dev->blk_size;
    if (start_sector + total_blocks > dev->capacity) {
        total_blocks = dev->capacity - start_sector;
        len = total_blocks * dev->blk_size;
    }

    //uint32_t blk_sz = dev->blk_size;
    uint32_t max_seg = dev->regs->config.blk.seg_max;
    if (max_seg == 0)
        max_seg = len;

    int num_data_desc = (len + max_seg - 1) / max_seg;
    int total_desc_needed = 1 + num_data_desc + 1; // header + data descriptors + status

    int chain[total_desc_needed];
    int count = 0;
    for (int i = 0; i < VIOBLK_DESC_COUNT && count < total_desc_needed; i++) {
        if (dev->desc_free[i]) {
            chain[count++] = i;
        }
    }
    if (count < total_desc_needed) {
        lock_release(&dev->lock);
        return -EBUSY;
    }

    int slot = chain[0];
    dev->requests[slot].in_use = 1;
    dev->requests[slot].result = 0;
    dev->requests[slot].status = 0xFF;

    // Fill header for write.
    dev->reqhdrs[slot].type = VIRTIO_BLK_T_OUT;
    dev->reqhdrs[slot].reserved = 0;
    dev->reqhdrs[slot].sector = start_sector;

    // Build the chain.
    // Header descriptor.
    dev->desc_free[chain[0]] = 0;
    dev->vq.desc[chain[0]].addr = (uint64_t)(uintptr_t)&dev->reqhdrs[slot];
    dev->vq.desc[chain[0]].len = sizeof(struct virtio_blk_req);
    if (total_desc_needed > 1) {
        dev->vq.desc[chain[0]].flags = VIRTQ_DESC_F_NEXT;
        dev->vq.desc[chain[0]].next = chain[1];
    } else {
        dev->vq.desc[chain[0]].flags = 0;
        dev->vq.desc[chain[0]].next = 0;
    }

    // Data descriptors: For write, device reads from our buffer.
    long remaining = len;
    const char *data_ptr = (const char *)buf;
    for (int j = 1; j <= num_data_desc; j++) {
        int idx = chain[j];
        dev->desc_free[idx] = 0;
        uint32_t seg_len = (remaining > max_seg) ? max_seg : remaining;
        dev->vq.desc[idx].addr = (uint64_t)(uintptr_t)data_ptr;
        dev->vq.desc[idx].len = seg_len;
        // For write, device reads; so do not set F_WRITE.
        if (j < num_data_desc) {
            dev->vq.desc[idx].flags = VIRTQ_DESC_F_NEXT;
            dev->vq.desc[idx].next = chain[j+1];
        } else {
            dev->vq.desc[idx].flags = VIRTQ_DESC_F_NEXT;
            dev->vq.desc[idx].next = chain[total_desc_needed - 1];
        }
        data_ptr += seg_len;
        remaining -= seg_len;
    }

    // Status descriptor.
    int stat_idx = chain[total_desc_needed - 1];
    dev->desc_free[stat_idx] = 0;
    dev->vq.desc[stat_idx].addr = (uint64_t)(uintptr_t)&dev->status_bytes[slot];
    dev->vq.desc[stat_idx].len = 1;
    dev->vq.desc[stat_idx].flags = VIRTQ_DESC_F_WRITE;
    dev->vq.desc[stat_idx].next = 0;

    // Enqueue the request.
    uint16_t avail_idx = dev->vq.avail.idx % VIOBLK_DESC_COUNT;
    dev->vq.avail.ring[avail_idx] = chain[0];
    dev->vq.avail.idx++;
    __sync_synchronize();

    dev->regs->queue_notify = 0;

    // Wait for the ISR to mark this request complete.
    while (dev->requests[slot].in_use) {
        condition_wait(&dev->io_done);
    }

    long ret = dev->requests[slot].result;
    lock_release(&dev->lock);
    return ret;
}