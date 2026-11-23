/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/net/net_udp.c
 * Description: NEWOS UDP transport over the IPv4 layer (connected sockets,
 *              broadcast delivery to the DHCP listener, datagram copy-out).
 * Added: 2026-11-22
 */
#include <net/net_stack.h>
#include <net/net_internal.h>
#include <core/core_time.h>
#include <iru_string.h>

/* Build a UDP segment for a socket -> remote pair and push it. The
 * pseudo-header checksum covers src/dst/protocol/length like RFC 768
 * UDP. A computed checksum of 0x0000 is transmitted as 0xFFFF. */
static int udp_emit(struct net_socket *s, u32 dst, u16 dport, const u8 *data,
                   u32 len, u32 timeout_ms)
{
    static u8 seg[8 + NET_UDP_MAX];

    if (len > NET_UDP_MAX - 8)
        return -1;

    net_put_be16(seg + 0, s->lport);
    net_put_be16(seg + 2, dport);
    net_put_be16(seg + 4, (u16)(8 + len));
    net_put_be16(seg + 6, 0);
    if (len)
        memcpy(seg + 8, data, len);

    {
        u16 csum = net_checksum_pseudo(s->rip, dst, NET_IP_PROTO_UDP, seg,
                                      8 + len);

        net_put_be16(seg + 6, csum ? csum : 0xFFFF);
    }
    return net_ip_send(dst, NET_IP_PROTO_UDP, seg, (u32)(8 + len), timeout_ms);
}

int net_udp_sendto(struct net_socket *s, u32 dst, u16 dport, const u8 *data,
                   u32 len, u32 timeout_ms)
{
    if (!s || s->type != NET_SOCK_UDP)
        return -1;
    if (len > NET_UDP_MAX - 8)
        return -1;
    if (udp_emit(s, dst, dport, data, len, timeout_ms) != 0)
        return -1;
    return (int)len;
}

void net_udp_rx(const u8 *pkt, u32 len, u32 src, u32 dst)
{
    u16 sport, dport;
    u32 seglen;

    (void)dst;
    if (!pkt || len < 8)
        return;

    sport = net_be16(pkt + 0);
    dport = net_be16(pkt + 2);
    seglen = net_be16(pkt + 4);

    /* The DHCP client listens on UDP/68 from the broadcast address. */
    if (dport == 68) {
        net_dhcp_input(pkt, len);
        return;
    }

    if (seglen > NET_UDP_MAX)
        return;
    if (seglen > len - 8)
        return;

    struct net_socket *s = net_sock_find_udp_peer(src, dport, sport);

    if (!s || s->udp_len > 0)
        return;                     /* single-datagram socket semantics */
    if (seglen > 0) {
        memcpy(s->udp, pkt + 8, seglen);
        s->udp_len = seglen;
        s->udp_src = src;
        s->udp_sport = sport;
        s->udp_ready = 1;
    }
}
