

#ifdef UART_TRACE
#define TRACE
#endif

#ifdef UART_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "uart.h"
#include "device.h"
#include "intr.h"
#include "heap.h"

#include "ioimpl.h"
#include "console.h"

#include "error.h"

#include <stdint.h>

#include "thread.h"

// COMPILE-TIME CONSTANT DEFINITIONS
//

#ifndef UART_RBUFSZ
#define UART_RBUFSZ 64
#endif

#ifndef UART_INTR_PRIO
#define UART_INTR_PRIO 1
#endif

#ifndef UART_NAME
#define UART_NAME "uart"
#endif

// INTERNAL TYPE DEFINITIONS
// 

struct uart_regs {
    union {
        char rbr; // DLAB=0 read
        char thr; // DLAB=0 write
        uint8_t dll; // DLAB=1
    };
    
    union {
        uint8_t ier; // DLAB=0
        uint8_t dlm; // DLAB=1
    };
    
    union {
        uint8_t iir; // read
        uint8_t fcr; // write
    };

    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
};

#define LCR_DLAB (1 << 7)
#define LSR_OE (1 << 1)
#define LSR_DR (1 << 0)
#define LSR_THRE (1 << 5)
#define IER_DRIE (1 << 0)
#define IER_THREIE (1 << 1)

struct ringbuf {
    unsigned int hpos; // head of queue (from where elements are removed)
    unsigned int tpos; // tail of queue (where elements are inserted)
    char data[UART_RBUFSZ];
};

struct uart_device {
    volatile struct uart_regs * regs;
    int irqno;
    int instno;

    struct io io;

    unsigned long rxovrcnt; // number of times OE was set

    struct ringbuf rxbuf;
    struct ringbuf txbuf;
    struct condition rxbuf_not_empty;
    struct condition txbuf_not_full;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int uart_open(struct io ** ioptr, void * aux);
static void uart_close(struct io * io);
static long uart_read(struct io * io, void * buf, long bufsz);
static long uart_write(struct io * io, const void * buf, long len);

static void uart_isr(int srcno, void * driver_private);

static void rbuf_init(struct ringbuf * rbuf);
static int rbuf_empty(const struct ringbuf * rbuf);
static int rbuf_full(const struct ringbuf * rbuf);
static void rbuf_putc(struct ringbuf * rbuf, char c);
static char rbuf_getc(struct ringbuf * rbuf);

// EXPORTED FUNCTION DEFINITIONS
// 

void uart_attach(void * mmio_base, int irqno) {
    static const struct iointf uart_iointf = {
        .close = &uart_close,
        .read = &uart_read,
        .write = &uart_write
    };

    struct uart_device * uart;

    uart = kcalloc(1, sizeof(struct uart_device));

    uart->regs = mmio_base;
    uart->irqno = irqno;

    ioinit0(&uart->io, &uart_iointf);

    // Check if we're trying to attach UART0, which is used for the console. It
    // had already been initialized and should not be accessed as a normal
    // device.

    if (mmio_base != (void*)UART0_MMIO_BASE) {

        uart->regs->ier = 0;
        uart->regs->lcr = LCR_DLAB;
        // fence o,o ?
        uart->regs->dll = 0x01;
        uart->regs->dlm = 0x00;
        // fence o,o ?
        uart->regs->lcr = 0; // DLAB=0

        uart->instno = register_device(UART_NAME, uart_open, uart);

    } else
        uart->instno = register_device(UART_NAME, NULL, NULL);
}

// int uart_open(struct io ** ioptr, void * aux)
// Inputs: struct io ** ioptr - pointer to reference of ioptr struct, 
//         void * aux - pointer to uart device structure
// Outputs: 0 on success
// Description: Opens given uart device
// Side Effects: changes ioptr's value, enables DRIE

int uart_open(struct io ** ioptr, void * aux) {       
    if(ioptr == NULL || aux == NULL){
        panic("uart open bad param");
    }  
    struct uart_device * const uart = aux;

    trace("%s()", __func__);

    if (iorefcnt(&uart->io) != 0)
        return -EBUSY;
    
    // Reset receive and transmit buffers
    
    rbuf_init(&uart->rxbuf);
    rbuf_init(&uart->txbuf);

    // Read receive buffer register to flush any stale data in hardware buffer

    uart->regs->rbr; // forces a read because uart->regs is volatile

    // FIXME your code goes here
    uart->regs->ier = IER_DRIE;

    enable_intr_source(uart->irqno,UART_INTR_PRIO,uart_isr,aux);

    condition_init(&(uart->rxbuf_not_empty), "rxbuf_not_empty");
    condition_init(&(uart->txbuf_not_full), "txbuf_not_full");

    *ioptr = ioaddref(&uart->io);


    return 0;
}

// void uart_close(struct io * io)
// Inputs: struct io * io - io ptr associated with uart device
// Outputs: none
// Description: closes uart communication
// Side Effects: io becomes NULL
void uart_close(struct io * io) {
    if(io == NULL){
        panic("uart close bad params");
    }
    struct uart_device * const uart =
        (void*)io - offsetof(struct uart_device, io);

    trace("%s()", __func__);
    assert (iorefcnt(io) == 0);

    // FIXME your code goes here
    io = NULL;                          //reverse uart_open

    disable_intr_source(uart->irqno);

    uart->regs->ier = 0;

    uart->regs->rbr;

    rbuf_init(&uart->rxbuf);
    rbuf_init(&uart->txbuf);
}

// long uart_read(struct io * io, void * buf, long bufsz)
// Inputs: struct io * io - io ptr associated with uart device
//         void * buf - buffer to write to
//         long bufsz - size of buffer in bytes
// Outputs: size of data written to buffer
// Description: Copies data read from device into input buffer
// Side Effects: Enables DRIE

long uart_read(struct io * io, void * buf, long bufsz) {    //change for cp3
    // FIXME your code goes here
    if(io == NULL || buf == NULL){
        panic("uart read bad params");
    }
    struct uart_device * const uart =
        (void*)io - offsetof(struct uart_device, io);

    if(UART_RBUFSZ < bufsz){
        return -ENOTSUP;
    }

    for(long i = 0; i < bufsz; i++){
        int pie = disable_interrupts();
        while(rbuf_empty(&uart->rxbuf)){
            condition_wait(&uart->rxbuf_not_empty);
        }
        ((char *)buf)[i] = rbuf_getc(&uart->rxbuf);
        uart->regs->ier |= IER_DRIE;
        restore_interrupts(pie);
    }



    return bufsz;
}

// long uart_write(struct io * io, const void * buf, long len)
// Inputs: struct io * io - io ptr associated with uart device
//         void * buf - buffer with data to write to device
//         long len - size of buffer in bytes
// Outputs: size of data written to device
// Description: Writes data into uart device transmit buffer
// Side Effects: Enables THREIE

long uart_write(struct io * io, const void * buf, long len) {       //change for cp3
    // FIXME your code goes here
    if(io == NULL || buf == NULL){
        panic("uart_write bad params");
    }
    struct uart_device * const uart =
        (void*)io - offsetof(struct uart_device, io);

    if(len == 0){
        return -ENOTSUP;
    }

    for(long i = 0; i < len; i++){
        int pie = disable_interrupts();
        while(rbuf_full(&uart->txbuf)){
            condition_wait(&uart->txbuf_not_full);
        }
        
        rbuf_putc(&uart->txbuf,((char*)buf)[i]);
        uart->regs->ier |= IER_THREIE;   
        restore_interrupts(pie);
    }

    return len;
}

// void uart_isr(int srcno, void * aux)
// Inputs: struct io * io - io ptr associated with uart device
//         void * buf - buffer with data to write to device
//         long len - size of buffer in bytes
// Outputs: none
// Description: Analyzes interrupt source and handles interrupt accodringly
// Side Effects: May disable DRIE or THREIE

void uart_isr(int srcno, void * aux) {                              //change for cp3
    // FIXME your code goes here
    if(aux == NULL){
        panic("uart isr bad params");
    }
    struct uart_device * const uart = aux;

    int pie;
    if(!rbuf_full(&uart->rxbuf)){
        if((uart->regs->lsr & LSR_DR)){
            pie = disable_interrupts();
            rbuf_putc(&uart->rxbuf,uart->regs->rbr);
            restore_interrupts(pie);
        }
    }
    else{
        uart->regs->ier &= (~IER_DRIE);
    }

    if(!rbuf_empty(&uart->txbuf)){
        if((uart->regs->lsr & LSR_THRE)){
            pie = disable_interrupts();
            uart->regs->thr = rbuf_getc(&uart->txbuf);
            restore_interrupts(pie);
        }
    }
    else{
         uart->regs->ier &= (~IER_THREIE);
    }
    if(!rbuf_empty(&uart->rxbuf)){
        condition_broadcast(&uart->rxbuf_not_empty);
    }

    if(!rbuf_full(&uart->txbuf)){
        condition_broadcast(&uart->txbuf_not_full);
    }
   // restore_interrupts(pie);
}

void rbuf_init(struct ringbuf * rbuf) {
    rbuf->hpos = 0;
    rbuf->tpos = 0;
}

int rbuf_empty(const struct ringbuf * rbuf) {
    return (rbuf->hpos == rbuf->tpos);
}

int rbuf_full(const struct ringbuf * rbuf) {
    return ((uint16_t)(rbuf->tpos - rbuf->hpos) == UART_RBUFSZ);
}

void rbuf_putc(struct ringbuf * rbuf, char c) {
    uint_fast16_t tpos;

    tpos = rbuf->tpos;
    rbuf->data[tpos % UART_RBUFSZ] = c;
    asm volatile ("" ::: "memory");
    rbuf->tpos = tpos + 1;
}

char rbuf_getc(struct ringbuf * rbuf) {
    uint_fast16_t hpos;
    char c;

    hpos = rbuf->hpos;
    c = rbuf->data[hpos % UART_RBUFSZ];
    asm volatile ("" ::: "memory");
    rbuf->hpos = hpos + 1;
    return c;
}

// The functions below provide polled uart input and output for the console.

#define UART0 (*(volatile struct uart_regs*)UART0_MMIO_BASE)

void console_device_init(void) {
    UART0.ier = 0x00;

    // Configure UART0. We set the baud rate divisor to 1, the lowest value,
    // for the fastest baud rate. In a physical system, the actual baud rate
    // depends on the attached oscillator frequency. In a virtualized system,
    // it doesn't matter.
    
    UART0.lcr = LCR_DLAB;
    UART0.dll = 0x01;
    UART0.dlm = 0x00;

    // The com0_putc and com0_getc functions assume DLAB=0.

    UART0.lcr = 0;
}

void console_device_putc(char c) {
    // Spin until THR is empty
    while (!(UART0.lsr & LSR_THRE))
        continue;

    UART0.thr = c;
}

char console_device_getc(void) {
    // Spin until RBR contains a byte
    while (!(UART0.lsr & LSR_DR))
        continue;
    
    return UART0.rbr;
}