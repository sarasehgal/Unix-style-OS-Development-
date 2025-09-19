// rtc.c - Goldfish RTC driver
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef RTC_TRACE
#define TRACE
#endif

#ifdef RTC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "rtc.h"
#include "device.h"
#include "ioimpl.h"
#include "console.h"
#include "string.h"
#include "heap.h"
#include "error.h"
#include <stdint.h>

// INTERNAL TYPE DEFINITIONS
// 

struct rtc_regs {
    // 2 32-bit regs to form a 64-bit timestamp
    uint32_t time_low; 
    uint32_t time_high;
};

struct rtc_device {
    // initializing the pointer to rtc_regs
    // need to store mmio_base somewhere so we introduce a ptr to regs n then attach
    volatile struct rtc_regs *regs;
    // alr initialized in ioimpl.h
    struct io io;
    //ref counter
    int instno;
};

// INTERNAL FUNCTION DEFINITIONS

static int rtc_open(struct io ** ioptr, void * aux);
static void rtc_close(struct io * io);
static int rtc_cntl(struct io * io, int cmd, void * arg);
static long rtc_read(struct io * io, void * buf, long bufsz);

static uint64_t read_real_time(struct rtc_regs * regs);

// EXPORTED FUNCTION DEFINITIONS

struct iointf rtc_intf = {
    .close = &rtc_close,
    .cntl = &rtc_cntl,
    .read = &rtc_read,
};
////////////////////////////////////////////////////////////////////////////////
// rtc_attach(void *mmio_base)
// Inputs: void *mmio_base - base addr of RTC mmio regs (memory-mapped I/O)
// Outputs: None
// Desc: 
// - This fn sets up the RTC device by allocating a struct rtc_device, storing 
//   the mmio base addr, and registering it under the name "rtc".
// - First, it checks if mmio_base is valid. If not, just return since there's 
//   nothing to attach.
// - Then, it allocates memory for rtc_device using kmalloc. If allocation fails, 
//   return immediately since we can't proceed without a struct.
// - After that, it stores mmio_base inside rtc->regs (casting it to an rtc_regs ptr).
// - The device is then registered using register_device() with "rtc" as its name 
//   and rtc_open as its open function. This assigns rtc->instno a unique ID.
// - If registration fails (instno < 0), we free the allocated memory to prevent leaks.
// - Finally, it initializes the I/O interface for the device with ioinit1() so 
//   other functions can interact with it properly.
// Side Effects: Allocates memory using kmalloc, registers device with system
////////////////////////////////////////////////////////////////////////////////
void rtc_attach(void * mmio_base) {
    if (!mmio_base) return;
    // allocates mem for rtc_device struct (check for null)
    struct rtc_device *rtc = kmalloc(sizeof(struct rtc_device));
    if (!rtc) return; 

    rtc->regs = (volatile struct rtc_regs *)mmio_base;// stores mmio_base(void ptr) into regs (casted)
    rtc->instno = register_device("rtc", rtc_open, rtc);// register - name rtc (check for fail)
    if (rtc->instno < 0) {//freeing mem if reg fails
        kfree(rtc);//free dev
        return;
    }
    ioinit1(&rtc->io, &rtc_intf);//initialize - 1 means ref cnt 1
}
////////////////////////////////////////////////////////////////////////////////
// int rtc_open(struct io **ioptr, void *aux)
// Inputs: struct io **ioptr - pointer to store the opened device io
//         void *aux - pointer to rtc_device struct (passed by attach)
// Outputs: int - 0 if success, -EINVAL if invalid args, -ENOTSUP if regs missing
// Desc: 
// - Opens the RTC device by associating it with ioptr, checking validity, 
//   and adding a reference count.
// - First, it checks if aux or ioptr are NULL, meaning invalid args were passed, 
//   and returns -EINVAL if so.
// - Then, it casts aux to rtc_device so we can access the struct fields.
// - If rtc->regs is NULL, that means the device doesn’t have valid registers, 
//   so return -ENOTSUP (operation not supported).
// - Otherwise, we associate ioptr with rtc->io and increase its reference count
//   using ioaddref().
// - ioaddref() increments refcnt, so we don't manually modify it here. 
// - Finally, it returns 0, indicating success.
// Side Effects: Increments reference count of rtc->io
////////////////////////////////////////////////////////////////////////////////
int rtc_open(struct io ** ioptr, void * aux) {
    if (!aux || !ioptr) return -EINVAL; //aux to rtc_device and associate it with ioptr
    struct rtc_device *rtc = (struct rtc_device *)aux;
    if (!rtc->regs) return -ENOTSUP;// chcking valid regs
    // ioaddref increments count anyway so can use that for incrementing
    // *ioptr = ioaddref(&rtc->io);
    *ioptr = ioaddref(&rtc->io);
    return 0;  //success
}
////////////////////////////////////////////////////////////////////////////////
// void rtc_close(struct io *io)
// Inputs: struct io *io - pointer to the RTC device's io struct
// Outputs: None
// Desc: 
// - Closes the RTC device by verifying that its reference count is 0.
// - First, it checks if io is NULL (invalid input). If so, just return.
// - Then, it calculates the pointer to the rtc_device struct by using
//   offsetof() to get the base address of the struct from the io field.
// - Finally, it asserts that the reference count (io.refcnt) is 0 before 
//   actually closing the device. This prevents accidental closing while 
//   other parts of the system might still be using it.
// Side Effects: None
////////////////////////////////////////////////////////////////////////////////
void rtc_close(struct io * io) {
    if (!io) return; 
    // get device from io ptr
    struct rtc_device *rtc = (struct rtc_device *)((char *)io - offsetof(struct rtc_device, io));
    assert(rtc->io.refcnt==0);//null ref cnt
}
////////////////////////////////////////////////////////////////////////////////
// int rtc_cntl(struct io * io, int cmd, void * arg)
// Inputs: struct io *io - pointer to the RTC device's io struct
//         int cmd - command specifying the operation (currently only supports IOCTL_GETBLKSZ)
//         void *arg - optional argument (not used here)
// Outputs: int - returns 8 if IOCTL_GETBLKSZ is requested, -ENOTSUP otherwise
// Desc: 
// - Handles control operations for the RTC device.
// - Right now, it only supports IOCTL_GETBLKSZ, which returns 8 (block size).
// - If cmd matches IOCTL_GETBLKSZ, it returns 8.
// - Otherwise, returns -ENOTSUP, meaning the operation isn’t supported.
// Side Effects: None
////////////////////////////////////////////////////////////////////////////////
int rtc_cntl(struct io * io, int cmd, void * arg) {
    // check if size matches
    if (cmd == IOCTL_GETBLKSZ) return 8;//matches
    else return -ENOTSUP;//deosnt match
}
////////////////////////////////////////////////////////////////////////////////
// long rtc_read(struct io *io, void *buf, long bufsz)
// Inputs: struct io *io - pointer to the RTC device's io struct
//         void *buf - buffer to store the read value
//         long bufsz - size of the buffer
// Outputs: long - number of bytes read (8) or -EINVAL if invalid
// Desc: 
// - Reads the current time from the RTC registers and stores it in buf.
// - First, it checks if io or buf are NULL. If so, return -EINVAL.
// - Then, it checks if the buffer size is at least 8 bytes (size of uint64_t).
// - It gets the real-time clock value using read_real_time() and copies it into buf.
// - Finally, it returns the number of bytes read (8).
// Side Effects: None
////////////////////////////////////////////////////////////////////////////////
long rtc_read(struct io *io, void *buf, long bufsz) {
    if (!io || !buf) return -EINVAL;
    if (bufsz < sizeof(uint64_t)) return -EINVAL; 
    struct rtc_device *rtc = (struct rtc_device *)((char *)io - offsetof(struct rtc_device, io));
    // rd clock vals n copy to bffr
    uint64_t real_time = read_real_time((struct rtc_regs *)rtc->regs);
    memcpy(buf, &real_time, sizeof(real_time));
    return sizeof(real_time); // ret no of bytes
}
////////////////////////////////////////////////////////////////////////////////
// uint64_t read_real_time(struct rtc_regs *regs)
// Inputs: struct rtc_regs *regs - pointer to RTC register struct
// Outputs: uint64_t - 64-bit real-time clock value
// Desc: 
// - Reads the current time from the RTC registers and returns it as a 64-bit value.
// - First, it checks if regs is NULL, if so return 0 since we can't read from NULL.
// - Reads the time_low register first (lower 32 bits).
// - Then reads time_high (upper 32 bits) to form a full 64-bit timestamp.
// - Uses bitwise shift `((uint64_t)high << 32) | low` to combine both values.
// Side Effects: None
////////////////////////////////////////////////////////////////////////////////
uint64_t read_real_time(struct rtc_regs * regs) {
    // reads time from regs and returns a combined value 64 bits
    // 1st low, 2nd high
    if (!regs) return 0;
    uint32_t low, high;
    low = regs->time_low;
    high = regs->time_high;
    return ((uint64_t)high << 32) | low;
}