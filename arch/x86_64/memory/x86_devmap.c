/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/arch/x86_64/memory/x86_devmap.c
 * Description: devmap: stable kernel-half window for PCI MMIO regions.
 * Added: 2026-10-22
 */
#include <x86_devmap.h>
#include <x86_mmu.h>
#include <x86_cpu.h>
#include <core/core.h>
#include <core/core_printk.h>

/* Device-MMIO window for PCI peripheral regions.
 *
 * Same discipline proven by drivers/graphics/fb.c: a private 1 GiB
 * kernel-half window at PML4[506] (next to the framebuffer window at
 * [507]), backed by one static PDPT + one static PD of 2 MiB pages with
 * PCD|PWT, wired through the direct map and invalidated explicitly.
 * Deliberately 2 MiB pages (never 1 GiB) so VirtualBox's missing
 * PDPE1GB cannot fault the mapping on first touch. Regions are handed
 * out sequentially top-down into the window and are never unmapped:
 * drivers map once at probe time and keep the address for life. */

#define DEVMAP_PML4_INDEX  506u
#define DEVMAP_VIRT_BASE   (0xFFFFFD0000000000ull)
#define DEVMAP_2M          (2u * 1024u * 1024u)
#define DEVMAP_MAX_SLOTS   512u

static u64 devmap_pdpt[512] __attribute__((aligned(4096)));
static u64 devmap_pd[512] __attribute__((aligned(4096)));
static int devmap_wired;
static u32 devmap_pages;   /* 2 MiB slots consumed so far */

void *devmap_map(u64 phys, u64 bytes)
{
    u64 base, span, n;
    u32 i;

    if (!bytes)
        return NULL;
    base = phys & ~(u64)(DEVMAP_2M - 1u);
    span = (phys - base) + bytes;
    n = (span + DEVMAP_2M - 1u) / DEVMAP_2M;
    if (devmap_pages + n > DEVMAP_MAX_SLOTS) {
        pr_warn("devmap: window exhausted (%llu more 2M pages)\n", n);
        return NULL;
    }

    if (!devmap_wired) {
        /* Reach the PML4 through the direct map, never through its
         * link-time address: the early tables sit at physical-low
         * addresses, so a high-half RIP-relative displacement cannot
         * reach them under -mcmodel=kernel (same idiom as fb.c). */
        u64 *pml4 = (u64 *)phys_to_virt(virt_to_phys((uintptr_t)x64_pml4));
        for (i = 0; i < 512; i++)
            devmap_pd[i] = 0;
        devmap_pdpt[0] = virt_to_phys((uintptr_t)devmap_pd) |
                         X64_PAGE_PRESENT | X64_PAGE_WRITE;
        pml4[DEVMAP_PML4_INDEX] = virt_to_phys((uintptr_t)devmap_pdpt) |
                                  X64_PAGE_PRESENT | X64_PAGE_WRITE;
        devmap_wired = 1;
    }

    for (i = 0; i < (u32)n; i++)
        devmap_pd[devmap_pages + i] = (base + (u64)i * DEVMAP_2M) |
            X64_PAGE_PRESENT | X64_PAGE_WRITE | X64_PAGE_PCD |
            X64_PAGE_PWT | X64_PAGE_HUGE;
    for (i = 0; i < (u32)n; i++)
        invlpg(DEVMAP_VIRT_BASE + (u64)(devmap_pages + i) * DEVMAP_2M);
    cpu_mfence();

    devmap_pages += (u32)n;
    return (u8 *)(DEVMAP_VIRT_BASE + (phys - base));
}
