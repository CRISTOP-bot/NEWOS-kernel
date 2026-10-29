/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/drivers/pcnet.h
 * Description: AMD PCnet/PCI Ethernet driver.
 * Added: 2026-10-17
 */
#ifndef DRIVERS_PCNET_H
#define DRIVERS_PCNET_H

#include <core/core_types.h>

/* AMD PCnet/PCI Ethernet driver.
 *
 * Drives the PCnet-PCI II (Am79C970A/Am79C973, PCI vendor 0x1022, device
 * 0x2000) - the NIC QEMU has natively (and the 'pcnet' SMSC/AMD card
 * VirtualBox offers) - in 32-bit DWIO mode with SWSTYLE-2 descriptors.
 *
 * The binder configures the PCI function (I/O BAR, bus mastering, IRQ);
 * bring-up (soft reset, BCR/init block, START) and the loopback selftest
 * are driven from pcnet_selftest(), which the kernel self-test suite runs. */

void pcnet_driver_register(void);
int  pcnet_selftest(void);

/* Net-core interface. All of these fail closed (-1 / no-op) until the
 * driver has probed a NIC. */
int  pcnet_open(void);                     /* 0 = card running */
void pcnet_get_mac(u8 mac[6]);
int  pcnet_send(const u8 *frame, u32 len); /* 0 queued, -1 busy/too big */
/* Harvest every RX descriptor with PCnet_RMD_OWN clear, deliver each
 * frame through (*cb)(ctx, frame, len) and re-arm the buffer. Returns
 * the number of frames delivered. */
int  pcnet_poll(int (*cb)(void *ctx, const u8 *frame, u32 len),
                void *ctx);

#endif