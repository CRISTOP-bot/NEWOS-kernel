/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/net/wifi.h
 * Description: NEWOS wireless API (virtual-radio backend over the NIC
 *             registry). The scan/status structs mirror the ABI records
 *             field for field (same widths) so the SYS_WIFI_* copies stay
 *             byte-exact; they live here, in the net domain, instead of
 *             reaching into the abi tier.
 * Added: 2026-11-22
 */
#ifndef NET_WIFI_H
#define NET_WIFI_H

#include <core/core_types.h>

#define WIFI_SSID_MAX 32
#define WIFI_PASS_MAX 64

/* One visible access point. `security`: 0 open, 1 WPA2-PSK. */
struct wifi_ap {
    char    ssid[WIFI_SSID_MAX];
    u8      bssid[6];
    u8      security;
    u8      connected;
    u16     channel;
    int     rssi;                     /* dBm, negative */
};

/* Association state + the addresses DHCP handed out. */
struct wifi_status {
    char    ssid[WIFI_SSID_MAX];
    char    ifname[16];
    u8      bssid[6];
    u8      security;
    u8      connected;
    u8      reserved;
    u16     channel;
    int     rssi;
    u32     ip;
    u32     gw;
};

/* WIFI: the virtual radio. wifi_scan() lists the channels the virtual
 * radio can see; wifi_connect()/wifi_disconnect() drive association
 * state; wifi_status() snapshots it. The radio backend itself is a NIC
 * registered through the driver tier (the QEMU user-net NIC bound as a
 * wireless radio). */
void wifi_init(void);
int wifi_scan(struct wifi_ap *out, int max);
int wifi_connect(const char *ssid, const char *pass, u32 timeout_ms);
int wifi_disconnect(void);
int wifi_status(struct wifi_status *st);
int wifi_up(void);

#endif
