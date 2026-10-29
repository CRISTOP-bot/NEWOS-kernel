/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/arch/x86_64/boot/x86_multiboot2.h
 * Description: Parses the multiboot2 info structure and populates the PMM.
 * Added: 2026-10-17
 */
#ifndef ARCH_X86_64_MBI_H
#define ARCH_X86_64_MBI_H

#include <core/core_types.h>

/* Parses the multiboot2 info structure and populates the PMM. */
void mbi_parse_mmap(u64 mbi_phys);
void mbi_print_summary(u64 mbi_phys);

#endif