/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/net/net_stack.h
 * Description: NEWOS IPv4 network stack, milestone 1 (kernel-side):
 * Added: 2026-10-22
 */
#ifndef NET_NET_STACK_H
#define NET_NET_STACK_H

#include <core/core_types.h>

/* NEWOS IPv4 network stack, milestone 1 (kernel-side):
 *
 * - PCnet-PCI TX/RX plumbed into a poll-mode core: frameling harvests RX
 *   descriptors and demultiplexes by ethertype; the PCnet IRQ (which
 *   under QEMU's PIIX routing does not fire reliably) stays masked and
 *   the deliver path runs from pi-honest net_poll() tick+worker calls.
 * - ARP (request/reply + tiny cache), IPv4 emit/parse with checksums,
 *   ICMP echo request/reply. UDP and TCP arrive as later milestones over
 *   the same frame plumbing.
 *
 * Address assignment is a compile-time SLIRP default for now (QEMU
 * `-netdev user`): 10.0.2.15/24 with 10.0.2.2 as gateway. DHCP is
 * pending; the honest requirement is to adjust NET_IP_* when the stack
 * is pointed at a different link. */

#define NET_IPV4(a, b, c, d) \
    (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

#define NET_IP_ADDR     NET_IPV4(10, 0, 2, 15)   /* this link */
#define NET_IP_MASK     NET_IPV4(255, 255, 255, 0)
#define NET_IP_GW       NET_IPV4(10, 0, 2, 2)    /* SLIRP gateway */

#define NET_ETH_MTU     1500u

/* Boot-tail bring-up: stack configuration + demux registration. Safe to
 * call with no NIC (the stack stays dormant). */
void net_init(void);

/* Poll path: harvest received frames and drive protocol processing (ARP
 * cache, ICMP). Interrupt-safe and intended both from the PIT tick and
 * from busy-wait workers (ping loops, dependent request handling).
 * Returns the number of frames consumed. */
int net_poll(void);

/* ARP: resolve an IPv4 address on the wire by emitting a request and
 * polling for a reply; returns 0 with *mac filled or -1. */
int net_arp_resolve(u32 ip, u8 mac[6]);

/* ICMP: send an echo request to `dst` and wait for the reply carrying
 * identical id/seq. Payload is echoed byte-for-byte. Returns 0 and
 * fills *reply_len on match; -1 on timeout or unreachable host. */
int net_icmp_ping(u32 dst, const void *payload, u32 len,
                  void *reply, u32 *reply_len, u32 timeout_us);

/* The gateway configured above, for selftests and userland defaults. */
u32 net_ip_gw(void);
u32 net_ip_addr(void);

/* Nonzero when a NIC is up and the stack is live. */
int net_up(void);

/* Rebind the stack onto a NIC by registry name (ktests exercise each
 * connected adapter this way; the ARP cache is flushed by the switch). */
int net_start_nic(const char *name);

#endif
