/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/drivers/bga.h
 * Description: Bochs Graphics Adapter (BGA/VBE dispi) GPU driver API.
 * Added: 2026-11-23
 */
#ifndef DRIVERS_BGA_H
#define DRIVERS_BGA_H

#include <core/core_types.h>

/* Bochs Graphics Adapter — the display device behind QEMU `-device
 * bochs-display` and VirtualBox's legacy VBE path. Driven through the
 * DISPI index/data register pair (0x1ce / 0x1cf) and a linear
 * framebuffer in a PCI BAR. See docs/roadmap graphics step 1. */

/* Query whether a BGA is present and was successfully initialized. */
int bga_present(void);

/* Framebuffer geometry of the BGA's scanout (only valid if present). */
void bga_geometry(u64 *w, u64 *h, u64 *pitch, u32 *bpp);

/* Physical address of the BGA linear framebuffer. */
u64 bga_framebuffer_phys(void);

/* Set a graphics mode and switch the kernel framebuffer to the BGA's
 * scanout. Returns 0 on success, -1 on unsupported geometry/no device.
 * Uses the standard XRGB8888 mask layout (BGR for 32-bit VBE modes). */
int bga_set_mode(u64 width, u64 height, u32 bpp);

/* Register the driver with the PCI bus (called before pci_bus_scan). */
void bga_driver_register(void);

#endif
