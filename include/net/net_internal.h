/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/net/net_internal.h
 * Description: private interfaces shared by the IPv4/UDP/TCP/DHCP layers.
 * Added: 2026-11-22
 */
#ifndef NET_NET_INTERNAL_H
#define NET_NET_INTERNAL_H

#include <core/core_types.h>

/* Private networking interfaces.
 *
 * The public user of the stack is <net/net_stack.h>; this header is only
 * for the net/ translation units. It keeps the Ethernet/IP helpers in one
 * place instead of duplicating byte-order and checksum code in every
 * protocol file. */

#define NET_ETH_ALEN   6
#define NET_ETH_HLEN   14
#define NET_ETH_P_ARP  0x0806u
#define NET_ETH_P_IPV4 0x0800u

#define NET_IP_PROTO_ICMP 1u
#define NET_IP_PROTO_TCP  6u
#define NET_IP_PROTO_UDP  17u

#define NET_SOCK_MAX     8
#define NET_SOCK_RXCAP   8192
#define NET_TCP_MSS      1460
#define NET_UDP_MAX      1600

/* TCP control-block states. */
#define NET_TCP_CLOSED      0
#define NET_TCP_SYN_SENT    1
#define NET_TCP_ESTABLISHED 2
#define NET_TCP_FIN_WAIT_1  3
#define NET_TCP_FIN_WAIT_2  4
#define NET_TCP_CLOSE_WAIT  5
#define NET_TCP_LAST_ACK    6
#define NET_TCP_TIME_WAIT   7

/* One socket handle. TCP and UDP share the slot; only the fields for the
 * selected type are meaningful. The stack is cooperative and poll driven,
 * so a plain struct (no locks) is enough: syscall paths never run
 * concurrently on the single CPU. */
struct net_socket {
    int used;
    int type;               /* 0 TCP, 1 UDP */
    int owner;              /* pid for diagnostics/teardown */
    u16 lport;
    u16 rport;
    u32 rip;
    int connected;

    /* TCP sender state (one segment in flight at a time). */
    int tcp_state;
    u32 snd_una;
    u32 snd_nxt;
    u32 snd_wnd;
    u32 rcv_nxt;
    u32 iss;
    u16 mss;
    u64 last_tx_ticks;
    u32 rto_ticks;
    int retries;

    /* TCP receive stream + EOF/error flags. */
    u8  rx[NET_SOCK_RXCAP];
    u32 rx_len;
    int rx_eof;
    int rx_err;

    /* One queued UDP datagram. */
    u8  udp[NET_UDP_MAX];
    u32 udp_len;
    u32 udp_src;
    u16 udp_sport;
    int udp_ready;
};

/* --- byte order + checksums -------------------------------------------- */
u16 net_be16(const u8 *p);
void net_put_be16(u8 *p, u16 v);
u32 net_be32(const u8 *p);
void net_put_be32(u8 *p, u32 v);
u16 net_checksum(const void *data, u32 len);
u16 net_checksum_pseudo(u32 src, u32 dst, u8 proto,
                        const u8 *data, u32 len);

/* --- link / IP transmit ------------------------------------------------- */
int net_tx_frame(const u8 *dst_mac, u16 ethertype,
                 const u8 *payload, u32 payload_len);
int net_ip_send_raw(u32 src, u32 dst, const u8 *dst_mac, u8 proto,
                    const u8 *payload, u32 len);
/* Route + ARP-resolve `dst`, then transmit. Blocks for at most timeout_ms. */
int net_ip_send(u32 dst, u8 proto, const void *payload, u32 len,
                u32 timeout_ms);
int net_arp_wait(u32 ip, u8 mac[NET_ETH_ALEN], u32 timeout_ms);
void net_arp_flush(void);

u32 net_ip_local(void);
u32 net_ip_netmask(void);
u32 net_ip_gateway(void);
u32 net_ip_dns_server(void);
void net_addr_set(u32 ip, u32 mask, u32 gw, u32 dns);
void net_mac_get(u8 mac[NET_ETH_ALEN]);

/* Monotonic time in PIT ticks (the stack's timeout unit). */
u64 net_ticks(void);
u64 net_ms_to_ticks(u32 ms);

/* --- L4 receive entry points (called by ipv4_rx) ------------------------ */
void net_udp_rx(const u8 *pkt, u32 len, u32 src, u32 dst);
int net_udp_sendto(struct net_socket *s, u32 dst, u16 dport, const u8 *data,
                   u32 len, u32 timeout_ms);
void net_tcp_rx(const u8 *pkt, u32 len, u32 src, u32 dst);
void net_icmp_rx(const u8 *pkt, u32 len, u32 src, u32 dst,
                 const u8 *src_mac);

/* --- DHCP --------------------------------------------------------------- */
void net_dhcp_input(const u8 *pkt, u32 len);
int net_dns_query(int s, const char *name, u32 *ip, u32 timeout_ms);
void net_dhcp_reset(void);

/* --- socket table ------------------------------------------------------- */
struct net_socket *net_sock_alloc(int type, int owner);
struct net_socket *net_sock_get(int handle, int owner);
void net_sock_free(struct net_socket *s);
struct net_socket *net_sock_find_tcp(u32 rip, u16 rport, u16 lport);
struct net_socket *net_sock_find_udp(u16 lport);
struct net_socket *net_sock_find_udp_peer(u32 rip, u16 rport, u16 lport);
u16 net_sock_ephemeral(void);
void net_sock_reset_all(void);

/* TCP client engine (net/net_tcp.c). */
int net_tcp_connect(struct net_socket *s, u32 ip, u16 port, u32 timeout_ms);
int net_tcp_send(struct net_socket *s, const u8 *buf, u32 len,
                 u32 timeout_ms);
int net_tcp_recv(struct net_socket *s, u8 *buf, u32 len, u32 timeout_ms);
void net_tcp_close(struct net_socket *s);

#endif
