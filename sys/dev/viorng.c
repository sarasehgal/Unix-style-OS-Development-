// viorng.c - VirtIO rng device
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//
#include "virtio.h"
#include "intr.h"
#include "heap.h"
#include "io.h"
#include "device.h"
#include "error.h"
#include "string.h"
#include "ioimpl.h"
#include "assert.h"
#include "conf.h"
#include "intr.h"
#include "console.h"
#include "thread.h"

// INTERNAL CONSTANT DEFINITIONS
//
#ifndef VIORNG_BUFSZ
#define VIORNG_BUFSZ 256
#endif

#ifndef VIORNG_NAME
#define VIORNG_NAME "rng"
#endif

#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif

// INTERNAL TYPE DEFINITIONS
//

struct viorng_device {
    volatile struct virtio_mmio_regs * regs;
    int irqno;
    int instno;

    struct io io;

    struct {
        uint16_t last_used_idx;

        union {
            struct virtq_avail avail;
            char _avail_filler[VIRTQ_AVAIL_SIZE(1)];
        };

        union {
            volatile struct virtq_used used;
            char _used_filler[VIRTQ_USED_SIZE(1)];
        };

        // The first descriptor is a regular descriptor and is the one used in
        // the avail and used rings.

        struct virtq_desc desc[1];
    } vq;

    // bufcnt is the number of bytes left in buffer. The usable bytes are
    // between buf+0 and buf+bufcnt. (We read from the end of the buffer.)

    unsigned int bufcnt;
    char buf[VIORNG_BUFSZ];
    struct condition rd_data; 
};

// INTERNAL FUNCTION DECLARATIONS
//

static int viorng_open(struct io ** ioptr, void * aux);
static void viorng_close(struct io * io);
static long viorng_read(struct io * io, void * buf, long bufsz);
static void viorng_isr(int irqno, void * aux);

// EXPORTED FUNCTION DEFINITIONS
//
static const struct iointf viorng_iointf = {
    .read = viorng_read,
    .close = viorng_close
};
// Attaches a VirtIO rng device. Declared and called directly from virtio.c.
////////////////////////////////////////////////////////////////////////////////////////
// void viorng_attach(volatile struct virtio_mmio_regs *regs, int irqno)  
// Inputs: volatile struct virtio_mmio_regs *regs - pointer to VirtIO MMIO registers  
//         int irqno - interrupt number assigned to the VirtIO RNG device  
// Outputs: None  
// Desc:  
// this fn j attaches a VirtIO RNG device by allocating a viorng_device struct 
// n then checks if regs is valid and verifies that the device ID matches VIRTIO_ID_RNG 
// it also initializes mem for dev, setting bufcnt = 0  
// negotiates VirtIO features to find which ones are enabled 
// it then sets up the descriptortable thing, avail ring, and used ring for VirtIO queue operations  
// - Registers the device under VIORNG_NAME and enables interrupts for the IRQ.  
// then initializing- calls condition_init() to initialize the rd_data condition variable for waiting threads  
// throughout if any step fails in the process like feature negotiation or device registration, it cleans up by freeing memory  
// Side Effects: Allocates memory, modifies device registers, sets up interrupts, initializes Virtio queues  
////////////////////////////////////////////////////////////////////////////////////////
void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    struct viorng_device *viorng;
    virtio_featset_t enabled_features, wanted_features, needed_features;
    int result;
    ///////////////
    assert(regs->device_id == VIRTIO_ID_RNG);

    // alloc mem for dev
    viorng = kmalloc(sizeof(struct viorng_device));
    if (!viorng) return;
    memset(viorng, 0, sizeof(struct viorng_device));
    if (!regs) return;
    viorng->regs = regs;
    viorng->irqno = irqno;
    viorng->bufcnt = 0;
    viorng->vq.last_used_idx = 0;
    
    // Signal device that we found a driver
    regs->status |= VIRTIO_STAT_DRIVER;
    
    // Memory barrier before feature negotiation
    __sync_synchronize();
    
    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);
    
    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        kfree(viorng);
        return;
    }
    /////////
    // setup virt queue desc - single buf for rng
    viorng->vq.desc[0].addr = (uint64_t)(uintptr_t)viorng->buf;
    viorng->vq.desc[0].len = VIORNG_BUFSZ;
    viorng->vq.desc[0].flags = VIRTQ_DESC_F_WRITE; // dev writes to buf
    viorng->vq.desc[0].next = 0;
    
    viorng->vq.avail.flags = 0;  // setup avail/used rings
    viorng->vq.avail.idx = 0;
    viorng->vq.used.flags = 0;
    viorng->vq.used.idx = 0;

    // register queue in virtio dev
    virtio_attach_virtq(viorng->regs, 0, 1, (uint64_t)(uintptr_t)&viorng->vq.desc,(uint64_t)(uintptr_t)&viorng->vq.used, (uint64_t)(uintptr_t)&viorng->vq.avail); // reg virtq  
    virtio_enable_virtq(viorng->regs, 0); // enable virtq  
    enable_intr_source(viorng->irqno, VIORNG_IRQ_PRIO, viorng_isr, viorng); // setup inrpt hndlr  
    viorng->instno = register_device(VIORNG_NAME, viorng_open, viorng); // reg dev  

    if (viorng->instno < 0) {  
    kprintf("Failed to register device %s\n", VIORNG_NAME);  
    disable_intr_source(viorng->irqno); // rm intr src  
    kfree(viorng); // free dev mem  
    return;  
    }  

    condition_init(&viorng->rd_data, "dready"); // init cond var for rng ready  
    regs->status |= VIRTIO_STAT_DRIVER_OK; // final dev status upd  
    __sync_synchronize(); // mem sync b4 returning  
}
////////////////////////////////////////////////////////////////////////////////////////
// int viorng_open(struct io **ioptr, void *aux)  
// Inputs: struct io **ioptr - pointer to store the opened device io  
//         void *aux - pointer to viorng_device struct  
// Outputs: int - 0 on success, -ENODEV if device not found  
// Desc:  
// opens the VirtIO RNG device by initializing its I/O interface.  
// first, it checks if the device pointer viorng is valid, returning -ENODEV if not  
// calls `ioinit0()` to initialize the device's io struct
// increments the reference count using ioaddref() and assigns it to ioptr  
// returns 0 on success, allowing the device to be used
// Side Effects: Increments device reference count, initializes io interface 
////////////////////////////////////////////////////////////////////////////////////////
int viorng_open(struct io **ioptr, void *aux) {
    struct viorng_device *viorng = (struct viorng_device *)aux;
    if (!viorng) return -ENODEV; // no dev, ret err
    ioinit0(&viorng->io, &viorng_iointf); // init io struct w/ dev intf
    *ioptr = ioaddref(&viorng->io); // inc ref count & assign to ioptr
    return 0;
}
////////////////////////////////////////////////////////////////////////////////////////
// void viorng_close(struct io *io)  
// Inputs: struct io *io - pointer to the VirtIO RNG device io struct  
// Outputs: None  
// Desc:  
// closes the VirtIO RNG device and disables its interrupt source  
// converts `io` back into a `viorng_device` pointer using offsetof()  
// if the device is NULL, returns immediately  
// calls disable_intr_source() to stop receiving interrupts for the device  
// calls virtio_reset_virtq() to reset the virtual queue associated with the device  
// Side Effects: Disables interrupts for the device, resets its VirtIO queue 
////////////////////////////////////////////////////////////////////////////////////////
void viorng_close(struct io * io) {
    struct viorng_device * const viorng =
    (void*)io - offsetof(struct viorng_device, io);
    if (!viorng)return; // no dev
    disable_intr_source(viorng->irqno);// Disable intrrpt for dev
    virtio_reset_virtq(viorng->regs, 0);  // virtq reset
}
////////////////////////////////////////////////////////////////////////////////////////
// long viorng_read(struct io *io, void *buf, long bufsz)  
// Inputs: struct io *io - pointer to VirtIO RNG device io struct  
//         void *buf - buffer to store random bytes  
//         long bufsz - requested number of bytes to read  
// Outputs: long - number of bytes read, or -EINVAL if invalid args  
// Desc:  
// rds random bytes from the VirtIO RNG device into `buf`  
// so first it checks if `io`, `buf`, or `bufsz` are invalid, returning -EINVAL if so  
// n if the buffer is empty, it requests new random bytes from the device by:  
// steps: 
//setting up a descriptor with VIRTQ_DESC_F_WRITE n adding it to the available ring and notifying the device 
// if there is no data available, it waits using condition_wait(&viorng->rd_data)  
// once data is available, it copies the requested bytes to `buf` and updates bufcnt  
// if there’s remaining unread data, shifts it to the front of the buffer 
// Side Effects: Blocks execution if no data is available, modifies VirtIO queue  
////////////////////////////////////////////////////////////////////////////////////////
long viorng_read(struct io *io, void *buf, long bufsz) {
    struct viorng_device * const viorng = (void*)io - offsetof(struct viorng_device, io);
    if (!viorng || !buf || bufsz <= 0) return -EINVAL; // invalid args chk
    // if no data, req new rnd bytes
    
    if (viorng->bufcnt == 0) {//descriptor table
        viorng->vq.desc[0].addr = (uint64_t)(uintptr_t)viorng->buf;
        viorng->vq.desc[0].len = VIORNG_BUFSZ;
        viorng->vq.desc[0].flags = VIRTQ_DESC_F_WRITE;
        viorng->vq.desc[0].next = 0;

        
        viorng->vq.avail.ring[viorng->vq.avail.idx % 1] = 0; // add desc to avail ring
        viorng->vq.avail.idx++;

        __sync_synchronize(); // mem sync before notifying dev
        viorng->regs->queue_notify = 0; // tell dev to fill buf
    }
    int pie = disable_interrupts();
    while (viorng->bufcnt == 0) condition_wait(&viorng->rd_data); // wait till dev writes new data
    restore_interrupts(pie);
    long rdbytes = (bufsz < viorng->bufcnt) ? bufsz : viorng->bufcnt; // decide how much to copy
    memcpy(buf, viorng->buf, rdbytes); // copy from dev buf
    viorng->bufcnt -= rdbytes; // reduce avail bytes

    if (viorng->bufcnt > 0) memcpy(viorng->buf, viorng->buf + rdbytes, viorng->bufcnt); // shift remaining bytes only if needed
    return rdbytes; // return num bytes read
}
////////////////////////////////////////////////////////////////////////////////////
// void viorng_isr(int irqno, void *aux)  
// Inputs: int irqno - interrupt number for the VirtIO RNG device  
//         void *aux - pointer to viorng_device struct  
// Outputs: None  
// Desc:  
// handles VirtIO RNG device interrupts when new random data is available
// first, checks if the device pointer viorng is valid
// reads the intrpt status register to verify the interrupt source
// retrieves the amount of data written to the buffer bufcnt  
// updates the `last_used_idx` to mv to the next completed descriptor 
// signals any thrd waiting on `rd_data` that new data is available (MP3)
// acknowledges the interpt by writing back to the status register
// Side Effects: Wakes up waiting threads, modifies buffer tracking, clears interrupt flag 
////////////////////////////////////////////////////////////////////////////////////
void viorng_isr(int irqno, void *aux) {
    struct viorng_device *viorng = (struct viorng_device *)aux;
    if (!viorng) return; // chck if dev valid
    trace("viorng_isr - intrpt fire chck St=%x", viorng->regs->interrupt_status);
    uint16_t used_pos = viorng->vq.last_used_idx % 1; // get entry pos
    viorng->bufcnt = viorng->vq.used.ring[used_pos].len; // update buf size
    
    trace("viorng_isr - bufcnt=%u", viorng->bufcnt);
    viorng->vq.last_used_idx++;// move to next used desc
    condition_broadcast(&viorng->rd_data); //mp3
    viorng->regs->interrupt_ack = viorng->regs->interrupt_status; //clear intrpt
}