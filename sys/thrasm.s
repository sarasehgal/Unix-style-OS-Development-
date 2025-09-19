# thrasm.s - Special functions called from thread.c
#
# Copyright (c) 2024-2025 University of Illinois
# SPDX-License-identifier: NCSA
#

# struct thread * _thread_swtch(struct thread * resuming_thread)

# Switches from the currently running thread to another thread and returns when
# the current thread is scheduled to run again. Argument /resuming_thread/ is
# the thread to be resumed. Returns a pointer to the previously-scheduled
# thread. This function is called in thread.c. The spelling of swtch is
# historic.

        .text
        .global _thread_swtch
        .type   _thread_swtch, @function

_thread_swtch:

        # We only need to save the ra and s0 - s12 registers. Save them on
        # the stack and then save the stack pointer. Our declaration is:
        # 
        #   struct thread * _thread_swtch(struct thread * resuming_thread);
        #
        # The currently running thread is suspended and resuming_thread is
        # restored to execution. swtch returns when execution is switched back
        # to the calling thread. The return value is the previously executing
        # thread. Interrupts are enabled when swtch returns.
        #
        # tp = pointer to struct thread of current thread (to be suspended)
        # a0 = pointer to struct thread of thread to be resumed
        # 
        # The first element of struct thread is:
        #
        #     struct thread_context {
        #         uint64_t s[12];
        #         void (*ra)(uint64_t);
        #         void * sp;
        #     };
        #

        sd      s0, 0*8(tp)
        sd      s1, 1*8(tp)
        sd      s2, 2*8(tp)
        sd      s3, 3*8(tp)
        sd      s4, 4*8(tp)
        sd      s5, 5*8(tp)
        sd      s6, 6*8(tp)
        sd      s7, 7*8(tp)
        sd      s8, 8*8(tp)
        sd      s9, 9*8(tp)
        sd      s10, 10*8(tp)
        sd      s11, 11*8(tp)
        sd      ra, 12*8(tp)
        sd      sp, 13*8(tp)

        mv t0, tp 
        mv tp, a0
        mv a0, t0 

        ld      sp, 13*8(tp)
        ld      ra, 12*8(tp)
        ld      s11, 11*8(tp)
        ld      s10, 10*8(tp)
        ld      s9, 9*8(tp)
        ld      s8, 8*8(tp)
        ld      s7, 7*8(tp)
        ld      s6, 6*8(tp)
        ld      s5, 5*8(tp)
        ld      s4, 4*8(tp)
        ld      s3, 3*8(tp)
        ld      s2, 2*8(tp)
        ld      s1, 1*8(tp)
        ld      s0, 0*8(tp)
                
        ret

        .global _thread_startup
        .type   _thread_startup, @function
##########################################################################################
# _thread_startup
# inputs: none (registers contain args and function pointer)  
# Outputs: none (never returns)  
# Desc:  
# this is the entry point for a new thread when it first starts running 
# the function arguments (if any) are stored in saved registers (s0-s7)  
# mv args from saved registers into a0-a7, which are used for function calls  
# mv the function pointer from s10 into t0  
# calls the function (entry point) using jalr t0, effectively jumping into it 
# - if the function ever returns, thread_exit is called to properly terminate the thread  
# Side Effects: Transfers execution to the thread's function, never returns to caller 
##########################################################################################
_thread_startup:
        # FIXME your code goes here
        # mv      a0, s0        # Load fn args into a regs
        # mv      a1, s1
        # mv      a2, s2
        # mv      a3, s3
        # mv      a4, s4
        # mv      a5, s5
        # mv      a6, s6
        # mv      a7, s7
        # mv      t0, s10

        # jalr    t0            # fn jump 
        # call thread_exit

        mv a0, s0                       #load arguments into a registers
        mv a1, s1
        mv a2, s2
        mv a3, s3
        mv a4, s4
        mv a5, s5
        mv a6, s6
        mv a7, s7

        jalr ra, s8, 0;                  #call entry, return here

        mv ra, s9                       #thread exit
        ret
        
# Statically allocated stack for the idle thread.

        .section        .data.stack, "wa", @progbits
        .balign          16
        
        .equ            IDLE_STACK_SIZE, 4096

        .global         _idle_stack_lowest
        .type           _idle_stack_lowest, @object
        .size           _idle_stack_lowest, IDLE_STACK_SIZE

        .global         _idle_stack_anchor
        .type           _idle_stack_anchor, @object
        .size           _idle_stack_anchor, 2*8

_idle_stack_lowest:
        .fill   IDLE_STACK_SIZE, 1, 0xBB

_idle_stack_anchor:
        .dword  0 # ktp
        .dword  0 # kgp
        .end

