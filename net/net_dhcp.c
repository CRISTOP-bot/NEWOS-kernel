/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/net/net_dhcp.c
 * Description: NEWOS DHCP client state machine over the UDP transport
 *             (DISCOVER -> OFFER -> REQUEST -> ACK) feeding the IPv4
 *             stack state via net_addr_set().
 * Added: 2026-11-22
 */
#include <net/net_stack.h>
#include <net/net_internal.h>
#include <core/core_time.h>
#include <iru_string.h>

#define DHCP_MAGIC        0x8000u
#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5

/* pkt layout: UDP header at 0..7, BOOTP at 8.. with magic at BOOTP+236
 * (pkt+244), option 53 at BOOTP+238 (pkt+246), options end around +248. */
struct dhcp_state {
    int active;
    u32 xid;
    int next_msg;
    u32 yiaddr;
    u32 mask;
    u32 router;
    u32 dns;
};

static struct dhcp_state g_dhcp;

static int dhcp_opt_u32(const u8 *pkt, u32 len, u8 want, u32 *out)
{
    u32 off = 8 + 238;

    while (off + 2 < len) {
        u8 t = pkt[off];
        u8 l = pkt[off + 1];
        u32 v = 0;

        if (t == 255)
            break;
        if (off + 2 + l > len)
            break;
        if (t == want && l == 4) {
            v = net_be32(pkt + off + 2);
            *out = v;
            return 1;
        }
        off += 2 + l;
    }
    return 0;
}

/* Fill a UDP/BOOTP DISCOVER or REQUEST packet. */
static void dhcp_build(u8 *pkt, int msg_type, u32 xid, u32 yiaddr)
{
    u32 o = 8;                     /* BOOTP start inside the UDP segment */

    memset(pkt, 0, 8 + 260);
    net_put_be16(pkt + 0, 68);                    /* src port: client */
    net_put_be16(pkt + 2, 67);                    /* dst port: server */
    pkt[o] = 1;                                    /* op: BOOTREQUEST */
    pkt[o + 1] = 1;                                /* htype: Ethernet */
    pkt[o + 2] = 6;                                /* hlen: 6 */
    pkt[o + 3] = 0;                                /* hops: 0 */
    net_put_be32(pkt + o + 4, xid);               /* xid */
    net_put_be16(pkt + o + 10, 0x8000);          /* flags: broadcast */
    net_put_be32(pkt + o + 16, yiaddr);          /* yiaddr */
    net_put_be16(pkt + o + 236, (u16)DHCP_MAGIC);
    pkt[o + 238] = 53;                            /* option: DHCP */
    pkt[o + 239] = 1;                             /* len */
    pkt[o + 240] = (u8)msg_type;
    pkt[o + 241] = 55;                            /* option: param list */
    pkt[o + 242] = 3;                             /* len */
    pkt[o + 243] = 1;
    pkt[o + 244] = 2;
    pkt[o + 245] = 3;
    pkt[o + 246] = 255;                           /* end of options */

    net_put_be16(pkt + 4, 8 + 248);              /* udp length */
    {
        u16 csum = net_checksum_pseudo(0, 0xFFFFFFFFu, NET_IP_PROTO_UDP,
                                      pkt, 8 + 248);

        net_put_be16(pkt + 6, csum ? csum : 0xFFFF);
    }
}

void net_dhcp_reset(void)
{
    g_dhcp.active = 0;
    g_dhcp.xid = 0;
    g_dhcp.next_msg = 0;
    g_dhcp.yiaddr = 0;
    g_dhcp.mask = 0;
    g_dhcp.router = 0;
    g_dhcp.dns = 0;
}

void net_dhcp_input(const u8 *pkt, u32 len)
{
    u32 xid, yiaddr, mask = 0, router = 0, dns = 0;
    u8 msg;

    if (!g_dhcp.active)
        return;
    if (!pkt || len < 8 + 240)
        return;
    if (net_be16(pkt + 8 + 236) != (u16)DHCP_MAGIC)
        return;
    xid = net_be32(pkt + 8 + 4);
    if (xid != g_dhcp.xid)
        return;
    msg = pkt[8 + 238 + 2];
    yiaddr = net_be32(pkt + 8 + 16);

    dhcp_opt_u32(pkt, len, 1, &mask);
    dhcp_opt_u32(pkt, len, 3, &router);
    dhcp_opt_u32(pkt, len, 6, &dns);

    g_dhcp.yiaddr = yiaddr;

    if (msg == DHCP_MSG_OFFER) {
        g_dhcp.next_msg = DHCP_MSG_REQUEST;
        return;
    }
    if (msg == DHCP_MSG_REQUEST) {
        g_dhcp.next_msg = DHCP_MSG_ACK;
        return;
    }
    if (msg == DHCP_MSG_ACK) {
        g_dhcp.active = 0;
        net_addr_set(yiaddr, mask, router, dns);
    }
}

int net_dhcp_config(u32 timeout_ms)
{
    static u8 pkt[8 + 260];
    u64 deadline;

    if (!net_up())
        return -1;

    net_dhcp_reset();
    g_dhcp.active = 1;
    g_dhcp.xid = (u32)net_ticks();
    g_dhcp.next_msg = DHCP_MSG_DISCOVER;
    g_dhcp.yiaddr = 0;
    g_dhcp.mask = 0;
    g_dhcp.router = 0;
    g_dhcp.dns = 0;

    dhcp_build(pkt, DHCP_MSG_DISCOVER, g_dhcp.xid, 0);
    if (net_ip_send(0xFFFFFFFFu, NET_IP_PROTO_UDP, pkt,
                    (u32)(8 + 248), timeout_ms) != 0) {
        net_dhcp_reset();
        return -1;
    }

    deadline = net_ticks() + net_ms_to_ticks(timeout_ms ? timeout_ms : 4000);
    while (net_ticks() < deadline) {
        net_poll();
        if (g_dhcp.next_msg == DHCP_MSG_OFFER)
            break;
        if (net_ticks() >= deadline)
            break;
        core_time_delay_us(1000);
    }
    if (g_dhcp.next_msg != DHCP_MSG_OFFER)
        goto fail;

    dhcp_build(pkt, DHCP_MSG_REQUEST, g_dhcp.xid, g_dhcp.yiaddr);
    if (net_ip_send(0xFFFFFFFFu, NET_IP_PROTO_UDP, pkt,
                    (u32)(8 + 248), timeout_ms) != 0)
        goto fail;

    deadline = net_ticks() + net_ms_to_ticks(timeout_ms ? timeout_ms : 6000);
    while (net_ticks() < deadline) {
        net_poll();
        if (g_dhcp.next_msg == DHCP_MSG_ACK)
            break;
        if (net_ticks() >= deadline)
            break;
        core_time_delay_us(1000);
    }
    if (g_dhcp.next_msg != DHCP_MSG_ACK)
        goto fail;

    return 0;

fail:
    net_dhcp_reset();
    return -1;
}
