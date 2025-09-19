// io.h - Unified I/O object
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _IO_H_
#define _IO_H_

#include <stddef.h>

// EXPORTED TYPE DEFINITIONS
//

struct io; // opaque (defined in ioimpl.h)

#define IOCTL_GETBLKSZ  0 // arg is ignored
#define IOCTL_GETEND    2 // arg is unsigned long long *
#define IOCTL_SETEND    3 // arg is const unsigned long long *
#define IOCTL_GETPOS    4 // arg is unsigned long long *
#define IOCTL_SETPOS    5 // arg is const unsigned long long *
#define PIPE_BUFSZ PAGE_SIZE 
// EXPORTED FUNCTION DECLARATIONS
//

extern unsigned long iorefcnt(const struct io * io);
extern struct io * ioaddref(struct io * io);
extern void ioclose(struct io * io);

extern int ioctl (
    struct io * io,
    int cmd,
    void * arg
);

extern long ioread (
    struct io * io,
    void * buf,
    long bufsz
);

extern long iofill (
    struct io * io,
    void * buf,
    long bufsz
);

extern long iowrite (
    struct io * io,
    const void * buf,
    long len
);

extern long ioreadat (
    struct io * io,
    unsigned long long pos,
    void * buf,
    long bufsz
);

extern long iowriteat (
    struct io * io,
    unsigned long long pos,
    const void * buf,
    long len
);

extern int ioseek (
    struct io * io,
    unsigned long long pos
);

extern int ioblksz(struct io * io);
extern struct io * create_memory_io(void * buf, size_t size);
extern struct io * create_seekable_io(struct io * io);
/**
 * @brief Creates a unidirectional pipe (writer and reader I/O objects)
 * 
 * @param wioptr pointer to store writer-end io*
 * @param rioptr pointer to store reader-end io*
 */

 void create_pipe(struct io **wioptr, struct io **rioptr);


#endif // _IO_H_