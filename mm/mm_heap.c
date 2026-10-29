/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/mm/mm_heap.c
 * Description: Kernel heap arena. In the early phase the arena is a static BSS array so
 * Added: 2026-10-17
 */
#include <mm/mm_heap.h>
#include <core/core_printk.h>
#include <iru_string.h>

/* Kernel heap arena. In the early phase the arena is a static BSS array so
 * the heap is available before PMM paging is fully wired; a later phase
 * grows it from PMM frames directly. */

/* Sized for package extraction: a `.new` archive of static musl binaries
 * lands in tmpfs through kmalloc, and a full GNU coreutils set is ~10 MiB
 * before the package itself is counted.  The complete BusyBox binary adds
 * another ~1.1 MiB, and initramfs duplicates the GNU payload while newpkg
 * extracts it, so the previous 32 MiB arena ran out during boot. */
#define KHEAP_ARENA_SIZE (48u * 1024u * 1024u)   /* 48 MiB */

static u8 kheap_arena[KHEAP_ARENA_SIZE] __attribute__((aligned(4096)));

void kheap_init_early(void)
{
    kheap_init(kheap_arena, KHEAP_ARENA_SIZE);
    pr_info("kheap: %uMiB early arena at %p\n",
            KHEAP_ARENA_SIZE / (1024u * 1024u), kheap_arena);
}