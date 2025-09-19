// thread.c - Threads
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef THREAD_TRACE
#define TRACE
#endif

#ifdef THREAD_DEBUG
#define DEBUG
#endif

#include "thread.h"

#include <stddef.h>
#include <stdint.h>

#include "assert.h"
#include "heap.h"
#include "string.h"
#include "riscv.h"
#include "intr.h"
#include "memory.h"
#include "error.h"
#include "process.h" 

#include <stdarg.h>

// COMPILE-TIME PARAMETERS
//

// NTHR is the maximum number of threads

#ifndef NTHR
#define NTHR 16
#endif

#ifndef STACK_SIZE
#define STACK_SIZE 4000
#endif

// EXPORTED GLOBAL VARIABLES
//

char thrmgr_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

enum thread_state {
    THREAD_UNINITIALIZED = 0,
    THREAD_WAITING,
    THREAD_RUNNING,
    THREAD_READY,
    THREAD_EXITED
};

struct thread_context {
    uint64_t s[12];
    void * ra;
    void * sp;
};

struct thread_stack_anchor {
    struct thread * ktp;
    void * kgp;
};



struct thread {
    struct thread_context ctx;  // must be first member (thrasm.s)
    int id; // index into thrtab[]
    //struct process *proc;
    enum thread_state state;
    const char * name;
    struct thread_stack_anchor * stack_anchor;
    void * stack_lowest;
    struct thread * parent;
    struct thread * list_next;
    struct condition * wait_cond;
    struct condition child_exit;
    struct lock *lock_list; //lst of locks acquired by this particular thrd (mp3)
    struct process *proc;
    
};

// INTERNAL MACRO DEFINITIONS
// 

// Pointer to running thread, which is kept in the tp (x4) register.

#define TP ((struct thread*)__builtin_thread_pointer())

// Macro for changing thread state. If compiled for debugging (DEBUG is
// defined), prints function that changed thread state.

#define set_thread_state(t,s) do { \
    debug("Thread <%s:%d> state changed from %s to %s by <%s:%d> in %s", \
        (t)->name, (t)->id, \
        thread_state_name((t)->state), \
        thread_state_name(s), \
        TP->name, TP->id, \
        __func__); \
    (t)->state = (s); \
} while (0)

// INTERNAL FUNCTION DECLARATIONS
//

// Initializes the main and idle threads. called from threads_init().

static void init_main_thread(void);
static void init_idle_thread(void);

// Sets the RISC-V thread pointer to point to a thread.

static void set_running_thread(struct thread * thr);

// Returns a string representing the state name. Used by debug and trace
// statements, so marked unused to avoid compiler warnings.

static const char * thread_state_name(enum thread_state state)
    __attribute__ ((unused));

// void thread_reclaim(int tid)
//
// Reclaims a thread's slot in thrtab and makes its parent the parent of its
// children. Frees the struct thread of the thread.

static void thread_reclaim(int tid);

// struct thread * create_thread(const char * name)
//
// Creates and initializes a new thread structure. The new thread is not added
// to any list and does not have a valid context (_thread_switch cannot be
// called to switch to the new thread).

static struct thread * create_thread(const char * name);

// void running_thread_suspend(void)
// Suspends the currently running thread and resumes the next thread on the
// ready-to-run list using _thread_swtch (in threasm.s). Must be called with
// interrupts enabled. Returns when the current thread is next scheduled for
// execution. If the current thread is TP, it is marked READY and placed
// on the ready-to-run list. Note that running_thread_suspend will only return if the
// current thread becomes READY.

static void running_thread_suspend(void);

// The following functions manipulate a thread list (struct thread_list). Note
// that threads form a linked list via the list_next member of each thread
// structure. Thread lists are used for the ready-to-run list (ready_list) and
// for the list of waiting threads of each condition variable. These functions
// are not interrupt-safe! The caller must disable interrupts before calling any
// thread list function that may modify a list that is used in an ISR.

static void tlclear(struct thread_list * list);
static int tlempty(const struct thread_list * list);
static void tlinsert(struct thread_list * list, struct thread * thr);
static struct thread * tlremove(struct thread_list * list);
//static void tlappend(struct thread_list * l0, struct thread_list * l1);

static void idle_thread_func(void);

// IMPORTED FUNCTION DECLARATIONS
// defined in thrasm.s
//

extern struct thread * _thread_swtch(struct thread * thr);

extern void _thread_startup(void);

void *running_thread_ktp_anchor(void) {
    return (void *)((uintptr_t)TP->stack_anchor - sizeof(struct trap_frame));
    //return (void *)((uintptr_t)TP->stack_anchor);
    kprintf("Child trap frame jump anchor: %p\n", running_thread_ktp_anchor());
}


// INTERNAL GLOBAL VARIABLES
//

#define MAIN_TID 0
#define IDLE_TID (NTHR-1)

static struct thread main_thread;
static struct thread idle_thread;

extern char _main_stack_lowest[]; // from start.s
extern char _main_stack_anchor[]; // from start.s

static struct thread main_thread = {
    .id = MAIN_TID,
    .name = "main",
    .state = THREAD_RUNNING,
    .stack_anchor = (void*)_main_stack_anchor,
    .stack_lowest = _main_stack_lowest,
    .child_exit.name = "main.child_exit"
};

extern char _idle_stack_lowest[]; // from thrasm.s
extern char _idle_stack_anchor[]; // from thrasm.s

static struct thread idle_thread = {
    .id = IDLE_TID,
    .name = "idle",
    .state = THREAD_READY,
    .parent = &main_thread,
    .stack_anchor = (void*)_idle_stack_anchor,
    .stack_lowest = _idle_stack_lowest,
    .ctx.sp = _idle_stack_anchor,
    .ctx.ra = &_thread_startup,
   // .ctx.s[10] = (uint64_t)(uintptr_t)idle_thread_func  //entry fn 
    // FIXME your code goes here
    .ctx.s[8] = (uint64_t)idle_thread_func,         //set the same as thread_spawn
    .ctx.s[9] = (uint64_t)thread_exit
};

static struct thread * thrtab[NTHR] = {
    [MAIN_TID] = &main_thread,
    [IDLE_TID] = &idle_thread
};

static struct thread_list ready_list = {
    .head = &idle_thread,
    .tail = &idle_thread
};

// EXPORTED FUNCTION DEFINITIONS
//


int running_thread(void) {
    return TP->id;
}

void thrmgr_init(void) {
    trace("%s()", __func__);
    init_main_thread();
    init_idle_thread();
    set_running_thread(&main_thread);
    thrmgr_initialized = 1;
}

///////////////////////////////////////////////////////////////////////////////
// int thread_spawn(const char *name, void (*entry)(void), ...)
// Inputs: const char *name - name of the new thread  
//         void (*entry)(void) - function the thread will start executing  
//         ... - up to 8 arguments to pass to the new thread  
// Outputs: int - thread ID of the newly created thread, or -EMTHR if failed  
// Desc:  
// - Creates a new thread with the given name and sets it to THREAD_READY.  
// - Adds the new thread to the ready list so the scheduler can pick it up.  
// - Initializes the thread’s child_exit condition var so thread_join() can wait on it.  
// - Sets up the thread’s stack pointer (sp) and return address (ra) to start at _thread_startup.  
// - Takes up to 8 args and stores them in the new thread’s saved registers.  
// - The thread will start running entry() when scheduled.  
// Side Effects: Allocates memory for the new thread, modifies the ready queue. 
///////////////////////////////////////////////////////////////////////////////
int thread_spawn (
    const char * name,
    void (*entry)(void),
    ...)
{
    struct thread * child;
    va_list ap;
    int pie;
    int i;

    child = create_thread(name);
    if (child == NULL)return -EMTHR;

    set_thread_state(child, THREAD_READY);
    pie = disable_interrupts();
    tlinsert(&ready_list, child);
    restore_interrupts(pie);

    // FIXME your code goes here
    condition_init(&child->child_exit, "child_exit");
    child->wait_cond = NULL;
    child->ctx.sp = (void *)((uintptr_t)child->stack_anchor); //sp top of stack 
    child->ctx.ra = (void *) &_thread_startup;  // ra to startup
    // filling in entry function arguments is given below, the rest is up to you

    va_start(ap, entry);
    for (i = 0; i < 8; i++)
        child->ctx.s[i] = va_arg(ap, uint64_t);
    va_end(ap);

    //child->ctx.s[10] = (uint_fast64_t)entry; //storing entry fn
    child->ctx.s[8] = (uint64_t)entry;
    child->ctx.s[9] = (uint64_t)thread_exit;                                      //thread startup called during first thread switch
    child->ctx.ra = &_thread_startup;                                   //first switch: thread_switch->thread_startup->entry->exit
    child->ctx.sp = child->stack_anchor;
    return child->id;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////
// void thread_exit(void)  
// Inputs: None  
// Outputs: None (never returns)  
// Desc:  
//thread exit basically terminates the current thread by first checking if the main thread is exiting. 
//If yes, it halts the system. else, it just marks the thread as exited and signal any parent waiting for it. 
//then we also call swtch to another thread n suspend. Lastly, halt failure just for safety. Also, we 
//disable n enable inttrpts before changing states. 
// Side Effects: Removes the thread from execution, signals the parent, may trigger a context switch.  
////////////////////////////////////////////////////////////////////////////////////////////////////////
void thread_exit(void) {
    // FIXME your code goes here
    while (TP->lock_list != NULL) {
        struct lock *l = TP->lock_list;
        TP->lock_list = l->next;
        
        l->owner = NULL;
        l->count = 0;
        condition_broadcast(&l->cv);  // Wake all waiters
    }
    if (TP->id == MAIN_TID) halt_success(); 
    set_thread_state(TP, THREAD_EXITED); //mark exit
    // condition_broadcast(&TP->parent->child_exit);//change tp->child to tp->parent->child
    condition_broadcast(&TP->child_exit); 
    running_thread_suspend(); //suspend exec
    halt_failure(); //wudnt reach this
}

void thread_yield(void) {
    trace("%s() in <%s:%d>", __func__, TP->name, TP->id);
    running_thread_suspend();
}
////////////////////////////////////////////////////////////////////////////////////////////////
// int thread_join(int tid)
// Inputs: int tid - thread ID of the child we waiting on (0 means wait for any child)  
// Outputs: int - ID of the exited child, or -EINVAL if tid is bad  
// Desc:  
// - If tid > 0, we wait only for that specific child to exit
// - If tid == 0, we just wait for any child to exit, then find the first one that did
// - Uses condition_wait(&TP->child_exit) so parent sleeps until a child actually exits
// - After waking up, it double check the child is exited before continuing
// - Once we **confirm** the child is exited, we call thread_reclaim() to clean it up
// - The child itself, in thread_exit, signals the parent when it’s done so the parent wakes up 
// Side Effects: Blocks parent thread until the child exits, frees up child resources so no mem leaks
////////////////////////////////////////////////////////////////////////////////////////////////
int thread_join(int tid) {
    struct thread *child = NULL;  // Pointer to the target child thread
    int pie; // Stores interrupt state for safe disabling/restoring


    // Validate 'tid': Ensure it is within the valid range and is actually a child.
    if (tid > 0 && tid < NTHR && (child = thrtab[tid]) && child->parent != TP) {
        return -EINVAL;
    }

    // If tid == 0, find ANY child thread of the calling thread.
    if (tid == 0) {
        for (int i = 0; i < NTHR; i++) {
            if (thrtab[i] && thrtab[i]->parent == TP) {
                child = thrtab[i];
                break; // Stop at the first found child
            }
        }
        if (child == NULL) {
            return -EINVAL;
        }
        tid = child->id;  // Update 'tid' to the found child's ID
    }

    // Wait for the child to exit.
    while(child->state != THREAD_EXITED) {
        condition_wait(&(child->child_exit));
    }

    // Reclaim resources safely with interrupts disabled.
    pie = disable_interrupts();
    thread_reclaim(tid);
    restore_interrupts(pie);

    return tid;  // Return the ID of the joined thread.
}


const char * thread_name(int tid) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    return thrtab[tid]->name;
}

const char * running_thread_name(void) {
    return TP->name;
}

void condition_init(struct condition * cond, const char * name) {
    tlclear(&cond->wait_list);
    cond->name = name;
}

void condition_wait(struct condition * cond) {
    int pie;

    assert(TP->state == THREAD_RUNNING);

    // Insert current thread into condition wait list
    
    set_thread_state(TP, THREAD_WAITING);
    TP->wait_cond = cond;
    TP->list_next = NULL;

    pie = disable_interrupts();
    tlinsert(&cond->wait_list, TP);
    restore_interrupts(pie);
    running_thread_suspend();
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// condition_broadcast(struct condition *cond): wakes up all threads waiting on a condition var
// - loops thru wait list and moves all threads to ready queue
// - clears their wait condition since they’re no longer waiting
// - disables intrpts to avoid race conditions while modifying lists
// - enables intrpts back after updates are done
////////////////////////////////////////////////////////////////////////////////////////////////////
void condition_broadcast(struct condition * cond) {
    struct thread *thr;
    int count = 0;
    int pie = disable_interrupts(); // Disable interrupts while modifying list.
    while ((thr = tlremove(&cond->wait_list)) != NULL) {
        set_thread_state(thr, THREAD_READY); // Mark thread as READY.
        thr->wait_cond = NULL;
        tlinsert(&ready_list, thr); // Insert into ready list.
        count++;
    }
    restore_interrupts(pie);
}

// INTERNAL FUNCTION DEFINITIONS
//

void init_main_thread(void) {
    // Initialize stack anchor with pointer to self
    main_thread.stack_anchor->ktp = &main_thread;
}

void init_idle_thread(void) {
    // Initialize stack anchor with pointer to self
    idle_thread.stack_anchor->ktp = &idle_thread;
}

static void set_running_thread(struct thread * thr) {
    asm inline ("mv tp, %0" :: "r"(thr) : "tp");
}

const char * thread_state_name(enum thread_state state) {
    static const char * const names[] = {
        [THREAD_UNINITIALIZED] = "UNINITIALIZED",
        [THREAD_WAITING] = "WAITING",
        [THREAD_RUNNING] = "RUNNING",
        [THREAD_READY] = "READY",
        [THREAD_EXITED] = "EXITED"
    };

    if (0 <= (int)state && (int)state < sizeof(names)/sizeof(names[0]))
        return names[state];
    else
        return "UNDEFINED";
};

void thread_reclaim(int tid) {
    struct thread * const thr = thrtab[tid];
    int ctid;

    assert (0 < tid && tid < NTHR && thr != NULL);
    assert (thr->state == THREAD_EXITED);

    // Make our parent thread the parent of our child threads. We need to scan
    // all threads to find our children. We could keep a list of all of a
    // thread's children to make this operation more efficient.

    for (ctid = 1; ctid < NTHR; ctid++) {
        if (thrtab[ctid] != NULL && thrtab[ctid]->parent == thr)
            thrtab[ctid]->parent = thr->parent;
    }

    thrtab[tid] = NULL;
    kfree(thr);
}

struct thread * create_thread(const char * name) {
    struct thread_stack_anchor * anchor;
    void * stack_page;
    struct thread * thr;
    int tid;

    trace("%s(name=\"%s\") in <%s:%d>", __func__, name, TP->name, TP->id);

    // Find a free thread slot.

    tid = 0;
    while (++tid < NTHR)
        if (thrtab[tid] == NULL)
            break;
    
    if (tid == NTHR)
        return NULL;
    
    // Allocate a struct thread and a stack

    thr = kcalloc(1, sizeof(struct thread));
    
    //stack_page = kmalloc(STACK_SIZE);
    stack_page = alloc_phys_page();                 //cp3, allocate full page for stack
    anchor = stack_page + PAGE_SIZE;
    anchor -= 1; // anchor is at base of stack
    thr->stack_lowest = stack_page;
    thr->stack_anchor = anchor;
    anchor->ktp = thr;
    anchor->kgp = NULL;

    thrtab[tid] = thr;

    thr->id = tid;
    thr->name = name;
    thr->parent = TP;
    return thr;
}

///////////////////////////////////////////////////////////////////////////////////////////////
// switch_running_thread(void): swaps out the current thread for the next ready one
// - if the curr thread is still runnable, we throw it back in the ready queue
// - if it's exited, we clean it up w/ thread_reclaim (which frees stack via kfree)
// - if no ready threads, we default to idle thread
// - uses _thread_swtch to actually swap context
// - disable/enable intrpts to keep scheduler updates atomic
// - this fn doesn’t return normally, we resume when this thread is next picked to run
////////////////////////////////////////////////////////////////////////////////////////////////

void running_thread_suspend(void) {
    struct thread *nextthrd, *currthrd;
    int pie;
    pie = disable_interrupts();
    currthrd = TP; // Current running thread.

    if (currthrd->state == THREAD_RUNNING) {
        set_thread_state(currthrd, THREAD_READY);
        tlinsert(&ready_list, currthrd);
    }
    nextthrd = tlremove(&ready_list); // Pick next thread.
    if (!nextthrd) {
        nextthrd = &idle_thread;
    }
    set_thread_state(nextthrd, THREAD_RUNNING);
    if (!nextthrd->proc || nextthrd->proc->mtag == main_mtag) {
        reset_active_mspace();              // -> csrw_satp(main_mtag) + sfence
    } else {
        switch_mspace(nextthrd->proc->mtag);// -> csrrw_satp(child_mtag) + sfence
    }
    struct thread* old_thr = _thread_swtch(nextthrd); // Switch context.
    restore_interrupts(pie);
    if (old_thr->state == THREAD_EXITED) {
        // kfree(old_thr->stack_lowest); // Free its stack & resources.
        free_phys_page(old_thr->stack_lowest);
    }
}


void tlclear(struct thread_list * list) {
    list->head = NULL;
    list->tail = NULL;
}

int tlempty(const struct thread_list * list) {
    return (list->head == NULL);
}

void tlinsert(struct thread_list * list, struct thread * thr) {
    thr->list_next = NULL;

    if (thr == NULL)
        return;

    if (list->tail != NULL) {
        assert (list->head != NULL);
        list->tail->list_next = thr;
    } else {
        assert(list->head == NULL);
        list->head = thr;
    }

    list->tail = thr;
}

struct thread * tlremove(struct thread_list * list) {
    struct thread * thr;

    thr = list->head;
    
    if (thr == NULL)
        return NULL;

    list->head = thr->list_next;
    
    if (list->head != NULL)
        thr->list_next = NULL;
    else
        list->tail = NULL;

    thr->list_next = NULL;
    return thr;
}

// Appends elements of l1 to the end of l0 and clears l1.

// void tlappend(struct thread_list * l0, struct thread_list * l1) {
//     if (l0->head != NULL) {
//         assert(l0->tail != NULL);
        
//         if (l1->head != NULL) {
//             assert(l1->tail != NULL);
//             l0->tail->list_next = l1->head;
//             l0->tail = l1->tail;
//         }
//     } else {
//         assert(l0->tail == NULL);
//         l0->head = l1->head;
//         l0->tail = l1->tail;
//     }

//     l1->head = NULL;
//     l1->tail = NULL;
// }

void idle_thread_func(void) {
    // The idle thread sleeps using wfi if the ready list is empty. Note that we
    // need to disable interrupts before checking if the thread list is empty to
    // avoid a race condition where an ISR marks a thread ready to run between
    // the call to tlempty() and the wfi instruction.

    for (;;) {
        // If there are runnable threads, yield to them.

        while (!tlempty(&ready_list))
            thread_yield();
        
        // No runnable threads. Sleep using the wfi instruction. Note that we
        // need to disable interrupts and check the runnable thread list one
        // more time (make sure it is empty) to avoid a race condition where an
        // ISR marks a thread ready before we call the wfi instruction.

        disable_interrupts();
        if (tlempty(&ready_list))
            asm ("wfi");
        enable_interrupts();
    }
}

void lock_init(struct lock *lock) {
    lock->owner = NULL;
    lock->count = 0;
    lock->next = NULL;
    condition_init(&lock->cv, "lock_cv");
}

void lock_acquire(struct lock *lock) {
    int pie = disable_interrupts();
    
    if (lock->owner == TP) { //extra case where we own the lock
        lock->count++;
        restore_interrupts(pie);
        return;
    }

    while (lock->owner != NULL) condition_wait(&lock->cv); //wait for free

    lock->owner = TP; //aquire
    lock->count = 1;
    
    lock->next = TP->lock_list; //add to list
    TP->lock_list = lock;
    
    restore_interrupts(pie);
}

void lock_release(struct lock *lock) {
    int pie = disable_interrupts();
    
    if (lock->owner != TP) { //do we own the lock?
        restore_interrupts(pie);
        return;  
    }

    if (--lock->count > 0) { //decrement
        restore_interrupts(pie);
        return;
    }

    struct lock **p = &TP->lock_list; //rem from list
    while (*p != lock) {
        p = &(*p)->next;
    }
    *p = lock->next;
    lock->next = NULL;

    lock->owner = NULL; //release n wake
    condition_broadcast(&lock->cv);
    
    restore_interrupts(pie);
}
/////////////////////////
struct process *thread_process(int tid) {
    if (tid < 0 || tid >= NTHR || thrtab[tid] == NULL)
        return NULL;
    return thrtab[tid]->proc;
}
struct process *running_thread_process(void) {
    //kprintf("%p\n", TP);
    return TP->proc;
}
void thread_set_process(int tid, struct process *proc) {
    if (tid >= 0 && tid < NTHR && thrtab[tid] != NULL)
        thrtab[tid]->proc = proc;
}
struct thread *running_thread_ptr(void){
    return TP;
}