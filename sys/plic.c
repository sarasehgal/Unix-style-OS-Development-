// plic.c - RISC-V PLIC
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef PLIC_TRACE
#define TRACE
#endif

#ifdef PLIC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "plic.h"
#include "assert.h"

#include <stdint.h>

// INTERNAL MACRO DEFINITIONS
//

// CTX(i,0) is hartid /i/ M-mode context
// CTX(i,1) is hartid /i/ S-mode context

#define CTX(i,s) (2*(i)+(s))

// INTERNAL TYPE DEFINITIONS
// 

struct plic_regs {
	union {
		uint32_t priority[PLIC_SRC_CNT];
		char _reserved_priority[0x1000];
	};

	union {
		uint32_t pending[PLIC_SRC_CNT/32];
		char _reserved_pending[0x1000];
	};

	union {
		uint32_t enable[PLIC_CTX_CNT][32];
		char _reserved_enable[0x200000-0x2000];
	};

	struct {
		union {
			struct {
				uint32_t threshold;
				uint32_t claim;
			};
			
			char _reserved_ctxctl[0x1000];
		};
	} ctx[PLIC_CTX_CNT];
};

#define PLIC (*(volatile struct plic_regs*)PLIC_MMIO_BASE)

// INTERNAL FUNCTION DECLARATIONS
//

static void plic_set_source_priority (
	uint_fast32_t srcno, uint_fast32_t level);
static int plic_source_pending(uint_fast32_t srcno);
static void plic_enable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);
static void plic_disable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);
static void plic_set_context_threshold (
	uint_fast32_t ctxno, uint_fast32_t level);
static uint_fast32_t plic_claim_context_interrupt (
	uint_fast32_t ctxno);
static void plic_complete_context_interrupt (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_enable_all_sources_for_context(uint_fast32_t ctxno);
static void plic_disable_all_sources_for_context(uint_fast32_t ctxno);

// We currently only support single-hart operation, sending interrupts to S mode
// on hart 0 (context 0). The low-level PLIC functions already understand
// contexts, so we only need to modify the high-level functions (plit_init,
// plic_claim_request, plic_finish_request)to add support for multiple harts.

// EXPORTED FUNCTION DEFINITIONS
// 

void plic_init(void) {
	int i;

	// Disable all sources by setting priority to 0

	for (i = 0; i < PLIC_SRC_CNT; i++)
		plic_set_source_priority(i, 0);
	
	// Route all sources to S mode on hart 0 only

	for (int i = 0; i < PLIC_CTX_CNT; i++)
		plic_disable_all_sources_for_context(i);
	
	plic_enable_all_sources_for_context(CTX(0,1));
}

extern void plic_enable_source(int srcno, int prio) {
	//trace("%s(srcno=%d,prio=%d)", __func__, srcno, prio);
	assert (0 < srcno && srcno <= PLIC_SRC_CNT);
	assert (prio > 0);

	plic_set_source_priority(srcno, prio);
}

extern void plic_disable_source(int irqno) {
	if (0 < irqno)
		plic_set_source_priority(irqno, 0);
	else
		debug("plic_disable_irq called with irqno = %d", irqno);
}

extern int plic_claim_interrupt(void) {
	// FIXME: Hardwired S-mode hart 0
	//trace("%s()", __func__);
	return plic_claim_context_interrupt(CTX(0,1));
}

extern void plic_finish_interrupt(int irqno) {
	// FIXME: Hardwired S-mode hart 0
	//trace("%s(irqno=%d)", __func__, irqno);
	plic_complete_context_interrupt(CTX(0,1), irqno);
}

// INTERNAL FUNCTION DEFINITIONS
//
////////////////////////////////////////////////////////////////////////////////
// uint_fast32_t plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level)
// Inputs: uint_fast32_t srcno - interrupt src to set prio for
//         uint_fast32_t level - priority level to set (higher = more urgent)
// Outputs: None
// Desc: Just sets prio level of a given interrupt src if valid, else does nothing
// Side Effects: Updates plic.priority[srcno]
////////////////////////////////////////////////////////////////////////////////
static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level) {
	// FIXME your code goes here
	if (srcno > PLIC_SRC_CNT || level > PLIC_PRIO_MAX) return;
	PLIC.priority[srcno] = level; //set level
}
////////////////////////////////////////////////////////////////////////////////
// int plic_source_pending(uint_fast32_t srcno)
// Inputs: uint_fast32_t srcno - interrupt src to check pending status for
// Outputs: int - 1 if pending, 0 if not
// Desc: checks if an interrupt src is waiting to be handled - by looking at the
//       right bit in plic.pendin n uses bitwise ops to extract status
// Side Effects: None
////////////////////////////////////////////////////////////////////////////////
static inline int plic_source_pending(uint_fast32_t srcno) {
	// FIXME your code goes here
	if ( srcno > PLIC_SRC_CNT) return 0;
	return (PLIC.pending[srcno / 32] >> (srcno % 32)) & 1;//find bitmask n set 1 or 0
}
////////////////////////////////////////////////////////////////////////////////
// void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno)
// Inputs: uint_fast32_t ctxno - which CPU/core context to enable intr for
//         uint_fast32_t srcno - interrupt src to enable
// Outputs: None
// Desc: finds the right reg using srcno / 32 then flips the bit using OR 
// Side Effects: Modifies PLIC.enable[ctxno] to allow this intr src
////////////////////////////////////////////////////////////////////////////////
static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno) {
	// FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT || srcno > PLIC_SRC_CNT) return;
	PLIC.enable[ctxno][srcno / 32] |= (1 << (srcno % 32));
}
////////////////////////////////////////////////////////////////////////////////
// void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid)
// Inputs: uint_fast32_t ctxno - which CPU/core context to disable intr for
//         uint_fast32_t srcid - interrupt src to disable
// Outputs: None
// Desc: Finds the right reg using srcid / 32, then clears the bit using AND 
// Side Effects: Modifies PLIC.enable[ctxno] to block this intr src
////////////////////////////////////////////////////////////////////////////////
static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid) {
	// FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT || srcid > PLIC_SRC_CNT) return;
	PLIC.enable[ctxno][srcid / 32] &= ~(1 << (srcid % 32));	//find bitmask n set 0
}
////////////////////////////////////////////////////////////////////////////////
// void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level)
// Inputs: uint_fast32_t ctxno - which CPU/core context to set threshold for
//         uint_fast32_t level - min prio an intr must have to be handled
// Outputs: None
// Desc: sets a threshold, so only intrs >= this prio get handled
// Side Effects: Modifies PLIC.ctx[ctxno].threshold
////////////////////////////////////////////////////////////////////////////////
static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level) {
	// FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT || level > PLIC_PRIO_MAX) return;
	PLIC.ctx[ctxno].threshold = level;//set level
}
////////////////////////////////////////////////////////////////////////////////
// uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno)
// Inputs: uint_fast32_t ctxno - CPU/core context trying to claim an interrupt
// Outputs: uint_fast32_t - srcno of highest prio pending intr, or 0 if none
// Desc: reads PLIC.ctx[ctxno].claim to fetch the highest prio intr that’s waiting
// Side Effects: Claiming an intr removes it from the pending list
////////////////////////////////////////////////////////////////////////////////
static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno) {
	// FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT) return 0;
	return PLIC.ctx[ctxno].claim;
}
////////////////////////////////////////////////////////////////////////////////
// void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno)
// Inputs: uint_fast32_t ctxno - CPU/core context completing an interrupt
//         uint_fast32_t srcno - interrupt src that was just handled
// Outputs: None
// Desc: writes `srcno` back to `PLIC.ctx[ctxno].claim` to mark it as done
// Side Effects: Frees up the PLIC to handle the next interrupt from this src
////////////////////////////////////////////////////////////////////////////////
static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno) {
	// FIXME your code goes here
	if ( ctxno >= PLIC_CTX_CNT || srcno > PLIC_SRC_CNT) return;
	PLIC.ctx[ctxno].claim = srcno;
}
////////////////////////////////////////////////////////////////////////////////
// void plic_enable_all_sources_for_context(uint_fast32_t ctxno)
// Inputs: uint_fast32_t ctxno - which CPU/core context to enable all intrs for
// Outputs: None
// Desc: loops thru all enable regs for this context and sets all bits to 1
// Side Effects: Allows all interrupts for this context
////////////////////////////////////////////////////////////////////////////////
static void plic_enable_all_sources_for_context(uint_fast32_t ctxno) {
	// FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT) return;
	for (uint_fast32_t i = 0; i < (PLIC_SRC_CNT / 32); i++) {
        PLIC.enable[ctxno][i] = 0xFFFFFFFF;  // Enable all intrrpt src
    }
}
////////////////////////////////////////////////////////////////////////////////
// void plic_disable_all_sources_for_context(uint_fast32_t ctxno)
// Inputs: uint_fast32_t ctxno - which CPU/core context to disable all intrs for
// Outputs: None
// Desc: loops thru all enable regs for this context and sets all bits to 0
// Side Effects: Blocks all interrupts for this context
////////////////////////////////////////////////////////////////////////////////
static void plic_disable_all_sources_for_context(uint_fast32_t ctxno) {
	// FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT) return;
	for (uint_fast32_t i = 0; i < (PLIC_SRC_CNT) / 32; i++) {
        PLIC.enable[ctxno][i] = 0x0;  // Disable all intrrpt src
    }
}
