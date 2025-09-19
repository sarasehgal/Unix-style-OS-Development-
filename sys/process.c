// process.c - user process
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//



#ifdef PROCESS_TRACE
#define TRACE
#endif

#ifdef PROCESS_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "process.h"
#include "elf.h"
#include "fs.h"
#include "io.h"
#include "string.h"
#include "thread.h"
#include "riscv.h"
#include "trap.h"
#include "memory.h"
#include "heap.h"
#include "error.h"
#include "intr.h"

// COMPILE-TIME PARAMETERS
//


#ifndef NPROC
#define NPROC 16
#endif

// INTERNAL FUNCTION DECLARATIONS
//


static int build_stack(void * stack, int argc, char ** argv);


static void fork_func(struct condition * forked, struct trap_frame * tfr);

// INTERNAL GLOBAL VARIABLES
//


static struct process main_proc;


static struct process * proctab[NPROC] = {
    &main_proc
};

// EXPORTED GLOBAL VARIABLES
//

char procmgr_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

void procmgr_init(void) {
    assert (memory_initialized && heap_initialized);
    assert (!procmgr_initialized);
    main_proc.idx = 0;
    main_proc.tid = running_thread();
    main_proc.mtag = active_mspace();
    thread_set_process(main_proc.tid, &main_proc);
    procmgr_initialized = 1;

    start_interrupter();        //enable preemptive multitasking
}
int process_exec(struct io *exeio, int argc, char **argv) {
    uintptr_t a1val;
    void *stack;
    void (*entry)(void);
    struct trap_frame *tfr;
    struct process *proc = running_thread_process();
 
    //reset_active_mspace();            //cp2
    discard_active_mspace();            //cp3
    if (elf_load(exeio, &entry) != 0)return -EINVAL;
    stack = alloc_and_map_range(UMEM_END_VMA - PAGE_SIZE, PAGE_SIZE, MAP_RWUG);
    if (!stack) return -ENOMEM;
    int stksz = build_stack(stack, argc, argv);
    if (stksz < 0)return stksz;

    tfr = kmalloc(sizeof(struct trap_frame));
    if (!tfr)return -ENOMEM;

    memset(tfr, 0, sizeof(struct trap_frame));
    tfr->a0 = argc;
    a1val = UMEM_END_VMA - PAGE_SIZE + (PAGE_SIZE - stksz);
    tfr->a1 = a1val;   // argv pointer for user
    tfr->sp = (void*)a1val;
    tfr->sepc = (void *)entry;       // entry point
    tfr->sstatus = csrr_sstatus();   // enable user mode
    tfr->sstatus &= ~RISCV_SSTATUS_SPP;
    tfr->sstatus |= RISCV_SSTATUS_SPIE;
    tfr->tp = running_thread_ptr();
    proc->mtag = active_mspace();
    proc->tid = running_thread();
    trap_frame_jump(tfr, running_thread_ktp_anchor());
}


void process_exit(void) {
    struct process *proc = running_thread_process();
    //if (!proc) panic("process_exit: no current process");
    if (proc->tid == 0)
        panic("Main process exited");
    for (int i = 0; i < PROCESS_IOMAX; i++) {
        if (proc->iotab[i]) {
            ioclose(proc->iotab[i]);
            proc->iotab[i] = NULL;
        }
    }
    proctab[proc->idx] = NULL;
    kfree(proc);
    thread_exit();  // 
}

// INTERNAL FUNCTION DEFINITIONS
//

int build_stack(void * stack, int argc, char ** argv) {
    size_t stksz, argsz;
    uintptr_t * newargv;
    char * p;
    int i;

    // We need to be able to fit argv[] on the initial stack page, so _argc_
    // cannot be too large. Note that argv[] contains argc+1 elements (last one
    // is a NULL pointer).

    if (PAGE_SIZE / sizeof(char*) - 1 < argc)
        return -ENOMEM;
    
    stksz = (argc+1) * sizeof(char*);

    // Add the sizes of the null-terminated strings that argv[] points to.

    for (i = 0; i < argc; i++) {
        argsz = strlen(argv[i])+1;
        if (PAGE_SIZE - stksz < argsz)
            return -ENOMEM;
        stksz += argsz;
    }

    // Round up stksz to a multiple of 16 (RISC-V ABI requirement).

    stksz = ROUND_UP(stksz, 16);
    assert (stksz <= PAGE_SIZE);

    // Set _newargv_ to point to the location of the argument vector on the new
    // stack and set _p_ to point to the stack space after it to which we will
    // copy the strings. Note that the string pointers we write to the new
    // argument vector must point to where the user process will see the stack.
    // The user stack will be at the highest page in user memory, the address of
    // which is `(UMEM_END_VMA - PAGE_SIZE)`. The offset of the _p_ within the
    // stack is given by `p - newargv'.

    newargv = stack + PAGE_SIZE - stksz;
    p = (char*)(newargv+argc+1);

    for (i = 0; i < argc; i++) {
        newargv[i] = (UMEM_END_VMA - PAGE_SIZE) + ((void*)p - (void*)stack);
        argsz = strlen(argv[i])+1;
        memcpy(p, argv[i], argsz);
        p += argsz;
    }

    newargv[argc] = 0;
    return stksz;
}

// Forks a child process.

// Creates a new process struct for the child, copies the parent's I/O objects and spawns a new thread for the child. 
// The child thread uses the parent's trap frame to return to U mode, signaling the parent that it is done with the trap frame.
int process_fork(const struct trap_frame * tfr) {
    if (!tfr) return -EINVAL;  // Sanity check for null trap frame

    struct process *child = kcalloc(1, sizeof(struct process));
    if (!child) return -ENOMEM;

    // Find a free slot in the process table and assign the process
    int idx = -1;
    for (int i = 0; i < NPROC; i++) {
        if (proctab[i] == NULL) {
            idx = i;
            proctab[i] = child;
            child->idx = i;
            break;
        }
    }
    if (idx < 0) return -EMPROC;  // No available process slot

    // Clone the current memory space for the child
    child->mtag = clone_active_mspace();
    if (!child->mtag) return -ENOMEM;

    // Copy I/O table from parent
    struct process *parent = running_thread_process();
    if (!parent) return -EINVAL;
    for (int i = 0; i < PROCESS_IOMAX; i++) {
        if (parent->iotab[i])
            child->iotab[i] = ioaddref(parent->iotab[i]);
    }

    // Allocate a condition variable on the heap for parent-child sync
    struct condition *done = kmalloc(sizeof(struct condition));
    if (!done) return -ENOMEM;
    condition_init(done, "fork_done");

    // Spawn the child thread with fork_func
    int tid = thread_spawn("forked", (void (*)(void))fork_func, done, tfr);
    if (tid < 0) {
        kfree(done);
        return tid;
    }
    // Link the thread to its process before it runs
    child->tid = tid;
    thread_set_process(tid, child);

    // Parent waits for child to finish copying trap frame
    condition_wait(done);
    kfree(done);

    return tid;
}

static void fork_func(struct condition *done, struct trap_frame *tfr) {
    if (!done || !tfr) halt_failure();
    struct trap_frame child_tfr = *tfr;
    child_tfr.a0 = 0;  // fork returns 0 in the child
    child_tfr.tp = running_thread_ptr();  // update tp for the child thread
    condition_broadcast(done);  // Notify parent that copy is done
    trap_frame_jump(&child_tfr, running_thread_ktp_anchor());
}