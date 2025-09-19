// elf.c - ELF file loader
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "elf.h"
#include "conf.h"
#include "io.h"
#include "string.h"
#include "memory.h"
#include "assert.h"
#include "error.h"
#include "ioimpl.h"

#include <stdint.h>

// Offsets into e_ident

#define EI_CLASS        4   
#define EI_DATA         5
#define EI_VERSION      6
#define EI_OSABI        7
#define EI_ABIVERSION   8   
#define EI_PAD          9  


// ELF header e_ident[EI_CLASS] values

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

// ELF header e_ident[EI_DATA] values

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

// ELF header e_ident[EI_VERSION] values

#define EV_NONE     0
#define EV_CURRENT  1

//ELF magic values

#define ELFMAG0 0x7f

#define ELFMAG1 'E'

#define ELFMAG2 'L'

#define ELFMAG3 'F'

// ELF header e_type values

enum elf_et {
    ET_NONE = 0,
    ET_REL,
    ET_EXEC,
    ET_DYN,
    ET_CORE
};

struct elf64_ehdr {
    unsigned char e_ident[16]; // Magic + identification bytes
    uint16_t e_type; // Object file type (ET_EXEC, ET_DYN, etc.)
    uint16_t e_machine; // Architecture (EM_RISCV=243)
    uint32_t e_version; // ELF version (EV_CURRENT=1)
    uint64_t e_entry; // Entry point virtual address
    uint64_t e_phoff; // Program header table offset
    uint64_t e_shoff; // Section header table offset (unused here)
    uint32_t e_flags;  // Processor-specific flags
    uint16_t e_ehsize;  // ELF header size
    uint16_t e_phentsize; // Program header entry size
    uint16_t e_phnum; // Number of program headers
    uint16_t e_shentsize;  // Section header entry size (unused)
    uint16_t e_shnum;      // Number of section headers (unused)
    uint16_t e_shstrndx;   // Section header string table index (unused)
};

enum elf_pt {
	PT_NULL = 0, 
	PT_LOAD,
	PT_DYNAMIC,
	PT_INTERP,
	PT_NOTE,
	PT_SHLIB,
	PT_PHDR,
	PT_TLS
};

// Program header p_flags bits

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

// ELF header e_machine values (short list)

#define  EM_RISCV   243
int elf_load(struct io *elfio, void (**eptr)(void)) {
    struct elf64_ehdr ehdr;

    if (elfio == NULL || elfio->intf == NULL)
        return -EIO;

    /* ① unmap any old user pages */
    //reset_active_mspace(); 

    /* seek & read ELF header */
    if (ioseek(elfio, 0) < 0 ||
        ioreadat(elfio, 0, &ehdr, sizeof(ehdr)) != sizeof(ehdr))
        return -EIO;

    /* validate magic */
    if (ehdr.e_ident[0] != ELFMAG0 ||
        ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 ||
        ehdr.e_ident[3] != ELFMAG3)
        return -EINVAL;

    /* 64‑bit RISC‑V, proper entry, endianness, version */
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr.e_machine              != EM_RISCV  ||
        ehdr.e_ident[EI_DATA]       != ELFDATA2LSB ||
        ehdr.e_ident[EI_VERSION]    != EV_CURRENT ||
        ehdr.e_entry < UMEM_START_VMA ||
        ehdr.e_entry >= UMEM_END_VMA)
        return -EINVAL;

    /* load each PT_LOAD segment */
    for (int i = 0; i < ehdr.e_phnum; i++) {
        struct elf64_phdr phdr;
        uint64_t off = ehdr.e_phoff + i * ehdr.e_phentsize;

        if (ioreadat(elfio, off, &phdr, sizeof(phdr)) != sizeof(phdr))
            return -EIO;
        if (phdr.p_type != PT_LOAD)
            continue;

        /* bounds check */
        if (phdr.p_vaddr < UMEM_START_VMA ||
            phdr.p_vaddr + phdr.p_memsz > UMEM_END_VMA)
            return -EINVAL;

        /* ② allocate & map RW pages for this segment */
        alloc_and_map_range(phdr.p_vaddr,
                            phdr.p_memsz,
                            PTE_R | PTE_W | PTE_U);  // :contentReference[oaicite:2]{index=2}&#8203;:contentReference[oaicite:3]{index=3}

        /* copy file data */
        if (ioreadat(elfio, phdr.p_offset,
                     (void*)phdr.p_vaddr,
                     phdr.p_filesz) != phdr.p_filesz)
            return -EIO;
        /* zero BSS */
        if (phdr.p_memsz > phdr.p_filesz)
            memset((void*)(phdr.p_vaddr + phdr.p_filesz),
                   0,
                   phdr.p_memsz - phdr.p_filesz);

        /* ③ set final permissions to match ELF flags */
        uint8_t flags = PTE_U;
        if (phdr.p_flags & PF_R) flags |= PTE_R;
        if (phdr.p_flags & PF_W) flags |= PTE_W;
        if (phdr.p_flags & PF_X) flags |= PTE_X;
        set_range_flags((void*)phdr.p_vaddr,
                        phdr.p_memsz,
                        flags);  // :contentReference[oaicite:4]{index=4}&#8203;:contentReference[oaicite:5]{index=5}
    }

    /* hand back entry point */
    *eptr = (void(*)(void))ehdr.e_entry;
    return 0;
}