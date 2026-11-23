/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/net/net_stack.c
 * Description: NEWOS IPv4 network stack (milestone 1): frame plumbing, ARP, IPv4 and
 * Added: 2026-10-22
 */
#include <net/net_stack.h>
#include <net/net_internal.h>
#include <net/wifi.h>
#include <drivers/nic.h>
#include <drivers/pit_timer.h>
#include <core/core.h>
#include <core/core_printk.h>
#include <core/core_time.h>
#include <iru_string.h>

/* NEWOS IPv4 network stack (milestone 1): frame plumbing, ARP, IPv4 and
 * ICMP echo, running poll-mode over whichever NIC registered itself
 * (see <drivers/nic.h>; PCnet was first, e1000 follows the same round
 * trip).
 *
 * Addressing is the compile-time SLIRP default from <net/net_stack.h>
 * (QEMU -netdev user): this host is 10.0.2.15/24, the gateway lives at
 * 10.0.2.2 and answers both ARP and ICMP echo. DHCP is pending and TCP
 * arrives as a later milestone over the same frame layer. The stack has
 * no timers of its own: every path is driven by callers polling
 * net_poll() (PIT tick, ping loops, later socket paths), which keeps it
 * cooperative and honest about the IRQ the QEMU wiring never delivers. */

#define ETH_ALEN        6
#define ETH_HLEN        14

#define ETH_P_ARP       0x0806u
#define ETH_P_IPV4      0x0800u

#define ARP_HRD_ETHER   1u
#define ARP_OP_REQUEST  1u
#define ARP_OP_REPLY    2u

#define IP_PROTO_ICMP   1u

#define ICMP_ECHO_REPLY 0u
#define ICMP_ECHO_REQ   8u

#define ARP_CACHE_SLOTS 8
#define ICMP_BODY_MAX   64      /* echo payload handled by the raw core */

#define PING_ARP_BUDGET 1000000u    /* us spent answering ARP per ping */

/* --- state ---------------------------------------------------------------- */

static u32 g_ip_addr = NET_IP_ADDR;
static u32 g_ip_mask = NET_IP_MASK;
static u32 g_ip_gw = NET_IP_GW;
static u32 g_ip_dns;
static u8 g_mac[ETH_ALEN];
static int g_up;

struct arp_entry {
    u32 ip;
    u8 mac[ETH_ALEN];
    int valid;
};

static struct arp_entry g_arp_cache[ARP_CACHE_SLOTS];

/* One outstanding echo request; the stack is cooperative, so a single
 * waiter slot is enough for the selftests and the first userland ping. */
struct icmp_wait {
    int waiting;
    u32 peer;
    u16 id;
    u16 seq;
    u8 *reply;
    u32 reply_cap;
    u32 *reply_len;
    int done;
};

static struct icmp_wait g_icmp_wait;

/* --- byte order + checksum helpers ----------------------------------------- */

u16 net_be16(const u8 *p)
{
    return (u16)(((u32)p[0] << 8) | p[1]);
}

void net_put_be16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)(v & 0xFF);
}

u32 net_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | (u32)p[3];
}

void net_put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)(v & 0xFF);
}

/* One's-complement Internet checksum over a byte stream. */
u16 net_checksum(const void *data, u32 len)
{
    const u8 *p = data;
    u32 sum = 0;

    for (u32 i = 0; i + 1 < len; i += 2)
        sum += (u32)((p[i] << 8) | p[i + 1]);
    if (len & 1)
        sum += (u32)(p[len - 1] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (u16)(~sum & 0xFFFFu);
}

/* Pseudo-header checksum for UDP/TCP: the transport checksum covers the
 * segment plus src/dst/protocol/length. Feeding the accumulator through
 * net_checksum over a small staging area keeps one implementation. */
u16 net_checksum_pseudo(u32 src, u32 dst, u8 proto,
                        const u8 *data, u32 len)
{
    static u8 scratch[12 + NET_UDP_MAX];

    if (len > NET_UDP_MAX)
        return 0xFFFFu;

    net_put_be32(scratch + 0, src);
    net_put_be32(scratch + 4, dst);
    scratch[8] = 0;
    scratch[9] = proto;
    net_put_be16(scratch + 10, (u16)len);
    memcpy(scratch + 12, data, len);
    return net_checksum(scratch, 12 + len);
}

/* --- frame TX --------------------------------------------------------------- */

static const u8 g_mac_bcast[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF,
                                         0xFF, 0xFF};

int net_tx_frame(const u8 *dst_mac, u16 ethertype,
                 const u8 *payload, u32 payload_len)
{
    if (!g_up || payload_len > NET_ETH_MTU)
        return -1;

    u8 frame[ETH_HLEN + NET_ETH_MTU];
    memcpy(frame, dst_mac, ETH_ALEN);
    memcpy(frame + ETH_ALEN, g_mac, ETH_ALEN);
    net_put_be16(frame + 12, ethertype);
    memcpy(frame + ETH_HLEN, payload, payload_len);

    /* Ethernet's minimum frame is 60 bytes (ARP shorts fall below); the
     * card layer refuses anything smaller, so pad as the NIC would. */
    u32 len = ETH_HLEN + payload_len;
    if (len < 60) {
        memset(frame + len, 0, 60 - len);
        len = 60;
    }
    {
        const struct net_nic *nic = net_nic_active();
        if (!nic)
            return -1;
        return nic->send(frame, len);
    }
}

/* --- ARP ---------------------------------------------------------------------- */

static void arp_cache_learn(u32 ip, const u8 mac[ETH_ALEN])
{
    if (ip == 0)
        return;

    int slot = -1;
    for (int i = 0; i < ARP_CACHE_SLOTS; i++) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            memcpy(g_arp_cache[i].mac, mac, ETH_ALEN);
            return;
        }
        if (!g_arp_cache[i].valid && slot < 0)
            slot = i;
    }
    if (slot < 0)
        slot = 0;                      /* overwrite: cache is tiny anyway */
    g_arp_cache[slot].valid = 1;
    g_arp_cache[slot].ip = ip;
    memcpy(g_arp_cache[slot].mac, mac, ETH_ALEN);
}

static int arp_cache_lookup(u32 ip, u8 mac[ETH_ALEN])
{
    for (int i = 0; i < ARP_CACHE_SLOTS; i++) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            memcpy(mac, g_arp_cache[i].mac, ETH_ALEN);
            return 0;
        }
    }
    return -1;
}

/* Wire an ARP message. `tpa` is the protocol target field; requests
 * zero the hardware target and broadcast, replies name the requester. */
static void arp_tx(u16 op, u32 tpa, const u8 *dst_mac, const u8 req_zero[6])
{
    u8 arp[28];

    net_put_be16(arp + 0, ARP_HRD_ETHER);
    net_put_be16(arp + 2, ETH_P_IPV4);
    arp[4] = ETH_ALEN;
    arp[5] = 4;
    net_put_be16(arp + 6, op);
    memcpy(arp + 8, g_mac, ETH_ALEN);
    net_put_be32(arp + 14, g_ip_addr);
    memcpy(arp + 18, req_zero, ETH_ALEN);
    net_put_be32(arp + 24, tpa);
    net_tx_frame(dst_mac, ETH_P_ARP, arp, sizeof(arp));
}

int net_arp_resolve(u32 ip, u8 mac_out[ETH_ALEN])
{
    if (!g_up)
        return -1;
    if (arp_cache_lookup(ip, mac_out) == 0)
        return 0;

    /* One request per miss; the caller's poll loop carries the reply
     * (and the entry it creates) in. */
    static const u8 zero[ETH_ALEN];
    arp_tx(ARP_OP_REQUEST, ip, g_mac_bcast, zero);
    return -1;
}

void net_arp_flush(void)
{
    memset(g_arp_cache, 0, sizeof(g_arp_cache));
}

/* Blocking resolve: emit a request, poll the NIC and repeat until the
 * reply lands or the caller's budget expires. Every higher layer (TCP,
 * UDP send, ICMP reply) goes through this, so ARP timing lives in one
 * place instead of in each protocol loop. */
int net_arp_wait(u32 ip, u8 mac[ETH_ALEN], u32 timeout_ms)
{
    u8 bcast[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    u64 deadline;

    if (!g_up)
        return -1;
    if (ip == 0xFFFFFFFFu) {
        memcpy(mac, bcast, ETH_ALEN);
        return 0;
    }
    if (arp_cache_lookup(ip, mac) == 0)
        return 0;

    deadline = net_ticks() + net_ms_to_ticks(timeout_ms ? timeout_ms : 2000);
    for (;;) {
        if (net_arp_resolve(ip, mac) == 0)
            return 0;
        net_poll();
        if (net_ticks() >= deadline)
            return -1;
        core_time_delay_us(2000);
    }
}

/* Route an IPv4 datagram: on-link destinations resolve directly, the
 * rest go through the gateway. Broadcast goes to the Ethernet broadcast
 * address without ARP (used by DHCP). */
int net_ip_send(u32 dst, u8 proto, const void *payload, u32 len,
                u32 timeout_ms)
{
    u8 mac[ETH_ALEN];
    u32 hop;
    if (!g_up)
        return -1;
    if (dst == 0xFFFFFFFFu || dst == (g_ip_addr | ~g_ip_mask)) {
        memset(mac, 0xFF, ETH_ALEN);
        return net_ip_send_raw(g_ip_addr, dst, mac, proto, payload, len);
    }

    if ((dst & g_ip_mask) == (g_ip_addr & g_ip_mask))
        hop = dst;
    else
        hop = g_ip_gw ? g_ip_gw : dst;
    if (hop == 0)
        hop = dst;
    if (net_arp_wait(hop, mac, timeout_ms) != 0)
        return -1;
    return net_ip_send_raw(g_ip_addr, dst, mac, proto, payload, len);
}

static void arp_rx(const u8 *arp)
{
    if (net_be16(arp + 0) != ARP_HRD_ETHER ||
        net_be16(arp + 2) != ETH_P_IPV4 ||
        arp[4] != ETH_ALEN || arp[5] != 4)
        return;

    u16 op = net_be16(arp + 6);
    u8 sha[ETH_ALEN];
    memcpy(sha, arp + 8, ETH_ALEN);
    u32 spa = net_be32(arp + 14);
    u32 tpa = net_be32(arp + 24);

    arp_cache_learn(spa, sha);

    if (op == ARP_OP_REQUEST && tpa == g_ip_addr)
        arp_tx(ARP_OP_REPLY, spa, sha, sha);
}

/* --- IPv4 ---------------------------------------------------------------------- */

static u16 g_ip_id;

/* Build + send an IPv4 datagram directly to a known neighbor MAC. `src`
 * is explicit so the DHCP client can transmit from 0.0.0.0 before the
 * interface has an address. */
int net_ip_send_raw(u32 src, u32 dst, const u8 *dst_mac, u8 proto,
                    const u8 *payload, u32 len)
{
    u8 out[20 + NET_ETH_MTU];

    if (len > NET_ETH_MTU - 20)
        return -1;

    out[0] = 0x45;                     /* version 4, IHL 5 */
    out[1] = 0;                        /* TOS */
    net_put_be16(out + 2, (u16)(20 + len));
    net_put_be16(out + 4, ++g_ip_id);
    net_put_be16(out + 6, 0);              /* flags + fragment offset: none */
    out[8] = 64;                       /* TTL */
    out[9] = proto;
    net_put_be16(out + 10, 0);             /* checksum placeholder */
    net_put_be32(out + 12, src);
    net_put_be32(out + 16, dst);
    net_put_be16(out + 10, net_checksum(out, 20));

    memcpy(out + 20, payload, len);
    return net_tx_frame(dst_mac, ETH_P_IPV4, out, 20 + len);
}

/* Direct-neighbor send used by ARP replies and ICMP echo answers. */
static int ipv4_tx(u32 dst, const u8 *dst_mac, u8 proto,
                   const u8 *payload, u32 payload_len)
{
    return net_ip_send_raw(g_ip_addr, dst, dst_mac, proto,
                           payload, payload_len);
}

static void ipv4_rx(const u8 *ip, u32 avail, const u8 *src_mac)
{
    if (avail < 20 || (ip[0] >> 4) != 4)
        return;
    u32 ihl = (ip[0] & 0x0Fu) * 4;
    if (ihl < 20 || ihl > avail)
        return;
    if (net_checksum(ip, ihl) != 0)
        return;

    /* Slice by the header's own total length: the NIC reports buffer
     * lengths that include padding and the 4-byte FCS (the loopback
     * self-test sees 64 for a 60-byte frame), both of which would
     * poison the L4 checksum below. */
    u32 tot = net_be16(ip + 2);
    if (tot > avail || tot < ihl)
        return;

    u32 src = net_be32(ip + 12);
    u32 dst = net_be32(ip + 16);
    /* Unicast to us, limited broadcast (DHCP) and the pre-address 0.0.0.0
     * destination all belong to this host. Subnet broadcast is accepted
     * when the mask makes it ours. */
    u32 subnet_bcast = g_ip_addr | ~g_ip_mask;
    if (dst != g_ip_addr && dst != 0xFFFFFFFFu && dst != 0 &&
        dst != subnet_bcast)
        return;

    switch (ip[9]) {
    case NET_IP_PROTO_ICMP:
        if (tot - ihl >= 8)
            net_icmp_rx(ip + ihl, tot - ihl, src, dst, src_mac);
        break;
    case NET_IP_PROTO_UDP:
        if (tot - ihl >= 8)
            net_udp_rx(ip + ihl, tot - ihl, src, dst);
        break;
    case NET_IP_PROTO_TCP:
        if (tot - ihl >= 20)
            net_tcp_rx(ip + ihl, tot - ihl, src, dst);
        break;
    default:
        break;
    }
}

/* --- ICMP ----------------------------------------------------------------------- */

/* Echo request to a neighbor already resolved (callers drive ARP via
 * net_arp_resolve + net_poll first). */
static int icmp_tx_echo_request(u32 dst, const u8 *dst_mac, u16 id,
                                u16 seq, const void *payload, u32 len)
{
    u8 msg[8 + ICMP_BODY_MAX];

    if (len > ICMP_BODY_MAX)
        return -1;
    msg[0] = ICMP_ECHO_REQ;
    msg[1] = 0;
    net_put_be16(msg + 2, 0);              /* checksum placeholder */
    net_put_be16(msg + 4, id);
    net_put_be16(msg + 6, seq);
    memcpy(msg + 8, payload, len);
    net_put_be16(msg + 2, net_checksum(msg, 8 + len));
    return ipv4_tx(dst, dst_mac, NET_IP_PROTO_ICMP, msg, 8 + len);
}

int net_icmp_ping(u32 dst, const void *payload, u32 len,
                  void *reply, u32 *reply_len, u32 timeout_us)
{
    u8 mac[ETH_ALEN];

    if (!g_up || !payload || !reply || !reply_len || len > ICMP_BODY_MAX)
        return -1;

    /* Ask + poll until the peer answers ARP. */
    u32 waited = 0;
    while (net_arp_resolve(dst, mac) != 0) {
        if (waited >= PING_ARP_BUDGET)
            return -1;
        net_poll();
        time_delay_us(2000);
        waited += 2000;
    }

    /* Claim the single waiter slot. */
    if (g_icmp_wait.waiting)
        return -1;
    g_icmp_wait.waiting = 1;
    g_icmp_wait.done = 0;
    g_icmp_wait.peer = dst;
    g_icmp_wait.id = 0x4E45;           /* "NE" for NEWOS */
    g_icmp_wait.seq++;
    g_icmp_wait.reply = reply;
    g_icmp_wait.reply_cap = ICMP_BODY_MAX;
    g_icmp_wait.reply_len = reply_len;

    int rc = icmp_tx_echo_request(dst, mac, g_icmp_wait.id,
                                  g_icmp_wait.seq, payload, len);
    if (rc != 0) {
        g_icmp_wait.waiting = 0;
        return -1;
    }

    waited = 0;
    while (waited < timeout_us) {
        net_poll();
        if (g_icmp_wait.done)
            break;
        time_delay_us(500);
        waited += 500;
    }

    g_icmp_wait.waiting = 0;
    *reply_len = g_icmp_wait.done ? *g_icmp_wait.reply_len : 0;
    return g_icmp_wait.done ? 0 : -1;
}

void net_icmp_rx(const u8 *icmp_msg, u32 len, u32 src, u32 dst,
                 const u8 *src_mac)
{
    (void)dst;
    if (len < 8 || net_checksum(icmp_msg, len) != 0)
        return;

    u8 type = icmp_msg[0];
    u16 id = net_be16(icmp_msg + 4);
    u16 seq = net_be16(icmp_msg + 6);

    if (type == ICMP_ECHO_REQ) {
        /* Reply with the identical message, swapped type. */
        u8 resp[8 + ICMP_BODY_MAX];
        u32 body = len - 8;
        if (body > ICMP_BODY_MAX)
            return;
        memcpy(resp, icmp_msg, 8 + body);
        resp[0] = ICMP_ECHO_REPLY;
        net_put_be16(resp + 2, 0);
        net_put_be16(resp + 2, net_checksum(resp, 8 + body));
        ipv4_tx(src, src_mac, NET_IP_PROTO_ICMP, resp, 8 + body);
        return;
    }

    if (type == ICMP_ECHO_REPLY && g_icmp_wait.waiting && !g_icmp_wait.done &&
        g_icmp_wait.peer == src && g_icmp_wait.id == id &&
        g_icmp_wait.seq == seq) {
        u32 body = len - 8;
        if (body > g_icmp_wait.reply_cap)
            body = g_icmp_wait.reply_cap;
        memcpy(g_icmp_wait.reply, icmp_msg + 8, body);
        *g_icmp_wait.reply_len = body;
        g_icmp_wait.done = 1;
    }
}

/* --- demux + poll --------------------------------------------------------------- */

static int rx_cb(void *ctx, const u8 *frame, u32 len)
{
    (void)ctx;

    if (len < ETH_HLEN + 2)
        return 0;
    u16 ethertype = net_be16(frame + 12);

    switch (ethertype) {
    case ETH_P_ARP:
        if (len >= ETH_HLEN + 28)
            arp_rx(frame + ETH_HLEN);
        break;
    case ETH_P_IPV4:
        if (len >= ETH_HLEN + 20)
            ipv4_rx(frame + ETH_HLEN, len - ETH_HLEN, frame + 6);
        break;
    default:
        break;
    }
    return 0;
}

int net_poll(void)
{
    const struct net_nic *nic;

    if (!g_up)
        return 0;
    nic = net_nic_active();
    if (!nic)
        return 0;
    return nic->poll(rx_cb, NULL);
}

/* --- time + interface state ------------------------------------------------------ */

u64 net_ticks(void)
{
    return core_time_ticks();
}

u64 net_ms_to_ticks(u32 ms)
{
    u64 hz = core_time_hz();

    if (hz == 0)
        return ms;
    return ((u64)ms * hz + 999u) / 1000u;
}

u32 net_ip_local(void)
{
    return g_ip_addr;
}

u32 net_ip_netmask(void)
{
    return g_ip_mask;
}

u32 net_ip_gateway(void)
{
    return g_ip_gw;
}

/* DHCP hands out the resolver explicitly; the slirp fallback (gateway+1)
 * keeps DNS usable when a server omits option 6. */
u32 net_ip_dns_server(void)
{
    if (g_ip_dns)
        return g_ip_dns;
    /* Try Google DNS */
    return NET_IPV4(8, 8, 8, 8);
}

void net_addr_set(u32 ip, u32 mask, u32 gw, u32 dns)
{
    g_ip_addr = ip;
    g_ip_mask = mask ? mask : NET_IP_MASK;
    g_ip_gw = gw;
    g_ip_dns = dns;
    net_arp_flush();
}

void net_mac_get(u8 mac[ETH_ALEN])
{
    memcpy(mac, g_mac, ETH_ALEN);
}

/* --- bring-up -------------------------------------------------------------------- */

u32 net_ip_gw(void)
{
    return g_ip_gw;
}

u32 net_ip_addr(void)
{
    return g_ip_addr;
}

int net_up(void)
{
    return g_up;
}

/* Quiet (re)start of the stack on the active NIC: open, bind its MAC,
 * flush the ARP/ICMP waiter state. Returns 0 when the stack is live. */
static int net_start(void)
{
    const struct net_nic *nic = net_nic_active();

    g_up = 0;
    if (!nic || nic->open() != 0)
        return -1;
    nic->get_mac(g_mac);
    memset(g_arp_cache, 0, sizeof(g_arp_cache));
    memset(&g_icmp_wait, 0, sizeof(g_icmp_wait));
    net_sock_reset_all();
    net_dhcp_reset();
    g_ip_id = 0;
    g_up = 1;
    return 0;
}

/* Ktests switch adapters by name (ARP/ICMP answers travel the slirp
 * network the selected card is on, so the cache is flushed per switch);
 * net_start_nic(NULL) restores the boot default without a banner. */
int net_start_nic(const char *name)
{
    if (net_nic_select(name) != 0)
        return -1;
    if (net_start() != 0)
        return -1;
    if (name)
        printk("net: switched to %s, mac %02x:%02x:%02x:%02x:%02x:%02x\n",
               name, g_mac[0], g_mac[1], g_mac[2],
               g_mac[3], g_mac[4], g_mac[5]);
    return 0;
}

void net_init(void)
{
    if (net_start() != 0) {
        pr_info("net: no NIC, stack dormant\n");
        return;
    }

    printk("net: up over %s, mac %02x:%02x:%02x:%02x:%02x:%02x, "
           "ip %u.%u.%u.%u mask %u.%u.%u.%u gw %u.%u.%u.%u (poll mode)\n",
           net_nic_active()->name,
           g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5],
           (g_ip_addr >> 24) & 0xFF, (g_ip_addr >> 16) & 0xFF,
           (g_ip_addr >> 8) & 0xFF, g_ip_addr & 0xFF,
           (g_ip_mask >> 24) & 0xFF, (g_ip_mask >> 16) & 0xFF,
           (g_ip_mask >> 8) & 0xFF, g_ip_mask & 0xFF,
           (g_ip_gw >> 24) & 0xFF, (g_ip_gw >> 16) & 0xFF,
           (g_ip_gw >> 8) & 0xFF, g_ip_gw & 0xFF);
    g_ip_dns = net_ip_dns_server();
    g_up = 1;
    wifi_init();
}
