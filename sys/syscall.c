/*! @file syscall.c
    @brief system call handlers 
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/

#ifdef SYSCALL_TRACE
#define TRACE
#endif

#ifdef SYSCALL_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "scnum.h"
#include "process.h"
#include "memory.h"
#include "io.h"
#include "device.h"
#include "fs.h"
#include "intr.h"
#include "timer.h"
#include "error.h"
#include "thread.h"

extern void handle_syscall(struct trap_frame * tfr);

static int64_t syscall(const struct trap_frame * tfr);
static int sysexit(void);
static int sysexec(int fd, int argc, char ** argv);
static int syswait(int tid);
static int sysprint(const char * msg);
static int sysusleep(unsigned long us);
static int sysdevopen(int fd, const char * name, int instno);
static int sysfsopen(int fd, const char * name);
static int sysclose(int fd);
static long sysread(int fd, void * buf, size_t bufsz);
static long syswrite(int fd, const void * buf, size_t len);
static int sysioctl(int fd, int cmd, void * arg);
static int syspipe(int * wfdptr, int * rfdptr);
static int sysfscreate(const char* name); 
static int sysfsdelete(const char* name);
int sysiodup (int oldfd, int newfd);
int sysfork	(const struct trap_frame * tfr);	


void handle_syscall(struct trap_frame * tfr) {
    tfr->sepc += 4;
    tfr->a0 = syscall(tfr);
}

int64_t syscall(const struct trap_frame * tfr) {
    switch(tfr->a7){
        case SYSCALL_EXIT:      return sysexit();
        case SYSCALL_EXEC:      return sysexec((int)tfr->a0, (int)tfr->a1, (char**)tfr->a2);
        case SYSCALL_WAIT:      return syswait((int)tfr->a0);
        case SYSCALL_PRINT:     return sysprint((const char*)tfr->a0);
        case SYSCALL_USLEEP:    return sysusleep((unsigned long)tfr->a0);
        case SYSCALL_DEVOPEN:   return sysdevopen((int)tfr->a0, (char*)tfr->a1, (int)tfr->a2);
        case SYSCALL_FSOPEN:    return sysfsopen((int)tfr->a0, (char*)tfr->a1);
        case SYSCALL_FSCREATE:  return sysfscreate((char*)tfr->a0);
        case SYSCALL_FSDELETE:  return sysfsdelete((char*)tfr->a0);
        case SYSCALL_CLOSE:     return sysclose((int)tfr->a0);
        case SYSCALL_READ:      return sysread((int)tfr->a0, (void*)tfr->a1, (size_t)tfr->a2);
        case SYSCALL_WRITE:     return syswrite((int)tfr->a0, (void*)tfr->a1, (size_t)tfr->a2);
        case SYSCALL_IOCTL:     return sysioctl((int)tfr->a0, (int)tfr->a1, (void*)tfr->a2);
        case SYSCALL_PIPE:      return syspipe((int*)tfr->a0, (int*)tfr->a1);
        case SYSCALL_IODUP:     return sysiodup((int)tfr->a0, (int)tfr->a1);
        case SYSCALL_FORK:      return sysfork(tfr);
        default:                return -ENOTSUP;
    }
}

int sysexit(void) { process_exit(); return 0; }

int sysexec(int fd, int argc, char ** argv) {
   // kprintf("exec\n");
    if (argc < 0 || !argv || fd < 0 || fd >= PROCESS_IOMAX || !current_process()->iotab[fd])
        return -EBADFD;
    return process_exec(current_process()->iotab[fd], argc, argv);
}

int syswait(int tid) { kprintf("wait\n"); return (tid >= 0) ? thread_join(tid) : -EINVAL; }

int sysprint(const char * msg) {
   // kprintf("print\n");
    kprintf("Thread <%s:%d> says: %s\n", thread_name(running_thread()), running_thread(), msg);
    return 0;
}

int sysusleep(unsigned long us) {
   // kprintf("sleep\n");
    sleep_us(us);
    return 0;
}

int sysdevopen(int fd, const char * name, int instno) {
    //kprintf("devopen\n");
    int desc = (fd < 0) ? ({ for (desc = 0; desc < PROCESS_IOMAX && current_process()->iotab[desc]; ++desc); desc; }) : fd;
    if (desc >= PROCESS_IOMAX || (fd >= 0 && current_process()->iotab[fd])) return -EBADFD;
    return (open_device(name, instno, &current_process()->iotab[desc]) < 0) ? -1 : desc;
}

int sysfsopen(int fd, const char * name) {
    //kprintf("fsopen\n");
    int desc = (fd < 0) ? ({ for (desc = 0; desc < PROCESS_IOMAX && current_process()->iotab[desc]; ++desc); desc; }) : fd;
    if (desc >= PROCESS_IOMAX || (fd >= 0 && current_process()->iotab[fd])) return -EBADFD;
    return (fsopen(name, &current_process()->iotab[desc]) < 0) ? -1 : desc;
}

int sysclose(int fd) {
    //kprintf("close\n");
    if (fd < 0 || fd >= PROCESS_IOMAX || !current_process()->iotab[fd]) return -EBADFD;
    ioclose(current_process()->iotab[fd]); current_process()->iotab[fd] = NULL;
    return 0;
}

long sysread(int fd, void * buf, size_t bufsz) {
   // kprintf("read\n");
    if (fd < 0 || fd >= PROCESS_IOMAX || !current_process()->iotab[fd]) return -EBADFD;
    return ioread(current_process()->iotab[fd], buf, bufsz);
}

long syswrite(int fd, const void * buf, size_t len) {
   // kprintf("write\n");
    if (fd < 0 || fd >= PROCESS_IOMAX || !current_process()->iotab[fd]) return -EBADFD;
    return iowrite(current_process()->iotab[fd], buf, len);
}

int sysioctl(int fd, int cmd, void * arg) {
    //kprintf("ioctl\n");
    if (fd < 0 || fd >= PROCESS_IOMAX || !current_process()->iotab[fd]) return -EBADFD;
    return ioctl(current_process()->iotab[fd], cmd, arg);
}

int sysfork	(const struct trap_frame *	tfr){
    return process_fork(tfr);
}

int syspipe(int * wfdptr, int * rfdptr) { 
    if (!wfdptr || !rfdptr) return -EINVAL;

    struct io *wio = NULL, *rio = NULL;
    create_pipe(&wio, &rio);
    if (!wio || !rio) return -ENOMEM;

    int wfd = *wfdptr;
    int rfd = *rfdptr;
    struct process *proc = current_process();

    if (wfd < 0 || rfd < 0) {
        for (int i = 0; i < PROCESS_IOMAX; i++) {
            if (wfd < 0 && !proc->iotab[i]) wfd = i;
            else if (rfd < 0 && !proc->iotab[i]) rfd = i;
        }
    }

    if (wfd < 0 || rfd < 0 || wfd == rfd || wfd >= PROCESS_IOMAX || rfd >= PROCESS_IOMAX ||
        proc->iotab[wfd] || proc->iotab[rfd]) {
        ioclose(wio);
        ioclose(rio);
        return -EBADFD;
    }

    proc->iotab[wfd] = wio;
    proc->iotab[rfd] = rio;
    *wfdptr = wfd;
    *rfdptr = rfd;
    return 0;
}

int sysfscreate(const char* name) { kprintf("create\n"); return fscreate(name); }

int sysfsdelete(const char* name) { kprintf("delete\n"); return fsdelete(name); }

int sysiodup (int oldfd, int newfd){
    if(oldfd < 0 || oldfd >= PROCESS_IOMAX){
        return -EBADFD;
    }
    if(current_process()->iotab[oldfd] == NULL){
        return -EBADFD;
    }
    if(newfd < 0){                                      //look for next available fd
        for(newfd = 0; newfd < PROCESS_IOMAX; newfd++){
            if(current_process()->iotab[newfd] == NULL){
                break;
            }
        }
    }
    if(newfd >= PROCESS_IOMAX){                         //no fds available or bad input
        return -EBADFD;
    }
    if(current_process()->iotab[newfd] != NULL){        //if fd being used, close it before changing
        ioclose(current_process()->iotab[newfd]);
    }
    current_process()->iotab[newfd] = ioaddref(current_process()->iotab[oldfd]);        //add io reference to newfd
    return 0;
}