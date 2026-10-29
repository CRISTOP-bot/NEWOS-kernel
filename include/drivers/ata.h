/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/drivers/ata.h
 * Description: Legacy-PIO ATA driver, primary master only (kernel/drivers/block/ata.c).
 * Added: 2026-10-22
 */
#ifndef DRIVERS_ATA_H
#define DRIVERS_ATA_H

#include <core/core_types.h>
#include <drivers/block_dev.h>

/* Legacy-PIO ATA driver, primary master only (kernel/drivers/block/ata.c).
 *
 * The disk shows up as a block_dev_t named "disk0" backed by ISA-legacy
 * I/O ports (0x1F0). Every command completes by polling the status
 * register with nIEN set, so no IRQ paths are involved; there is no DMA
 * and no PCI programming. This is the one device shape QEMU exposes via
 * `-drive if=ide` and it is also what VirtualBox's IDE controller pro-
 * vides on its default ports, so the driver intentionally stays away
 * from AHCI/PCI IDE programming. Returns -1 when no plain ATA disk is
 * present (ATAPI signatures are skipped, not claimed). */
int ata_init(void);

/* The probed primary-master disk, or NULL before a successful ata_init()
 * (or when no drive is attached). devfs uses this to decide /dev/disk0. */
block_dev_t *ata_get_dev(void);

#endif
