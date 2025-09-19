// #include "io.h"
// #include "assert.h"
// #include "console.h"
// #include "string.h"
// #include "errno.h"


// // Test function for memio
// void test_memio_read_write() {
//     char buffer[512];
//     struct io *memio;

// // Test invalid input (buf == NULL)
// memio = create_memory_io(NULL, sizeof(buffer));
// assert(memio == NULL);

// // Test invalid input (size == 0)
// memio = create_memory_io(buffer, 0);
// assert(memio == NULL);

//     // Test valid input
//     memio = create_memory_io(buffer, sizeof(buffer));
//     assert(memio != NULL);

//     // Write data to memio
//     const char *data = "Hello, memio!";
//     long write_result = iowriteat(memio, 0, data, strlen(data) + 1);
//     assert(write_result == strlen(data) + 1);

//     // Read data back from memio
//     char read_buffer[512];
//     long read_result = ioreadat(memio, 0, read_buffer, sizeof(read_buffer));
//     assert(read_result == strlen(data) + 1);

//     // Verify the data
//     assert(strcmp(data, read_buffer) == 0);

//     // Clean up
//     ioclose(memio);

//     kprintf("test_memio_read_write: PASSED\n");
// }

// // Main function to run all tests
// int main() {
//     console_init(); // Initialize console for output

//     kprintf("Running tests...\n");

//     // Run memio tests
//     test_memio_read_write();

//     kprintf("All tests passed!\n");

//     return 0;
// }


// #include "io.h"
// #include "assert.h"
// #include "console.h"
#include "string.h"
#include "conf.h"
#include "heap.h"
#include "console.h"
#include "elf.h"
#include "assert.h"
#include "thread.h"
#include "fs.h"
#include "io.h"
#include "device.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "intr.h"
#include "dev/virtio.h"
#include "ktfs.h"


// Basic memio test
void test_memio_basic() {
    char buffer[128];  // Test memory region
    struct io *memio;

    // Create a memory I/O object
    memio = create_memory_io(buffer, sizeof(buffer));
    assert(memio != NULL);

    // Test writing to memio
    const char *test_str = "memio test";
    long write_result = iowriteat(memio, 0, test_str, strlen(test_str) + 1);
    assert(write_result == strlen(test_str) + 1);

    // Test reading from memio
    char read_buffer[128];
    long read_result = ioreadat(memio, 0, read_buffer, strlen(test_str) + 1);
    assert(read_result == strlen(test_str) + 1);

    // Verify read data
    assert(strcmp(test_str, read_buffer) == 0);

    // Cleanup
    ioclose(memio);

    kprintf("test_memio_basic: PASSED\n");
}

// // Main function to run test
// extern char _kimg_end[]; 
// int main() {
//     console_init();
//     heap_init(_kimg_end, UMEM_START); 
//     kprintf("Running basic memio test...\n");

//     test_memio_basic();

//     kprintf("All basic tests passed!\n");
//     return 0;
// }

extern  char  _kimg_blob_start[];
extern char  _kimg_blob_end[];
extern char _kimg_end[]; 

void test_ktfs(){
    console_init();
    devmgr_init();
    intrmgr_init();
    thrmgr_init();
    heap_init(_kimg_end, UMEM_START); 

    struct io *memio;
    memio = create_memory_io(_kimg_blob_start, _kimg_blob_end - _kimg_blob_start);

    kprintf("memio created\n");

    fsmount(memio);
    kprintf("mount successful!\n");

    struct io * io;
    if(fsopen("hello.txt",&io)){
        kprintf("open failed!\n");
    }
    else{
        kprintf("open sucessful!\n");
    };

    char c[5];                //read first block of data
    ioreadat(io, 0, c, 5);

    kprintf(c);
    kprintf("\n");

    char b[5] = "bye";
    iowriteat(io,2,b,3);

    ioreadat(io, 0, c, 5);

    kprintf(c);
    kprintf("\n");

    ioclose(io);
}

void test_ktfs_create(){
    console_init();
    devmgr_init();
    intrmgr_init();
    thrmgr_init();
    heap_init(_kimg_end, UMEM_START); 

    struct io *memio;
    memio = create_memory_io(_kimg_blob_start, _kimg_blob_end - _kimg_blob_start);

    kprintf("memio created\n");

    fsmount(memio);
    kprintf("mount successful!\n");

    struct io * io;
    if(fscreate("wow") <0){
        panic("create failed");
    }
    kprintf("create sucessful!\n");

    if(fsopen("wow",&io)){
        panic("open failed!\n");
    }
    else{
        kprintf("open sucessful!\n");
    };

    char c[10];
    if(ioreadat(io,0, c, 10) != 0){
        panic("read from empty file!");
    }

    long arg = 3;

    if(ioctl(io,IOCTL_SETEND, &arg) > 0){
        panic("getend failed!");
    }

    if(ioctl(io,IOCTL_GETEND, &arg) > 0){
        panic("getend failed!");
    }
    if(arg != 3){
        panic("setend failed!");
    }

    char i[3] = "wow";

    iowriteat(io,0,i,3);

    ioreadat(io,0,c,3);

    kprintf(c);

    ioclose(io);

    if(fsdelete("wow") < 0){
        panic("delete failed");
    }

    if(fsopen("wow", &io) < 0){
        kprintf("open failed! (good!)\n");
        return;
    }
    else{
        panic("opened deleted file!");
    }
}

#define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE-VIRTIO0_MMIO_BASE)

void test_elf(){
    struct io *blkio;
    struct io *termio;
    struct io *trekio;
    int result;
    int i;
    int tid;
    void (*exe_entry)(struct io*);

    console_init();
    devmgr_init();
    intrmgr_init();
    thrmgr_init();
    heap_init(_kimg_end, UMEM_START); 
    uart_attach((void*)UART0_MMIO_BASE, UART0_INTR_SRCNO+0);
    uart_attach((void*)UART1_MMIO_BASE, UART0_INTR_SRCNO+1);
    rtc_attach((void*)RTC_MMIO_BASE);
    
    for (i = 0; i < 8; i++) {
        virtio_attach ((void*)VIRTIO0_MMIO_BASE + i*VIRTIO_MMIO_STEP, VIRTIO0_INTR_SRCNO + i);
    }

    //run_vioblk_unit_tests();

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

    result = open_device("uart", 1, &termio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open UART\n");
    }

    result = fsopen("hello", &trekio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open hello\n");
    }

    // TODO:
    // 1. Load the trek file into memory
    result = elf_load(trekio,(void(**)(void))&exe_entry);
    // 2. Verify the loading of the file into memory
    if(result < 0){
        kprintf("Error: %d\n", result);
        panic("Failed to load into memory\n");
    }
    // 3. Run trek on a new thread
    tid = thread_spawn("hello", (void (*)(void))exe_entry, termio);
    // 4. Verify that the thread was able to run properly, if it was have the main thread wait for trek to finish
    if(tid < 0){
        kprintf("Error: %d\n", tid);
        panic("Failed to run thread\n");
    }
    thread_join(0);
}


int main() {
    //console_init();
    //heap_init(_kimg_end, UMEM_START); 

    test_ktfs_create();
    //test_elf();

    return 0;
}