#include "conf.h"
#include "console.h"
#include "elf.h"
#include "assert.h"
#include "thread.h"
#include "process.h"
#include "memory.h"
#include "fs.h"
#include "io.h"
#include "device.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "intr.h"
#include "dev/virtio.h"
#include "heap.h"
#include "string.h"

#define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE-VIRTIO0_MMIO_BASE)
extern char _kimg_end[]; 




void main(void) {
    struct io *blkio;
    int result;
    int i;

    
    console_init();
    devmgr_init();
    intrmgr_init();
    thrmgr_init();
    memory_init();
    procmgr_init();


    uart_attach((void*)UART0_MMIO_BASE, UART0_INTR_SRCNO+0);
    uart_attach((void*)UART1_MMIO_BASE, UART0_INTR_SRCNO+1);
    rtc_attach((void*)RTC_MMIO_BASE);
    
    for (i = 0; i < 8; i++) {
        virtio_attach ((void*)VIRTIO0_MMIO_BASE + i*VIRTIO_MMIO_STEP, VIRTIO0_INTR_SRCNO + i);
    }

    result = open_device("vioblk", 0, &blkio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open vioblk\n");
    }

    result = fsmount(blkio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to mount filesystem\n");
    }

    result = open_device("uart", 1, &current_process()->iotab[2]);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open UART\n");
    }

    // insert testcase below
    
}



