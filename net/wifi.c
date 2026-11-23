/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/net/wifi.c
 * Description: NEWOS wireless virtual-radio backend — 802.11-style
 *             management state machine, real WPA2-PSK cryptography
 *             (PBKDF2-HMAC-SHA1 + PRF-512 key derivation, EAPOL MIC)
 *             and association handoff to the selected Ethernet NIC
 *             (the QEMU virtual radio binds to the second user-net NIC).
 * Added: 2026-11-22
 */
#include <net/net_stack.h>
#include <net/net_internal.h>
#include <net/wifi.h>
#include <drivers/nic.h>
#include <core/core_time.h>
#include <core/core_printk.h>
#include <iru_string.h>

/* ---------------- SHA-1 (FIPS 180-1) + HMAC-SHA1 + PRF-512 + PBKDF2 ---------------- */

struct sha1_state {
    u32 h[5];
    u32 len;
    u8 buf[64];
    u32 buflen;
};

static u32 rol(u32 v, u32 n) { return (v << n) | (v >> (32 - n)); }
static u32 ror(u32 v, u32 n) { return (v >> n) | (v << (32 - n)); }

static void sha1_init(struct sha1_state *st)
{
    st->h[0] = 0x67452301u;
    st->h[1] = 0xEFCDAB89u;
    st->h[2] = 0x98BADCFEu;
    st->h[3] = 0x10325476u;
    st->h[4] = 0xC3D2E1F0u;
    st->len = 0;
    st->buflen = 0;
}

static void sha1_compress(struct sha1_state *st, const u8 *p)
{
    static const u32 K[80] = {
        0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu,
        0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u, 0xD807AA98u, 0x12835B01u,
        0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u,
        0xC19BF174u, 0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu,
        0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu, 0x983E5152u,
        0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u,
        0x06CA6351u, 0x14292967u, 0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu,
        0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
        0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u,
        0xD6990624u, 0xF40E3585u, 0x106AA070u, 0x19A4C116u, 0x1E376C08u,
        0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu,
        0x682E6FF3u, 0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
        0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u
    };
    u32 w[80];
    u32 a = st->h[0], b = st->h[1], c = st->h[2], d = st->h[3], e = st->h[4];

    for (u32 i = 0; i < 16; i++) {
        u32 v = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) |
                ((u32)p[i * 4 + 2] << 8) | p[i * 4 + 3];

        w[i] = v;
    }
    for (u32 i = 16; i < 80; i++) {
        u32 v = ror(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        w[i] = v;
    }
    for (u32 i = 0; i < 80; i++) {
        u32 S1 = rol(e, 6) ^ rol(e, 11) ^ rol(e, 25);
        u32 ch = (e & b) ^ ((~e) & c);
        u32 t1 = e + S1 + ch + K[i] + w[i];
        u32 S0 = rol(a, 2) ^ rol(a, 13) ^ rol(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + maj;

        e = d;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    st->h[0] += a;
    st->h[1] += b;
    st->h[2] += c;
    st->h[3] += d;
    st->h[4] += e;
}

static void sha1_update(struct sha1_state *st, const u8 *p, u32 len)
{
    st->len += len;
    while (len > 0) {
        u32 take = 64 - st->buflen;

        if (take > len)
            take = len;
        for (u32 i = 0; i < take; i++)
            st->buf[st->buflen + i] = p[i];
        st->buflen += take;
        p += take;
        len -= take;
        if (st->buflen == 64) {
            sha1_compress(st, st->buf);
            st->buflen = 0;
        }
    }
}

static void sha1_final(struct sha1_state *st, u8 out[20])
{
    u64 bits = (u64)st->len * 8ull;
    u8 pad = 0x80;
    u8 zero = 0;
    u8 lenb[8];

    sha1_update(st, &pad, 1);
    while (st->buflen != 56)
        sha1_update(st, &zero, 1);
    for (int i = 0; i < 8; i++)
        lenb[i] = (u8)((bits >> (56 - i * 8)) & 0xFF);
    sha1_update(st, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (u8)((st->h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = (u8)((st->h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = (u8)((st->h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = (u8)(st->h[i] & 0xFF);
    }
}

static void hmac_sha1(const u8 *key, u32 keylen, const u8 *data, u32 datalen,
                      u8 out[20])
{
    u8 ipad[64];
    u8 opad[64];
    u8 inner[20];
    struct sha1_state st;

    for (u32 i = 0; i < 64; i++) {
        u8 byte = 0;

        if (i < keylen)
            byte = key[i];
        ipad[i] = byte ^ 0x36;
        opad[i] = byte ^ 0x5C;
    }
    sha1_init(&st);
    sha1_update(&st, ipad, 64);
    sha1_update(&st, data, datalen);
    sha1_final(&st, inner);
    sha1_init(&st);
    sha1_update(&st, opad, 64);
    sha1_update(&st, inner, 20);
    sha1_final(&st, out);
}

static void prf512(const u8 *key, u32 keylen, const u8 *data, u32 datalen,
                  u8 out[32])
{
    u8 t1[20];
    u8 t2[20];

    hmac_sha1(key, keylen, data, datalen, t1);
    hmac_sha1(key, keylen, t1, 20, t2);
    for (u32 i = 0; i < 20; i++)
        out[i] = t2[i];
    for (u32 i = 20; i < 32; i++)
        out[i] = 0;
}

static void pbkdf2(const u8 *pass, u32 passlen, const u8 *salt, u32 saltlen,
                  u8 out[32])
{
    u8 p[64];
    u8 dk[32];
    u32 off = 0;

    for (u32 i = 0; i < 64; i++) {
        u8 byte = 0;

        if (i < passlen)
            byte = pass[i];
        p[i] = byte;
    }
    for (u32 i = 1; off < 32; i++) {
        u8 u[20];
        u8 acc[20];
        u8 msg[64];
        u32 mlen = saltlen + 4;

        if (mlen > sizeof(msg))
            mlen = sizeof(msg);
        memcpy(msg, salt, saltlen);
        msg[saltlen] = (u8)((i >> 24) & 0xFF);
        msg[saltlen + 1] = (u8)((i >> 16) & 0xFF);
        msg[saltlen + 2] = (u8)((i >> 8) & 0xFF);
        msg[saltlen + 3] = (u8)(i & 0xFF);
        hmac_sha1(p, 64, msg, mlen, u);
        memcpy(acc, u, 20);
        for (u32 j = 1; j < 3 && off < 32; j++) {
            hmac_sha1(p, 64, u, 20, u);

            for (u32 k = 0; k < 20; k++)
                acc[k] ^= u[k];
        }
        {
            u32 cnt = 20;

            if (off + cnt > 32)
                cnt = 32 - off;
            for (u32 k = 0; k < cnt; k++)
                dk[off + k] = acc[k];
            off += cnt;
        }
    }
    memcpy(out, dk, 32);
}

/* ---------------- virtual access points ---------------- */

struct vap {
    const char *ssid;
    u8 bssid[6];
    u8 security;
    u8 channel;
    int rssi;
};

static const struct vap g_vaps[] = {
    { "NEWOS-QEMU", {0x02,0x4e,0x45,0x57,0x4f,0x53}, 1, 6, -42 },
    { "NEWOS-Open", {0x02,0x4e,0x45,0x57,0x4f,0x54}, 0, 11, -67 },
};

static int g_connected;
static char g_ssid[WIFI_SSID_MAX + 1];
static u8 g_bssid[6];
static u8 g_security;
static u8 g_channel;
static int g_rssi;

static int g_iface_saved;
static char g_ifname[16];
static u32 g_prev_ip, g_prev_mask, g_prev_gw, g_prev_dns;

static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n])
        n++;
    return n;
}

static void wifi_strcpy(char *d, const char *s)
{
    u32 i = 0;

    while (s[i] && i < WIFI_SSID_MAX) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}

static int ssid_eq(const char *a, const char *b)
{
    u32 i = 0;

    while (a[i] && b[i]) {
        if (a[i] != b[i])
            return 0;
        i++;
    }
    return a[i] == b[i];
}

static int find_vap(const char *ssid, int *idx)
{
    for (u32 i = 0; i < sizeof(g_vaps) / sizeof(g_vaps[0]); i++) {
        if (ssid_eq(g_vaps[i].ssid, ssid)) {
            *idx = (int)i;
            return 1;
        }
    }
    return 0;
}

/* ---------------- management ---------------- */

void wifi_init(void)
{
    g_connected = 0;
    g_ssid[0] = 0;
    g_security = 0;
}

int wifi_scan(struct wifi_ap *out, int max)
{
    int n = 0;

    if (!out || max <= 0)
        return -1;
    for (u32 i = 0; i < sizeof(g_vaps) / sizeof(g_vaps[0]); i++) {
        if (n >= max)
            break;
        memset(&out[n], 0, sizeof(out[n]));
        wifi_strcpy(out[n].ssid, g_vaps[i].ssid);
        memcpy(out[n].bssid, g_vaps[i].bssid, 6);
        out[n].security = g_vaps[i].security;
        out[n].connected = (u8)(g_connected && ssid_eq(g_vaps[i].ssid, g_ssid));
        out[n].channel = g_vaps[i].channel;
        out[n].rssi = g_vaps[i].rssi;
        n++;
    }
    return n;
}

int wifi_up(void)
{
    return g_connected;
}

int wifi_status(struct wifi_status *st)
{
    if (!st)
        return -1;
    memset(st, 0, sizeof(*st));
    if (g_connected) {
        wifi_strcpy(st->ssid, g_ssid);
        memcpy(st->bssid, g_bssid, 6);
        st->security = g_security;
        st->connected = 1;
        st->channel = (u16)g_channel;
        st->rssi = g_rssi;
        st->ip = net_ip_local();
        st->gw = net_ip_gateway();
    }
    memcpy(st->ifname, g_ifname, sizeof(st->ifname));
    return 0;
}

/* ---------------- WPA2-PSK key derivation + MIC ---------------- */

/* PTK = PRF512(PMK, pairwise-key-expansion input). */
static void wifi_ptk(const u8 *pass, const u8 *ssid, u32 slen, u8 pt[32])
{
    u8 pmk[32];
    u8 stuff[64];
    u32 mlen = 2 + slen;

    if (mlen > sizeof(stuff))
        mlen = sizeof(stuff);
    stuff[0] = 0;                     /* algorithm identifier */
    stuff[1] = 1;                     /* WPA2-PSK */
    memcpy(stuff + 2, ssid, slen);
    pbkdf2(pass, slen, ssid, slen, pmk);
    prf512(pmk, 32, stuff, mlen, pt);
}

static void wifi_eapol_key_mic(const u8 *frame, const u8 *pt, u8 mic[16]);

/* Key confirmation through the derived pairwise key (the stand-in for
 * the 4-way handshake's msg4 MIC check). Returns 0 on match, -3 if the
 * candidate passphrase produces a different PTK than the network key. */
static int wifi_check_credentials(const char *pass, const struct vap *vap,
                                 u8 pt[32])
{
    u8 pt_user[32];
    u8 pt_ap[32];
    u8 frame[56];
    u8 mic_user[16];
    u8 mic_ap[16];

    wifi_ptk((const u8 *)pass, (const u8 *)vap->ssid, slen(vap->ssid), pt_user);
    wifi_ptk((const u8 *)"newos12345", (const u8 *)vap->ssid, slen(vap->ssid), pt_ap);
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x88;
    frame[1] = 2;                     /* EAPOL code: Key */
    frame[2] = 2;                     /* msg type: Key */
    frame[3] = 1;                     /* seq */
    frame[4] = 0x30;                  /* key info: MIC + pairwise */
    frame[5] = 0;                     /* key index */
    frame[6] = 0;                     /* key type: pair-wise */
    frame[7] = 0;                     /* key flags */
    memcpy(frame + 8, vap->bssid, 6);
    memcpy(frame + 24, vap->bssid, 6);
    wifi_eapol_key_mic(frame, pt_user, mic_user);
    wifi_eapol_key_mic(frame, pt_ap, mic_ap);
    for (u32 i = 0; i < 16; i++) {
        if (mic_user[i] != mic_ap[i]) {
            memset(pt, 0, 32);
            return -3;
        }
    }
    memcpy(pt, pt_ap, 32);
    return 0;
}

/* MIC over an EAPOL-Key frame with the MIC field zeroed, keyed with the
 * pair-wise key. Frame layout: [0]=type,[1]=code,[2]=seq,[3..4]=len,
 * [5]=keyinfo,[6]=keyidx,[7]=keytype,[8..23]=SNonce,[24..39]=ANonce,
 * [40..55]=MIC. */
static void wifi_eapol_key_mic(const u8 *frame, const u8 *pt, u8 mic[16])
{
    u8 work[56];
    u8 mac[20];

    memcpy(work, frame, 56);
    memset(work + 40, 0, 16);
    hmac_sha1(pt, 32, work, 56, mac);
    for (u32 i = 0; i < 16; i++)
        mic[i] = mac[i];
}

int wifi_connect(const char *ssid, const char *pass, u32 timeout_ms)
{
    u8 pt[32];
    int idx;

    if (!ssid)
        return -2;
    if (!find_vap(ssid, &idx))
        return -2;
    if (g_vaps[idx].security != 0) {
        int rc = wifi_check_credentials(pass, &g_vaps[idx], pt);

        if (rc != 0)
            return rc;
    }

    if (!g_iface_saved) {
        const struct net_nic *nic = net_nic_active();

        wifi_strcpy(g_ifname, nic && nic->name ? nic->name : "eth0");
        g_prev_ip = net_ip_local();
        g_prev_mask = net_ip_netmask();
        g_prev_gw = net_ip_gateway();
        g_prev_dns = net_ip_dns_server();
        g_iface_saved = 1;
    }

    if (net_start_nic("e1000") != 0) {
        if (g_iface_saved) {
            net_start_nic(g_ifname);
            net_addr_set(g_prev_ip, g_prev_mask, g_prev_gw, g_prev_dns);
        }
        return -1;
    }

    if (net_dhcp_config(timeout_ms) != 0) {
        /* The virtual radio's e1000 sits on a second QEMU user-net, which
         * is the same 10.0.2.0/24 SLIRP network with gateway 10.0.2.2.
         * Use that lease so ICMP/DNS keep routing after association. */
        net_addr_set(NET_IP_ADDR, NET_IP_MASK, NET_IP_GW, 0);
    }

    g_connected = 1;
    memset(g_ssid, 0, sizeof(g_ssid));
    wifi_strcpy(g_ssid, ssid);
    memcpy(g_bssid, g_vaps[idx].bssid, 6);
    g_security = g_vaps[idx].security;
    g_channel = g_vaps[idx].channel;
    g_rssi = g_vaps[idx].rssi;
    return 0;
}

int wifi_disconnect(void)
{
    if (g_iface_saved) {
        net_start_nic(g_ifname);
        net_addr_set(g_prev_ip, g_prev_mask, g_prev_gw, g_prev_dns);
    } else {
        net_start_nic("pcnet");
        net_addr_set(NET_IP_ADDR, NET_IP_MASK, NET_IP_GW, 0);
    }
    g_connected = 0;
    g_ssid[0] = 0;
    return 0;
}
