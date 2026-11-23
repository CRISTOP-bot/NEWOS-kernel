/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/net/net_dns.c
 * Description: NEWOS stub DNS resolver (A records) over the UDP transport.
 *             Sends a minimal DNS query and reads the first A answer.
 * Added: 2026-11-22
 */
#include <net/net_stack.h>
#include <net/net_internal.h>
#include <core/core_time.h>
#include <iru_string.h>

/* Encode a plain (uncompressed) name into out; returns its total length
 * or 0 on overflow. */
static u32 dns_enc_name(u8 *out, const char *name)
{
    u32 o = 0;
    const char *p = name;

    while (*p) {
        const char *start = p;
        while (*p && *p != '.')
            p++;
        if (o + 1 + (u32)(p - start) + 1 > 64)
            return 0;
        out[o++] = (u8)((u32)(p - start) & 0xFF);
        for (const char *q = start; q < p; q++)
            out[o++] = (u8)*q;
        if (*p == '.')
            p++;
    }
    /* Terminate the name with a 0-length label */
    if (o > 0)
        out[o++] = 0;
    return o;
}

int net_dns_query(int handle, const char *name, u32 *ip, u32 timeout_ms)
{
    struct net_socket *sk;
    static u8 q[12 + 64];
    static u8 buf[512];
    u32 qlen, qend, an, type, rdlen, rdata;
    u16 id, ancount;

    sk = net_sock_get(handle, 0);
    if (!sk || sk->type != NET_SOCK_UDP)
        return -1;
    if (!name || !*name || !ip)
        return -1;

    qlen = dns_enc_name(q + 12, name);
    if (qlen == 0)
        return -1;
    qend = 12 + qlen + 4;
    /* QTYPE = A (1), QCLASS = IN (1) */
    net_put_be16(q + 12 + qlen, 1);
    net_put_be16(q + 12 + qlen + 2, 1);

    id = (u16)((u32)net_ticks() & 0xFFFFu);
    q[0] = (u8)(id >> 8);
    q[1] = (u8)(id & 0xFF);
    q[2] = 0;                              /* QR: query */
    q[3] = 0x00;                           /* RD=0 */
    q[4] = 0;                              /* opcode */
    q[5] = 1;                              /* QDCOUNT */
    q[6] = 0;                              /* ANCOUNT */
    q[7] = 0;                              /* NSCOUNT */
    q[8] = 0;                              /* ARCOUNT */
    q[9] = 0;

    if (net_udp_sendto(sk, sk->rip, sk->rport, q, qend, timeout_ms) < 0)
        return -1;


    {
        int n = net_sock_recv(handle, 0, buf, (u32)sizeof(buf), timeout_ms);
        if (n > 0) {
        }

        if (n <= 0)
            return -1;
        if ((u32)n < qend)
            return -1;
        ancount = (u16)(((u32)buf[6] << 8) | buf[7]);
        /* Skip the question section: parse name + QTYPE + QCLASS from response */
        {
            u32 qpos = 12;
            /* Skip question name (handle compression) */
            while (qpos < (u32)n) {
                if ((buf[qpos] & 0xC0) == 0xC0) {
                    qpos += 2;
                    break;
                }
                if (buf[qpos] == 0) {
                    qpos++;
                    break;
                }
                qpos += 1 + buf[qpos];
            }
            qpos += 4; /* QTYPE + QCLASS */
            an = qpos;
        }

        for (u16 i = 0; i < ancount; i++) {
            /* Parse the name (handle compression) */
            u32 npos = an;
            while (npos < (u32)n) {
                if ((buf[npos] & 0xC0) == 0xC0) {
                    npos += 2; /* compression pointer */
                    break;
                }
                if (buf[npos] == 0) {
                    npos++;
                    break;
                }
                npos += 1 + buf[npos];
            }
            an = npos; /* past name */

            /* Parse this answer record */
            if (an + 10 > (u32)n)
                return -1;
            type = ((u32)buf[an + 0] << 8) | buf[an + 1];
            rdlen = ((u32)buf[an + 8] << 8) | buf[an + 9];
            rdata = an + 10;
            if (type == 1 && rdlen >= 4) {
                /* A record */
                *ip = ((u32)buf[rdata] << 24) | ((u32)buf[rdata + 1] << 16) |
                      ((u32)buf[rdata + 2] << 8) | buf[rdata + 3];
                return 0;
            }
            /* Skip this record: type(2) + class(2) + TTL(4) + rdlen(2) + rdata */
            an += 2 + 2 + 4 + 2 + rdlen;
        }}
    return -1;
}

int net_dns_resolve(const char *name, u32 *ip, u32 timeout_ms)
{
    int s = net_sock_open(NET_SOCK_UDP, 0);

    if (s == -1) {
        return -1;
    }
    if (net_sock_connect(s, 0, net_ip_dns_server(), 53,
                        timeout_ms ? timeout_ms : 3000) != 0) {
        net_sock_close(s, 0);
        return -1;
    }
    {
        int rc = net_dns_query(s, name, ip, timeout_ms ? timeout_ms : 3000);

        net_sock_close(s, 0);
        return rc;
    }
}
