/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/drivers/nic.h
 * Description: NIC registry: drivers hand the net stack a uniform surface.
 * Added: 2026-10-22
 */
#ifndef DRIVERS_NIC_H
#define DRIVERS_NIC_H

#include <core/core_types.h>

/* Ethernet NIC registry.
 *
 * Every probe()d NIC hands the net stack one of these; the stack
 * (kernel/net/) talks to whichever adapter is active and never needs
 * driver-specific symbols beyond this header. Drivers become eligible
 * for network duty by registering here during PCI probe; the first
 * registered NIC is the boot default and net_nic_select() can switch
 * (ktests cycle through every NIC this way). */

struct net_nic {
    const char *name;                       /* registry key, e.g. "pcnet" */
    int  (*open)(void);                     /* 0 = card running */
    void (*get_mac)(u8 mac[6]);
    int  (*send)(const u8 *frame, u32 len); /* 0 queued, -1 busy/too big */
    int  (*poll)(int (*cb)(void *ctx, const u8 *frame, u32 len),
                 void *ctx);                /* frames delivered, re-arms */
};

void net_nic_register(const struct net_nic *nic);
const struct net_nic *net_nic_active(void);
int  net_nic_select(const char *name);      /* 0 or -1 (unknown name) */

#endif
