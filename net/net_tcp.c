/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/net/net_tcp.c
 * Description: NEWOS TCP client — SYN/SYN-ACK/ACK handshake, in-order RX
 *             stream, FIN close, ACK-gated data send, timeouts.
 * Added: 2026-11-22
 */
#include <net/net_stack.h>
#include <net/net_internal.h>
#include <core/core_time.h>
#include <iru_string.h>

#define TCP_FLAG_FIN 0x01u
#define TCP_FLAG_SYN 0x02u
#define TCP_FLAG_RST 0x04u
#define TCP_FLAG_PSH 0x08u
#define TCP_FLAG_ACK 0x10u

static u8 tcp_seg[20 + NET_TCP_MSS];

/* Build a TCP segment (header + optional payload) in the static buffer.
 * Byte 12-13 carry the data offset (4) and flags; window/checksum at
 * the RFC 793 offsets. */
static void tcp_build(u16 sport, u16 dport, u32 seq, u32 ack, u16 flags,
                     u16 window, const u8 *payload, u32 len)
{
    net_put_be16(tcp_seg + 0, sport);
    net_put_be16(tcp_seg + 2, dport);
    net_put_be32(tcp_seg + 4, seq);
    net_put_be32(tcp_seg + 8, ack);
    net_put_be16(tcp_seg + 12, (u16)((4u << 12) | (u32)flags));
    net_put_be16(tcp_seg + 14, window);
    net_put_be16(tcp_seg + 16, 0);   /* checksum placeholder */
    net_put_be16(tcp_seg + 18, 0);   /* urgent pointer */
    if (len)
        memcpy(tcp_seg + 20, payload, len);
    net_put_be32(tcp_seg + 16, 0);
    net_put_be16(tcp_seg + 16, net_checksum_pseudo(net_ip_local(), net_ip_local(),
                    NET_IP_PROTO_TCP, tcp_seg, 20 + len));
}

int net_tcp_connect(struct net_socket *s, u32 ip, u16 port, u32 timeout_ms)
{
    u32 iss, wait_ms;
    int established = 0;

    if (!s || s->type != NET_SOCK_TCP)
        return -1;
    s->rip = ip;
    s->rport = port;
    s->lport = s->lport ? s->lport : net_sock_ephemeral();
    if (s->lport == 0)
        return -1;

    iss = (u32)net_ticks() ^ ((u32)s->lport * 2654435761u);
    s->iss = iss;
    s->snd_una = iss + 1;             /* SYN consumes one sequence number */
    s->snd_nxt = iss + 1;
    s->rcv_nxt = 0;
    s->snd_wnd = 8192;
    s->tcp_state = NET_TCP_SYN_SENT;

    tcp_build((u16)s->lport, (u16)port, iss, 0, TCP_FLAG_SYN, 8192, 0, 0);
    if (net_ip_send(ip, NET_IP_PROTO_TCP, tcp_seg, 20,
                    timeout_ms ? timeout_ms : 5000) != 0)
        return -1;

    wait_ms = timeout_ms ? timeout_ms : 10000;
    {
        u64 deadline = net_ticks() + net_ms_to_ticks(wait_ms);

        while (net_ticks() < deadline) {
            net_poll();
            if (s->tcp_state == NET_TCP_ESTABLISHED)
                break;
            if (net_ticks() >= deadline)
                break;
            core_time_delay_us(1000);
        }
    }

    if (s->tcp_state == NET_TCP_ESTABLISHED) {
        /* Handshake done: send the accepting ACK. */
        tcp_build((u16)s->lport, (u16)port, s->snd_nxt, s->snd_una,
                  TCP_FLAG_ACK, (u16)s->snd_wnd, 0, 0);
        net_ip_send(s->rip, NET_IP_PROTO_TCP, tcp_seg, 20, 3000);
        established = 1;
    }
    return established ? 0 : -1;
}

int net_tcp_send(struct net_socket *s, const u8 *buf, u32 len, u32 timeout_ms)
{
    u32 seq, seg_len;
    int sent = 0;

    if (!s || s->type != NET_SOCK_TCP)
        return -1;
    if (s->tcp_state != NET_TCP_ESTABLISHED)
        return -1;
    if (len == 0)
        return 0;

    seg_len = len < NET_TCP_MSS ? len : NET_TCP_MSS;
    seq = s->snd_nxt;

    tcp_build((u16)s->lport, (u16)s->rport, seq, s->snd_una,
              TCP_FLAG_ACK | TCP_FLAG_PSH, (u16)s->snd_wnd, buf, seg_len);
    if (net_ip_send(s->rip, NET_IP_PROTO_TCP, tcp_seg, (u32)(20 + seg_len),
                    timeout_ms ? timeout_ms : 5000) != 0)
        return -1;

    /* Wait for the ACK covering this segment. */
    {
        u64 deadline = net_ticks() + net_ms_to_ticks(timeout_ms ? timeout_ms : 5000);
        int acked = 0;

        while (net_ticks() < deadline) {
            net_poll();
            if (net_ticks() >= deadline)
                break;
            if (s->snd_una >= seq + seg_len) {
                acked = 1;
                break;
            }
            core_time_delay_us(1000);
        }
        if (acked) {
            s->snd_una = seq + seg_len;
            s->snd_nxt = seq + seg_len;
            sent = (int)seg_len;
        }
    }
    return sent;
}

int net_tcp_recv(struct net_socket *s, u8 *buf, u32 len, u32 timeout_ms)
{
    u64 deadline;

    if (!s || s->type != NET_SOCK_TCP)
        return -1;
    if (len == 0)
        return 0;
    deadline = net_ticks() + net_ms_to_ticks(timeout_ms ? timeout_ms : 5000);
    while (net_ticks() < deadline) {
        net_poll();
        if (s->rx_len > 0) {
            u32 n = s->rx_len < len ? s->rx_len : len;

            if (n)
                memcpy(buf, s->rx, n);
            s->rx_len -= n;
            if (n)
                memmove(s->rx, s->rx + n, s->rx_len);
            return (int)n;
        }
        if (s->rx_eof)
            return 0;
        if (s->rx_err)
            return -1;
        if (net_ticks() >= deadline)
            break;
        core_time_delay_us(1000);
    }
    return -1;
}

void net_tcp_close(struct net_socket *s)
{
    if (!s || s->type != NET_SOCK_TCP)
        return;
    if (s->tcp_state != NET_TCP_ESTABLISHED)
        return;
    tcp_build((u16)s->lport, (u16)s->rport, s->snd_nxt, s->snd_una,
              TCP_FLAG_ACK | TCP_FLAG_FIN, (u16)s->snd_wnd, 0, 0);
    net_ip_send(s->rip, NET_IP_PROTO_TCP, tcp_seg, 20, 3000);
    s->tcp_state = NET_TCP_FIN_WAIT_1;
}

void net_tcp_rx(const u8 *pkt, u32 len, u32 src, u32 dst)
{
    u16 flags, sport, dport, window;
    u32 seq, ack;
    struct net_socket *s;

    (void)dst;
    if (!pkt || len < 20)
        return;
    if (net_checksum_pseudo(net_ip_local(), src, NET_IP_PROTO_TCP, pkt, len) != 0)
        return;

    flags = net_be16(pkt + 12);
    seq = net_be32(pkt + 4);
    ack = net_be32(pkt + 8);
    sport = net_be16(pkt + 0);
    dport = net_be16(pkt + 2);
    window = net_be16(pkt + 14);

    s = net_sock_find_tcp(src, sport, dport);
    if (!s)
        return;

    if (flags & TCP_FLAG_RST) {
        s->tcp_state = NET_TCP_CLOSED;
        s->rx_err = 1;
        s->rx_eof = 1;
        return;
    }

    if (flags & TCP_FLAG_SYN) {
        if (s->tcp_state != NET_TCP_SYN_SENT)
            return;
        if (ack != s->snd_una)
            return;                     /* retry the handshake */
        s->snd_una = seq + 1;
        s->snd_nxt = seq + 1;
        s->rcv_nxt = seq + 1;
        s->snd_wnd = window;
        s->tcp_state = NET_TCP_ESTABLISHED;
        tcp_build((u16)s->lport, (u16)s->rport, s->snd_nxt, s->snd_una,
                  TCP_FLAG_ACK, (u16)s->snd_wnd, 0, 0);
        net_ip_send(s->rip, NET_IP_PROTO_TCP, tcp_seg, 20, 3000);
        return;
    }

    if (s->tcp_state != NET_TCP_ESTABLISHED)
        return;

    if (flags & TCP_FLAG_ACK) {
        if (ack > s->snd_una)
            s->snd_una = ack;
    }

    u32 data = len - 20;
    if (data > NET_TCP_MSS)
        data = NET_TCP_MSS;

    if (flags & TCP_FLAG_FIN) {
        s->rcv_nxt = seq + data + 1;
        s->rx_eof = 1;
        s->tcp_state = NET_TCP_FIN_WAIT_1;
        return;
    }

    if (seq != s->rcv_nxt)
        return;                         /* duplicate / future segment: drop */

    if (data > 0) {
        if (s->rx_len + data > NET_SOCK_RXCAP) {
            s->rx_err = 1;
            return;
        }
        memcpy(s->rx + s->rx_len, pkt + 20, data);
        s->rx_len += data;
        s->rcv_nxt = seq + data;
    }
}
