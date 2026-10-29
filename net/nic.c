/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/net/nic.c
 * Description: NIC registry: drivers hand the net stack a uniform surface.
 * Added: 2026-10-22
 */
#include <drivers/nic.h>
#include <iru_string.h>

/* The registry maps every probe()d Ethernet adapter to one struct.
 * PCI scan order fixes the boot default (the stack's net start opens
 * the active NIC only), and ktests pick a specific adapter with
 * net_nic_select() — ARP/ICMP replies travel the slirp network the
 * selected card is plugged into, so a switch also clears the ARP
 * cache (the stack owns it; see net_stack.c).

 * Deliberately small: one dead-simple registry, no refcounts — NICs
 * live for the whole boot. */

#define NET_NIC_MAX 4

static const struct net_nic *g_nics[NET_NIC_MAX];
static unsigned g_nic_count;
static const struct net_nic *g_default;     /* first registered */
static const struct net_nic *g_active;      /* explicit override */

void net_nic_register(const struct net_nic *nic)
{
    unsigned i;

    if (!nic || !nic->name || !nic->open || !nic->send || !nic->poll ||
        !nic->get_mac)
        return;
    for (i = 0; i < g_nic_count; i++)
        if (g_nics[i] == nic)
            return;                     /* same object, duplicate call */
    if (g_nic_count == NET_NIC_MAX)
        return;
    g_nics[g_nic_count++] = nic;
    if (!g_default)
        g_default = nic;
}

const struct net_nic *net_nic_active(void)
{
    return g_active ? g_active : g_default;
}

int net_nic_select(const char *name)
{
    unsigned i;

    if (!name) {
        g_active = NULL;        /* back to the boot default */
        return 0;
    }
    for (i = 0; i < g_nic_count; i++) {
        if (strcmp(g_nics[i]->name, name) == 0) {
            g_active = g_nics[i];
            return 0;
        }
    }
    return -1;
}
