/*! @file memory.c
    @brief Physical and virtual memory manager    
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

#ifdef MEMORY_TRACE
#define TRACE
#endif

#ifdef MEMORY_DEBUG
#define DEBUG
#endif

#include "memory.h"
#include "conf.h"
#include "riscv.h"
#include "heap.h"
#include "console.h"
#include "assert.h"
#include "string.h"
#include "thread.h"
#include "process.h"
#include "error.h"

// COMPILE-TIME CONFIGURATION
//

// Minimum amount of memory in the initial heap block.

#ifndef HEAP_INIT_MIN
#define HEAP_INIT_MIN 256
#endif

// INTERNAL CONSTANT DEFINITIONS
//

#define MEGA_SIZE ((1UL << 9) * PAGE_SIZE) // megapage size
#define GIGA_SIZE ((1UL << 9) * MEGA_SIZE) // gigapage size

#define PTE_ORDER 3
#define PTE_CNT (1U << (PAGE_ORDER - PTE_ORDER))

#ifndef PAGING_MODE
#define PAGING_MODE RISCV_SATP_MODE_Sv39
#endif

#ifndef ROOT_LEVEL
#define ROOT_LEVEL 2
#endif

// IMPORTED GLOBAL SYMBOLS
//

// linker-provided (kernel.ld)
extern char _kimg_start[];
extern char _kimg_text_start[];
extern char _kimg_text_end[];
extern char _kimg_rodata_start[];
extern char _kimg_rodata_end[];
extern char _kimg_data_start[];
extern char _kimg_data_end[];
extern char _kimg_end[];

// EXPORTED GLOBAL VARIABLES
//

char memory_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

// We keep free physical pages in a linked list of _chunks_, where each chunk
// consists of several consecutive pages of memory. Initially, all free pages
// are in a single large chunk. To allocate a block of pages, we break up the
// smallest chunk on the list.

/**
 * @brief Section of consecutive physical pages. We keep free physical pages in a
 * linked list of chunks. Initially, all free pages are in a single large chunk. To
 * allocate a block of pages, we break up the smallest chunk in the list
 */
struct page_chunk {
    struct page_chunk * next; ///< Next page in list
    uintptr_t first_ppn;
    unsigned long pagecnt; ///< Number of pages in chunk
};

/**
 * @brief RISC-V PTE. RTDC (RISC-V docs) for what each of these fields means!
 */
struct pte {
    uint64_t flags:8;
    uint64_t rsw:2;
    uint64_t ppn:44;
    uint64_t reserved:7;
    uint64_t pbmt:2;
    uint64_t n:1;
};

// INTERNAL MACRO DEFINITIONS
//

#define VPN(vma) ((vma) / PAGE_SIZE)
#define VPN2(vma) ((VPN(vma) >> (2*9)) % PTE_CNT)
#define VPN1(vma) ((VPN(vma) >> (1*9)) % PTE_CNT)
#define VPN0(vma) ((VPN(vma) >> (0*9)) % PTE_CNT)

#define MIN(a,b) (((a)<(b))?(a):(b))

#define ROUND_UP(n,k) (((n)+(k)-1)/(k)*(k)) 
#define ROUND_DOWN(n,k) ((n)/(k)*(k))

// The following macros test is a PTE is valid, global, or a leaf. The argument
// is a struct pte (*not* a pointer to a struct pte).

#define PTE_VALID(pte) (((pte).flags & PTE_V) != 0)
#define PTE_GLOBAL(pte) (((pte).flags & PTE_G) != 0)
#define PTE_LEAF(pte) (((pte).flags & (PTE_R | PTE_W | PTE_X)) != 0)

#define PT_INDEX(lvl, vpn) (((vpn) & (0x1FF << (lvl * (PAGE_ORDER - PTE_ORDER)))) \
                             >> (lvl * (PAGE_ORDER - PTE_ORDER)))
// INTERNAL FUNCTION DECLARATIONS
//



static inline mtag_t active_space_mtag(void);
static inline mtag_t ptab_to_mtag(struct pte * root, unsigned int asid);
static inline struct pte * mtag_to_ptab(mtag_t mtag);
static inline struct pte * active_space_ptab(void);

static inline void * pageptr(uintptr_t n);
static inline uintptr_t pagenum(const void * p);
static inline int wellformed(uintptr_t vma);

static inline struct pte leaf_pte(const void * pp, uint_fast8_t rwxug_flags);
static inline struct pte ptab_pte(const struct pte * pt, uint_fast8_t g_flag);
static inline struct pte null_pte(void);
static inline uint16_t vpn_l2(uintptr_t v) { return (v >> 30) & 0x1FF; }
static inline uint16_t vpn_l1(uintptr_t v) { return (v >> 21) & 0x1FF; }
static inline uint16_t vpn_l0(uintptr_t v) { return (v >> 12) & 0x1FF; }
static inline struct pte *walk_create(uintptr_t vma, int alloc);


// INTERNAL GLOBAL VARIABLES
//

mtag_t main_mtag;

static struct pte main_pt2[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt1_0x80000[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt0_0x80000[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct page_chunk * free_chunk_list;

// EXPORTED FUNCTION DECLARATIONS
// 

void memory_init(void) {
    const void * const text_start = _kimg_text_start;
    const void * const text_end = _kimg_text_end;
    const void * const rodata_start = _kimg_rodata_start;
    const void * const rodata_end = _kimg_rodata_end;
    const void * const data_start = _kimg_data_start;
    
    void * heap_start;
    void * heap_end;

    uintptr_t pma;
    const void * pp;

    trace("%s()", __func__);

    assert (RAM_START == _kimg_start);

    kprintf("           RAM: [%p,%p): %zu MB\n",
        RAM_START, RAM_END, RAM_SIZE / 1024 / 1024);
    kprintf("  Kernel image: [%p,%p)\n", _kimg_start, _kimg_end);

    // Kernel must fit inside 2MB megapage (one level 1 PTE)
    
    if (MEGA_SIZE < _kimg_end - _kimg_start)
        panic(NULL);

    // Initialize main page table with the following direct mapping:
    // 
    //         0 to RAM_START:           RW gigapages (MMIO region)
    // RAM_START to _kimg_end:           RX/R/RW pages based on kernel image
    // _kimg_end to RAM_START+MEGA_SIZE: RW pages (heap and free page pool)
    // RAM_START+MEGA_SIZE to RAM_END:   RW megapages (free page pool)
    //
    // RAM_START = 0x80000000
    // MEGA_SIZE = 2 MB
    // GIGA_SIZE = 1 GB
    
    // Identity mapping of MMIO region as two gigapage mappings
    for (pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)
        main_pt2[VPN2(pma)] = leaf_pte((void*)pma, PTE_R | PTE_W | PTE_G);
    
    // Third gigarange has a second-level subtable
    main_pt2[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);

    // First physical megarange of RAM is mapped as individual pages with
    // permissions based on kernel image region.

    main_pt1_0x80000[VPN1(RAM_START_PMA)] = ptab_pte(main_pt0_0x80000, PTE_G);

    for (pp = text_start; pp < text_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_X | PTE_G);
    }

    for (pp = rodata_start; pp < rodata_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_G);
    }

    for (pp = data_start; pp < RAM_START + MEGA_SIZE; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Remaining RAM mapped in 2MB megapages

    for (pp = RAM_START + MEGA_SIZE; pp < RAM_END; pp += MEGA_SIZE) {
        main_pt1_0x80000[VPN1((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Enable paging; this part always makes me nervous.

    main_mtag = ptab_to_mtag(main_pt2, 0);
    csrw_satp(main_mtag);

    // Give the memory between the end of the kernel image and the next page
    // boundary to the heap allocator, but make sure it is at least
    // HEAP_INIT_MIN bytes.

    heap_start = _kimg_end;
    heap_end = (void*)ROUND_UP((uintptr_t)heap_start, PAGE_SIZE);

    if (heap_end - heap_start < HEAP_INIT_MIN) {
        heap_end += ROUND_UP (
            HEAP_INIT_MIN - (heap_end - heap_start), PAGE_SIZE);
    }

    if (RAM_END < heap_end)
        panic("out of memory");
    
    // Initialize heap memory manager

    heap_init(heap_start, heap_end);

    kprintf("Heap allocator: [%p,%p): %zu KB free\n",
        heap_start, heap_end, (heap_end - heap_start) / 1024);
    
    // TODO: Initialize the free chunk list here

    uintptr_t free_start = ROUND_UP((uintptr_t)heap_end, PAGE_SIZE);
    uintptr_t free_end   = (uintptr_t)RAM_END;

    if (free_end <= free_start)
        panic("no free RAM for page allocator");

    struct page_chunk *first =
        (struct page_chunk *)kmalloc(sizeof(struct page_chunk)); // use kmalloc or heap_alloc
    first->next    = NULL;
    first->pagecnt = (free_end - free_start) >> PAGE_ORDER;
    first->first_ppn = pagenum((void*)free_start);

    free_chunk_list = first;
    
    
    // Allow supervisor to access user memory. We could be more precise by only
    // enabling supervisor access to user memory when we are explicitly trying
    // to access user memory, and disable it at other times. This would catch
    // bugs that cause inadvertent access to user memory (due to bugs).

    csrs_sstatus(RISCV_SSTATUS_SUM);

    memory_initialized = 1;
}

// static struct pte *clone_root(void)
// {
//     struct pte *new_pt2 = alloc_phys_page();
//     if (!new_pt2) return NULL;
//     memset(new_pt2, 0, PAGE_SIZE);

//     /* Copy kernel half ([256..511]) so user part starts empty. */
//     memcpy(&new_pt2[256], &main_pt2[256],
//            (PTE_CNT - 256) * sizeof(struct pte));
//     return new_pt2;
// }

static struct pte *clone_root(void) {
    struct pte *old_l2 = active_space_ptab();
    struct pte *new_l2 = alloc_phys_page();
    if (!new_l2) return NULL;
    memset(new_l2, 0, PAGE_SIZE);

    int kern_idx = vpn_l2((uintptr_t)RAM_START);
    // Copy kernel half of L2 table
    memcpy(&new_l2[kern_idx],
           &old_l2[kern_idx],
           (PTE_CNT - kern_idx) * sizeof(struct pte));

    // Clone user half
    for (int i = 0; i < kern_idx; i++) {
        struct pte e = old_l2[i];
        if (!PTE_VALID(e) || PTE_LEAF(e))
            continue;

        struct pte *old_l1 = pageptr(e.ppn);
        struct pte *new_l1 = alloc_phys_page();
        if (!new_l1) continue;
        memset(new_l1, 0, PAGE_SIZE);

        for (int j = 0; j < PTE_CNT; j++) {
            struct pte le = old_l1[j];
            if (!PTE_VALID(le) || !PTE_LEAF(le)) continue;

            void *old_page = pageptr(le.ppn);
            void *new_page = alloc_phys_page();
            if (!new_page) continue;

            memcpy(new_page, old_page, PAGE_SIZE);

            new_l1[j] = leaf_pte(new_page,
                                 le.flags & (PTE_R|PTE_W|PTE_X|PTE_U|PTE_G));
        }

        new_l2[i] = ptab_pte(new_l1, e.flags & PTE_G);
    }

    return new_l2;
}


static inline mtag_t ptab_to_satp(struct pte *pt)
{
    return ptab_to_mtag(pt, 0);       /* ASID 0 for now */
}


mtag_t active_mspace(void) {
    return active_space_mtag();
}

mtag_t switch_mspace(mtag_t mtag) {
    mtag_t prev;
    
    prev = csrrw_satp(mtag);
    sfence_vma();
    return prev;
}

mtag_t clone_active_mspace(void) {
    struct pte *root = clone_root();
    return root ? ptab_to_satp(root) : 0;
}

void reset_active_mspace(void) {
    csrw_satp(main_mtag);
    sfence_vma();
}

mtag_t discard_active_mspace(void) {
    mtag_t cur = active_space_mtag();
    if (cur == main_mtag) return cur;           /* kernel space */

    struct pte *pt2 = mtag_to_ptab(cur);

    /* Walk user half and free subtables + leaves */
    for (int i = 0; i < 256; ++i) {
        if (!PTE_VALID(pt2[i]) || PTE_LEAF(pt2[i])) continue;
        struct pte *pt1 = pageptr(pt2[i].ppn);
        for (int j = 0; j < PTE_CNT; ++j) {
            if (!PTE_VALID(pt1[j]) || PTE_LEAF(pt1[j])) continue;
            free_phys_page(pageptr(pt1[j].ppn));
        }
        free_phys_page(pt1);
    }
    free_phys_page(pt2);

    reset_active_mspace();
    return main_mtag;
}

// The map_page() function maps a single page into the active address space at
// the specified address. The map_range() function maps a range of contiguous
// pages into the active address space. Note that map_page() is a special case
// of map_range(), so it can be implemented by calling map_range(). Or
// map_range() can be implemented by calling map_page() for each page in the
// range. The current implementation does the latter.

// We currently map 4K pages only. At some point it may be disirable to support
// mapping megapages and gigapages.

/* -------------------------------------------------------------------------- */
/* walk_create – descend the active page‑table; optionally allocate           */
/*                                                                              */
/*  vma      : virtual address we are interested in                            */
/*  alloc    : 0 = just look, 1 = allocate missing subtables                  */
/*  returns  : pointer to **level‑0 PTE** for this vma, or NULL on failure    */
/*                                                                              */
/*          root (level‑2)  -->  level‑1  -->  level‑0  -->  returns &pte[0]   */
/* -------------------------------------------------------------------------- */
static inline struct pte *walk_create(uintptr_t vma, int alloc) {
    if (!wellformed(vma))          /* sign‑extension check */
        return NULL;

    struct pte *pt = active_space_ptab();   /* level‑2 table (root) */

    /* ---- level‑2 -------------------------------------------------------- */
    struct pte *pte2 = &pt[vpn_l2(vma)];
    if (!PTE_VALID(*pte2) || !PTE_LEAF(*pte2)) {
        if (!PTE_VALID(*pte2)) {
            /* allocate new level‑1 table if caller allows */
            if (!alloc) return NULL;
            void *new_pt = alloc_phys_page();
            if (!new_pt) return NULL;
            memset(new_pt, 0, PAGE_SIZE);
            *pte2 = ptab_pte((struct pte *)new_pt, PTE_G);  /* global   */
            sfence_vma();                                   /* flush tlb */
        }
        /* point to level‑1 table */
        pt = (struct pte *)pageptr(pte2->ppn);
    } else {
        /* huge 1‑GiB leaf – not supported in CP2 */
        return NULL;
    }

    /* ---- level‑1 -------------------------------------------------------- */
    struct pte *pte1 = &pt[vpn_l1(vma)];
    if (!PTE_VALID(*pte1) || !PTE_LEAF(*pte1)) {
        if (!PTE_VALID(*pte1)) {
            if (!alloc) return NULL;
            void *new_pt = alloc_phys_page();
            if (!new_pt) return NULL;
            memset(new_pt, 0, PAGE_SIZE);
            *pte1 = ptab_pte((struct pte *)new_pt, PTE_G);
            sfence_vma();
        }
        pt = (struct pte *)pageptr(pte1->ppn);
    } else {
        /* 2‑MiB leaf – not used in CP2 */
        return NULL;
    }

    /* ---- level‑0 -------------------------------------------------------- */
    return &pt[vpn_l0(vma)];
}

void * map_page(uintptr_t vma, void * pp, int rwxug_flags) {
    if (!wellformed(vma) || ((uintptr_t)pp & (PAGE_SIZE - 1)))
        return (void *)(intptr_t)-EINVAL;

    struct pte *leaf = walk_create(vma, 1);  // Changed to always allocate if needed
    if (!leaf) return (void *)(intptr_t)-ENOMEM;
    
    *leaf = leaf_pte(pp, rwxug_flags);
    sfence_vma();
    return NULL;
}

void * map_range(uintptr_t vma, size_t size, void * pp, int rwxug_flags) {
    if (size == 0) return (void *)(intptr_t)-EINVAL;
    size = ROUND_UP(size, PAGE_SIZE);
    if (!pp) return (void*)(intptr_t)-EINVAL;

    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        void *phys = pp ? (char *)pp + off : NULL;
        void *ret  = map_page(vma + off, phys, rwxug_flags);
        if ((intptr_t)ret < 0) return ret;        /* propagate errno */
    }
    return NULL;
}

void * alloc_and_map_range(uintptr_t vma, size_t size, int rwxug_flags) {
    if (size == 0)
        panic("alloc_and_map_range: size 0");

    size_t npages = ROUND_UP(size, PAGE_SIZE) >> PAGE_ORDER;
    void  *phys   = alloc_phys_pages(npages);
    if (!phys)
        panic("alloc_and_map_range: out of physical pages");

    void *err = map_range(vma, size, phys, rwxug_flags);
    if ((intptr_t)err < 0)
        panic("alloc_and_map_range: map_range failed");   /* should not happen */
    
    /* Success: by convention the function returns (void*)vma.               */
    return (void *)vma;
}

void set_range_flags(const void * vp, size_t size, int rwxug_flags) {
    uintptr_t vma = (uintptr_t)vp;
    size = ROUND_UP(size, PAGE_SIZE);

    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        struct pte *leaf = walk_create(vma + off, 0);
        if (!leaf || !PTE_VALID(*leaf) || !PTE_LEAF(*leaf))
            continue;
        leaf->flags = (leaf->flags & ~(PTE_R|PTE_W|PTE_X|PTE_U|PTE_G))
                      | (rwxug_flags & (PTE_R|PTE_W|PTE_X|PTE_U|PTE_G));
    }
    sfence_vma();
}

void unmap_and_free_range(void * vp, size_t size) {
    uintptr_t vma = (uintptr_t)vp;
    size = ROUND_UP(size, PAGE_SIZE);

    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        struct pte *leaf = walk_create(vma + off, 0);
        if (!leaf || !PTE_VALID(*leaf) || !PTE_LEAF(*leaf))
            continue;
        void *pp = pageptr(leaf->ppn);
        *leaf = null_pte();
        free_phys_page(pp);
    }
    sfence_vma();
}

void * alloc_phys_page(void) {
    return alloc_phys_pages(1);
}

void free_phys_page(void * pp) {
    free_phys_pages(pp, 1);
}
// TODO: heap_free(best) needs to be done
void * alloc_phys_pages(unsigned int cnt) {
    if (cnt == 0) return NULL;

    struct page_chunk **prevp = &free_chunk_list;
    struct page_chunk  *c     = free_chunk_list;
    struct page_chunk **best_prevp = NULL;
    struct page_chunk  *best       = NULL;

    /* best‑fit search */
    while (c) {
        if (c->pagecnt >= cnt &&
            (!best || c->pagecnt < best->pagecnt)) {
            //best_prev = *prevp ? prevp : NULL;
            best_prevp = prevp;
            best      = c;
        }
        prevp = &c->next;
        c     = c->next;
    }
    if (!best) return NULL;            /* out of memory */
    uintptr_t start_ppn = best->first_ppn;
    if (best->pagecnt == cnt) {
        /* exact fit – remove node from list */
        //start_ppn = ptr_to_pp(best); /* we don’t know real address yet, see below */
        /* fix links */
        if (best_prevp) *best_prevp = best->next;
        else           free_chunk_list = best->next;
        kfree(best);
    } else {
        /* split from front of chunk */
        best->first_ppn += cnt;
        best->pagecnt   -= cnt;
        /* keep node in list (it now represents the tail) */
    }

    return pageptr(start_ppn);
}

// No coalescing of free pages is done. This is a simple first-fit
void free_phys_pages(void * pp, unsigned int cnt) {
    if (!pp || cnt == 0) return;

    struct page_chunk *node = kmalloc(sizeof *node);
    node->first_ppn = pagenum(pp);
    node->pagecnt   = cnt;

    node->next = free_chunk_list;
    free_chunk_list = node;
}

unsigned long free_phys_page_count(void) {
    unsigned long total = 0;
    for (struct page_chunk *c = free_chunk_list; c; c = c->next)
        total += c->pagecnt;
    return total;
}

int handle_umode_page_fault(struct trap_frame * tfr, uintptr_t vma) {
    /* Only handle faults below the kernel base and 4‑KiB aligned */
    if (!wellformed(vma) || vma >= 0x400000000000UL)
        return 0;

    vma = ROUND_DOWN(vma, PAGE_SIZE);

    void *pp = alloc_phys_page();
    if (!pp) return 0;
    memset(pp, 0, PAGE_SIZE);

    void *ret = map_page(vma, pp, MAP_RWUG);
    if ((intptr_t)ret < 0) {
        free_phys_page(pp);
        return 0;
    }
    return 1; // no handled
}


mtag_t active_space_mtag(void) {
    return csrr_satp();
}

static inline mtag_t ptab_to_mtag(struct pte * ptab, unsigned int asid) {
    return (
        ((unsigned long)PAGING_MODE << RISCV_SATP_MODE_shift) |
        ((unsigned long)asid << RISCV_SATP_ASID_shift) |
        pagenum(ptab) << RISCV_SATP_PPN_shift);
}

static inline struct pte * mtag_to_ptab(mtag_t mtag) {
    return (struct pte *)((mtag << 20) >> 8);
}

static inline struct pte * active_space_ptab(void) {
    return mtag_to_ptab(active_space_mtag());
}

static inline void * pageptr(uintptr_t n) {
    return (void*)(n << PAGE_ORDER);
}

static inline unsigned long pagenum(const void * p) {
    return (unsigned long)p >> PAGE_ORDER;
}

static inline int wellformed(uintptr_t vma) {
    // Address bits 63:38 must be all 0 or all 1
    uintptr_t const bits = (intptr_t)vma >> 38;
    return (!bits || !(bits+1));
}

static inline struct pte leaf_pte(const void * pp, uint_fast8_t rwxug_flags) {
    return (struct pte) {
        .flags = rwxug_flags | PTE_A | PTE_D | PTE_V,
        .ppn = pagenum(pp)
    };
}

static inline struct pte ptab_pte(const struct pte * pt, uint_fast8_t g_flag) {
    return (struct pte) {
        .flags = g_flag | PTE_V,
        .ppn = pagenum(pt)
    };
}


static inline struct pte null_pte(void) {
    return (struct pte) { };
}