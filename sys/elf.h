// elf.c - ELF file loader
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _ELF_H_
#define _ELF_H_

#include "io.h"

// Memory range constants
#define USER_MEM_START  0x80100000
#define USER_MEM_END    0x81000000

extern int elf_load(struct io * elfio, void (**eptr)(void));

#endif // _ELF_H_