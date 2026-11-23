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

/* Interface state getters + setter (SYS_NET_INFO / SYS_NET_SETIP). */
u32 net_ip_local(void);
u32 net_ip_netmask(void);
u32 net_ip_gateway(void);
u32 net_ip_dns_server(void);
void net_addr_set(u32 ip, u32 mask, u32 gw, u32 dns);
void net_mac_get(u8 mac[6]);

/* Nonzero when a NIC is up and the stack is live. */
int net_up(void);

/* Rebind the stack onto a NIC by registry name (ktests exercise each
 * connected adapter this way; the ARP cache is flushed by the switch). */
int net_start_nic(const char *name);

/* --- socket / transport API (kernel side) -------------------------------- */

#define NET_SOCK_TCP 0
#define NET_SOCK_UDP 1

/* Per-process socket handles. Each call takes the caller's pid so a
 * handle cannot be used by another process. Return values are >= 0 on
 * success and -1 on error. */
int net_sock_open(int type, int owner);
int net_sock_connect(int handle, int owner, u32 ip, u16 port,
                     u32 timeout_ms);
int net_sock_bind(int handle, int owner, u16 port);
int net_sock_send(int handle, int owner, const void *buf, u32 len,
                  u32 timeout_ms);
int net_sock_recv(int handle, int owner, void *buf, u32 len,
                  u32 timeout_ms);
int net_sock_close(int handle, int owner);

/* DHCP: DISCOVER/OFFER/REQUEST/ACK on the active NIC, then apply the
 * lease. Returns 0 when the interface is configured. */
int net_dhcp_config(u32 timeout_ms);

/* ICMP echo with an RTT measured in PIT ticks converted to microseconds. */
int net_ping(u32 ip, u32 timeout_ms, u32 *rtt_us);

/* Stub resolver over UDP: A records only, returns 0 and *ip in host order. */
int net_dns_resolve(const char *name, u32 *ip, u32 timeout_ms);

#endif
