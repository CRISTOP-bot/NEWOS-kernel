/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/net/net_socket.c
 * Description: NEWOS socket table plus the user-facing BSD-ish socket API.
 * Added: 2026-11-22
 */
#include <net/net_stack.h>
#include <net/net_internal.h>
#include <core/core_time.h>
#include <core/core_printk.h>
#include <iru_string.h>

/* Socket handles are slots in one process-owned table. The stack is
 * cooperative (single CPU, poll driven, no interrupts touching it), so
 * a plain array and no locks is exactly the right shape: two syscalls
 * never overlap, and the RX path runs on the same stack as the caller
 * that is polling for it.
 *
 * The slot itself carries the transport state (TCP control block and
 * the one-segment-in-flight sender, or the single queued UDP datagram);
 * net_tcp.c and net_udp.c operate on the same struct so a handle never
 * needs translating. */

static struct net_socket g_socks[NET_SOCK_MAX];
static u16 g_ephemeral = 49152;

/* --- slot table --------------------------------------------------------- */

struct net_socket *net_sock_alloc(int type, int owner)
{
    if (type != NET_SOCK_TCP && type != NET_SOCK_UDP)
        return NULL;

    for (int i = 0; i < NET_SOCK_MAX; i++) {
        struct net_socket *s = &g_socks[i];
        u64 rto;

        if (s->used)
            continue;
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->type = type;
        s->owner = owner;
        s->tcp_state = NET_TCP_CLOSED;
        s->mss = NET_TCP_MSS;

        /* One second of retransmission timeout, clamped to the tick
         * counter's granularity. */
        rto = net_ms_to_ticks(1000);
        if (rto == 0)
            rto = 1;
        if (rto > 0xFFFFFFFFu)
            rto = 0xFFFFFFFFu;
        s->rto_ticks = (u32)rto;
        return s;
    }
    return NULL;
}

struct net_socket *net_sock_get(int handle, int owner)
{
    if (handle < 0 || handle >= NET_SOCK_MAX)
        return NULL;
    if (!g_socks[handle].used || g_socks[handle].owner != owner)
        return NULL;
    return &g_socks[handle];
}

void net_sock_free(struct net_socket *s)
{
    if (!s)
        return;
    memset(s, 0, sizeof(*s));
}

/* Exact 4-tuple match. During the SYN handshake rip/rport are already
 * published by net_tcp_connect(), so the reply finds the socket. */
struct net_socket *net_sock_find_tcp(u32 rip, u16 rport, u16 lport)
{
    for (int i = 0; i < NET_SOCK_MAX; i++) {
        struct net_socket *s = &g_socks[i];

        if (!s->used || s->type != NET_SOCK_TCP)
            continue;
        if (s->rip == rip && s->rport == rport && s->lport == lport)
            return s;
    }
    return NULL;
}

struct net_socket *net_sock_find_udp(u16 lport)
{
    for (int i = 0; i < NET_SOCK_MAX; i++) {
        struct net_socket *s = &g_socks[i];

        if (!s->used || s->type != NET_SOCK_UDP)
            continue;
        if (s->lport == lport)
            return s;
    }
    return NULL;
}

struct net_socket *net_sock_find_udp_peer(u32 rip, u16 rport, u16 lport)
{
    for (int i = 0; i < NET_SOCK_MAX; i++) {
        struct net_socket *s = &g_socks[i];

        if (!s->used || s->type != NET_SOCK_UDP || !s->connected)
            continue;
        if (s->rip == rip && s->rport == rport && s->lport == lport)
            return s;
    }
    return NULL;
}

u16 net_sock_ephemeral(void)
{
    /* Walk the ephemeral range, skipping ports one of our sockets already
     * owns. The range is large enough that 16384 probes always succeed. */
    for (u32 i = 0; i < 16384u; i++) {
        u16 port = g_ephemeral++;
        int busy = 0;

        if (g_ephemeral < 49152)
            g_ephemeral = 49152;
        for (int j = 0; j < NET_SOCK_MAX; j++) {
            if (g_socks[j].used && g_socks[j].lport == port) {
                busy = 1;
                break;
            }
        }
        if (!busy)
            return port;
    }
    return 0;
}

void net_sock_reset_all(void)
{
    memset(g_socks, 0, sizeof(g_socks));
    g_ephemeral = 49152;
}

/* --- public handle API -------------------------------------------------- */

int net_sock_open(int type, int owner)
{
    for (int i = 0; i < NET_SOCK_MAX; i++) {
        if (!g_socks[i].used) {
            if (!net_sock_alloc(type, owner))
                return -1;
            return i;
        }
    }
    return -1;
}

int net_sock_connect(int handle, int owner, u32 ip, u16 port,
                     u32 timeout_ms)
{
    struct net_socket *s = net_sock_get(handle, owner);

    if (!s || port == 0)
        return -1;
    if (s->type == NET_SOCK_TCP)
        return net_tcp_connect(s, ip, port, timeout_ms);

    /* Connected UDP: latch the peer, pick a local port if unbound. */
    if (s->lport == 0) {
        s->lport = net_sock_ephemeral();
        if (s->lport == 0)
            return -1;
    }
    s->rip = ip;
    s->rport = port;
    s->connected = 1;
    return 0;
}

int net_sock_bind(int handle, int owner, u16 port)
{
    struct net_socket *s = net_sock_get(handle, owner);

    if (!s)
        return -1;
    if (s->type == NET_SOCK_TCP) {
        if (s->tcp_state != NET_TCP_CLOSED)
            return -1;
        s->lport = port ? port : net_sock_ephemeral();
        return s->lport ? 0 : -1;
    }

    s->lport = port ? port : net_sock_ephemeral();
    return s->lport ? 0 : -1;
}

int net_sock_send(int handle, int owner, const void *buf, u32 len,
                  u32 timeout_ms)
{
    struct net_socket *s = net_sock_get(handle, owner);

    if (!s || !buf)
        return -1;
    if (s->type == NET_SOCK_TCP)
        return net_tcp_send(s, buf, len, timeout_ms);

    if (!s->connected || s->rport == 0)
        return -1;
    if (s->lport == 0) {
        s->lport = net_sock_ephemeral();
        if (s->lport == 0)
            return -1;
    }
    return net_udp_sendto(s, s->rip, s->rport, buf, len, timeout_ms);
}

int net_sock_recv(int handle, int owner, void *buf, u32 len,
                  u32 timeout_ms)
{
    struct net_socket *s = net_sock_get(handle, owner);
    u64 deadline;

    if (!s || (!buf && len))
        return -1;
    if (s->type == NET_SOCK_TCP)
        return net_tcp_recv(s, buf, len, timeout_ms);

    if (len == 0)
        return 0;
    deadline = net_ticks() + net_ms_to_ticks(timeout_ms);
    while (!s->udp_ready) {
        if (net_ticks() >= deadline)
            return -1;
        net_poll();
        if (s->udp_ready)
            break;
        if (timeout_ms == 0)
            return -1;
        core_time_delay_us(1000);
    }

    {
        u32 n = s->udp_len < len ? s->udp_len : len;

        memcpy(buf, s->udp, n);
        s->udp_ready = 0;
        s->udp_len = 0;
        return (int)n;
    }
}

int net_sock_close(int handle, int owner)
{
    struct net_socket *s = net_sock_get(handle, owner);

    if (!s)
        return -1;
    if (s->type == NET_SOCK_TCP)
        net_tcp_close(s);
    net_sock_free(s);
    return 0;
}

/* --- ICMP convenience --------------------------------------------------- */

int net_ping(u32 ip, u32 timeout_ms, u32 *rtt_us)
{
    static u8 reply[64];
    static const char payload[] = "NEWOS ping";
    u32 rlen = 0;
    u64 t0, t1, hz;

    if (!net_up() || !rtt_us)
        return -1;

    /* net_icmp_ping() takes microseconds and keeps its own budget in a
     * 32-bit counter; 60 s is far beyond any useful RTT and cannot
     * overflow the multiply. */
    if (timeout_ms > 60000)
        timeout_ms = 60000;

    t0 = net_ticks();
    if (net_icmp_ping(ip, payload, (u32)(sizeof(payload) - 1),
                      reply, &rlen, timeout_ms * 1000u) != 0)
        return -1;
    t1 = net_ticks();

    hz = core_time_hz();
    if (hz == 0)
        *rtt_us = (u32)((t1 - t0) * 1000u);
    else
        *rtt_us = (u32)(((t1 - t0) * 1000000u) / hz);
    return 0;
}
