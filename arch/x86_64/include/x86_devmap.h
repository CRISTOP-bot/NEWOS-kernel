/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/arch/x86_64/include/x86_devmap.h
 * Description: devmap: stable kernel-half window for PCI MMIO regions.
 * Added: 2026-10-22
 */
#ifndef ARCH_X86_64_DEVMAP_H
#define ARCH_X86_64_DEVMAP_H

#include <core/core_types.h>

/* Map a device MMIO region into the private kernel-half device window
 * and return its virtual address, or NULL if the window is exhausted.
 * Mappings are uncached (PCD|PWT), never wired again and are intended
 * to outlive the driver: call once per region at probe time. `phys`
 * is clamped down to the enclosing 2 MiB page, so the caller must only
 * touch the range [phys, phys+bytes) inside the returned window. */
void *devmap_map(u64 phys, u64 bytes);

#endif
