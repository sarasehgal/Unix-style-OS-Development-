// timer.c
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef TIMER_TRACE
#define TRACE
#endif

#ifdef TIMER_DEBUG
#define DEBUG
#endif

#include "timer.h"
#include "thread.h"
#include "riscv.h"
#include "assert.h"
#include "intr.h"
#include "conf.h"
#include "see.h" // for set_stcmp

// EXPORTED GLOBAL VARIABLE DEFINITIONS
// 

char timer_initialized = 0;

// INTERNVAL GLOBAL VARIABLE DEFINITIONS
//

static struct alarm * sleep_list;

// INTERNAL FUNCTION DECLARATIONS
//

// EXPORTED FUNCTION DEFINITIONS
//

void timer_init(void) {
    set_stcmp(UINT64_MAX);
    timer_initialized = 1;
}
//////////////////////////////////////////////////////////////////
// void alarm_init(struct alarm *al, const char *name)  
// Inputs: struct alarm *al - alarm struct to initialize  
//         const char *name - name for the alarm (default: "alarm" if NULL)  
// Outputs: None  
// Desc:  
// basically just initializing- so we start w setting up an alarm with a condition variable for sleeping/waking  
// if no name is provided, assigns the default name alarm  
// sets the wake-up time twake to the current time rdtime
// initialize next to NULL since it's not in the sleep list yet  
// side Effects: Modifies alarm struct, initializes condition variable  
////////////////////////////////////////////////////////////////////////
void alarm_init(struct alarm * al, const char * name) {
    // FIXME your code goes here
    condition_init(&al->cond, name ? name : "alarm");
    al->twake = rdtime(); //curr tm in ticks
    al->next = NULL; //not in sleep list rn
}
////////////////////////////////////////////////////////////////////////////////
// void alarm_sleep(struct alarm *al, unsigned long long tcnt)
// Inputs: struct alarm *al - alarm struct to track sleep time  
//         unsigned long long tcnt - time to sleep (in ticks)  
// Outputs: None  
// Desc:  
// - makes the calling thread sleep for `tcnt` ticks using an alarm.  
// - First, checks if `tcnt` is 0, in which case we return there n then
// - if tcnt is very big and would cause an overflow, we then wud cap it at max value
// - if the wake time has already passed, return since no need to sleep
// - disables interrupts and inserts the alarm into the sleep list in sorted order
// - if this alarm is now the soonest one to wake up, update the timer (`mtimecmp`)
// - calls condition_wait() to put the thread to sleep until the alarm wakes it up
// - restores interrupts before sleeping and enables timer interrupts again 
// Side Effects: Puts the thread to sleep, modifies the sleep list, updates timer interrupts  
/////////////////////////////////////////////////////////////////////////////////////////////
void alarm_sleep(struct alarm *al, unsigned long long tcnt) {
    unsigned long long now = rdtime(); // get curr time
    struct alarm **curr;
    int saved_intr;

    if (tcnt == 0) return; // if time 0, no need to sleep
    al->twake = (UINT64_MAX - al->twake < tcnt) ? UINT64_MAX : al->twake + tcnt;// if tcnt too big n wraps around, cap it at max val
    if (al->twake < now) return;// if wake time already passed, just return
    saved_intr = disable_interrupts(); // turn off intr b4 modifying sleep list
    curr = &sleep_list; // insert into sleep list in sorted order
    while (*curr && (*curr)->twake < al->twake) curr = &(*curr)->next;
    al->next = *curr;
    *curr = al;
    // update mtimecmp if this alarm is now first in list
    if (sleep_list == al || al->twake < sleep_list->twake) set_stcmp(al->twake);
    condition_wait(&al->cond); // put thread to sleep
    restore_interrupts(saved_intr); // restore intr before sleeping
    csrs_sie(RISCV_SIE_STIE); // enable timer intr again
}

// Resets the alarm so that the next sleep increment is relative to the time
// alarm_reset is called.

void alarm_reset(struct alarm * al) {
    al->twake = rdtime();
}

void alarm_sleep_sec(struct alarm * al, unsigned int sec) {
    alarm_sleep(al, sec * TIMER_FREQ);
}

void alarm_sleep_ms(struct alarm * al, unsigned long ms) {
    alarm_sleep(al, ms * (TIMER_FREQ / 1000));
}

void alarm_sleep_us(struct alarm * al, unsigned long us) {
    alarm_sleep(al, us * (TIMER_FREQ / 1000 / 1000));
}

void sleep_sec(unsigned int sec) {
    sleep_ms(1000UL * sec);
}

void sleep_ms(unsigned long ms) {
    sleep_us(1000UL * ms);
}

void sleep_us(unsigned long us) {
    struct alarm al;

    alarm_init(&al, "sleep");
    alarm_sleep_us(&al, us);
}

// handle_timer_interrupt() is dispatched from intr_handler in intr.c
////////////////////////////////////////////////////////////////////////////////////////////
// void handle_timer_interrupt(void)  
// Inputs: None  
// Outputs: None  
// Desc:  
// handles timer interrupts by chcking for expired alarms
// if sleep_list is empty, return immediately -nothing to wake up 
// rds the current time rdtime
// disables interrupts and loops thru expired alarms, waking up threads 
// removes alarms from the sleep list as they are processed
// Updates mtimecmp for the next alarm, or disables timer interrupts if none left  
// Side Effects: Wakes up sleeping threads, updates sleep_list, modifies timer interrupts 
////////////////////////////////////////////////////////////////////////////////////////////
void handle_timer_interrupt(void) {
    struct alarm *head = sleep_list;
    struct alarm *next;
    uint64_t now;
    int pie;

    if (!head) return; // no alarms, nothing 
    now = rdtime(); // get curr tm
    pie = disable_interrupts(); // intr off before modifying list
    // process expired alarms
    while (head && head->twake <= now) {
        next = head->next;  // str next alarm
        condition_broadcast(&head->cond); // wake up thread waiting on this alarm
        sleep_list = next; // rem alarm from list
        head = next; // mv to next
    }
    // set mtimecmp for next alarm/disable timer if no alarms left
    if (sleep_list) set_stcmp(sleep_list->twake); // set nxt alarm tm
    else {
        set_stcmp(UINT64_MAX); // disable timer
        csrc_sie(RISCV_SIE_STIE); // turn off timer intr
    }
    restore_interrupts(pie); // intr back on
}