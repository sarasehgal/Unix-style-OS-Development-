#ifndef VIOBLK_H
#define VIOBLK_H

#include "virtio.h"    // For struct virtio_mmio_regs, etc.
#include "io.h"        // For struct io

/* Function prototypes for the VirtIO block device driver */
void vioblk_attach(volatile struct virtio_mmio_regs *regs, int irqno);
int vioblk_open(struct io **ioptr, void *aux);
void vioblk_close(struct io *io);
long vioblk_readat(struct io *io, unsigned long long pos, void *buf, long bufsz);
long vioblk_writeat(struct io *io, unsigned long long pos, const void *buf, long len);
int vioblk_cntl(struct io *io, int cmd, void *arg);
void vioblk_isr(int srcno, void *aux);

#endif /* VIOBLK_H */