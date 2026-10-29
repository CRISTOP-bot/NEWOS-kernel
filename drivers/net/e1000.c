/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/drivers/net/e1000.c
 * Description: Intel 82540EM (QEMU -device e1000) poll-mode Ethernet driver.
 * Added: 2026-10-22
 */
#include <drivers/e1000.h>
#include <drivers/nic.h>
#include <drivers/pci.h>
#include <drivers/drv_core.h>
#include <core/core.h>
#include <core/core_printk.h>
#include <mm/mm_pmm.h>
#include <iru_string.h>
#include <x86_cpu.h>
#include <x86_devmap.h>

/* Intel 82540EM Ethernet controller driver.
 *
 * Register model (mirrors the QEMU e1000 emulation, which follows the
 * 8254x software developer's manual): the 128 KiB control plane lives
 * on memory BAR0. Rings are standard 16-byte legacy descriptors —
 * the classic RX buffer model (RDBA ring, guest-owned per-buffer
 * addresses, RCTLEN + RDT tail).
 *
 * DMA model: RX descriptors point at pre-armed 2 KiB buffers and the
 * card flips each descriptor's DD status when its copy lands, EOP on
 * the last chunk (all slirp frames fit one descriptor). TX descriptors
 * point at one scratch buffer, like the PCnet driver; the card
 * consumes TDH->TDT synchronously on the TDT MMIO write and reports
 * completion by setting DD when RS was requested. Both sides poll:
 * no MSI/IRQ is installed. */

#define E1000_REG_CTRL      0x0000u
#define E1000_REG_STATUS    0x0008u
#define E1000_REG_RCTL      0x0100u
#define E1000_REG_TCTL      0x0400u
#define E1000_REG_TIPG      0x0410u
#define E1000_REG_RDBAL     0x2800u
#define E1000_REG_RDBAH     0x2804u
#define E1000_REG_RDLEN     0x2808u
#define E1000_REG_RDH       0x2810u
#define E1000_REG_RDT       0x2818u
#define E1000_REG_TDBAL     0x3800u
#define E1000_REG_TDBAH     0x3804u
#define E1000_REG_TDLEN     0x3808u
#define E1000_REG_TDH       0x3810u
#define E1000_REG_TDT       0x3818u
#define E1000_REG_RAL       0x5400u
#define E1000_REG_RAH       0x5404u

#define E1000_STATUS_LU     (1u << 1)
#define E1000_RCTL_EN       0x00000002u
#define E1000_RCTL_BAM      0x00008000u
#define E1000_RCTL_SZ_2048  0x00000000u
#define E1000_TCTL_EN       0x00000002u
#define E1000_TCTL_PSP      0x00000008u
#define E1000_TCTL_CT       0x00000ff0u
#define E1000_TCTL_COLD     0x003ff000u
#define E1000_RAH_AV        0x80000000u

/* legacy descriptor bits (DD lives in the upper dword's status byte,
 * bits [7:0] — same layout Linux's fields.status reads) */
#define E1000_TXD_CMD_EOP   0x01000000u
#define E1000_TXD_CMD_IFCS  0x02000000u
#define E1000_TXD_CMD_RS    0x08000000u
#define E1000_TXD_STAT_DD   0x00000001u
#define E1000_RXD_STAT_DD   0x01u
#define E1000_RXD_STAT_EOP  0x02u

#define E1000_RX_RING       16u
#define E1000_TX_RING       8u
#define E1000_RX_BUF_SIZE   2048u
#define E1000_ETH_MIN_FRAME 60u

/* DMA layout inside one 4-order physical block (35 200 bytes of 64 KiB):
 * 0x000 rx_desc[16]  (256)  -> 0x100
 * 0x100 tx_desc[8]   (128)  -> 0x180
 * 0x180 rx_buf[16][2048]    -> 0x8180
 * 0x8180 tx_buf[2048]       -> 0x8980 */
#define E1000_DMA_OFF_RX_DESC 0x000u
#define E1000_DMA_OFF_TX_DESC 0x100u
#define E1000_DMA_OFF_RX_BUF  0x180u
#define E1000_DMA_OFF_TX_BUF  0x8180u

struct e1000_tx_desc {
    u64 addr;
    u32 lower;              /* length:16 | cso:8 | cmd:8 */
    u32 upper;              /* status:8 | css:8 | special:16 */
} __packed;

struct e1000_rx_desc {
    u64 addr;
    u16 length;
    u16 csum;
    u8 status;
    u8 errors;
    u16 special;
} __packed;

struct e1000_dma {
    struct e1000_rx_desc rx[E1000_RX_RING];
    struct e1000_tx_desc tx[E1000_TX_RING];
    u8 rx_buf[E1000_RX_RING][E1000_RX_BUF_SIZE];
    u8 tx_buf[E1000_RX_BUF_SIZE];
} __packed;

_Static_assert(sizeof(struct e1000_dma) == 0x8980,
               "e1000 DMA block layout");

struct e1000_inst {
    volatile u8 *mm;
    u8 mac[6];
    u64 dma_phys;
    struct e1000_dma *dma;
    int present;
    unsigned rx_next;       /* guest cursor: first descriptor to check */
    unsigned tx_next;       /* next TX slot to fill */
};

static struct e1000_inst g_e1000;

static inline u32 e1000_r32(u32 off)
{
    return *(volatile u32 *)(g_e1000.mm + off);
}

static inline void e1000_w32(u32 off, u32 v)
{
    *(volatile u32 *)(g_e1000.mm + off) = v;
}

static void e1000_tx_arm(unsigned slot, u64 phys, u16 len)
{
    struct e1000_tx_desc *d;

    d = &g_e1000.dma->tx[slot];
    d->addr = phys;
    d->lower = (u32)len |
               (E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS);
    d->upper = 0;
    cpu_mfence();
    /* TDT is the "next free" pointer: one past the last descriptor
     * handed to hardware. TDT == TDH means "nothing to do", so the
     * first frame must announce slot 0 as TDT=1. */
    e1000_w32(E1000_REG_TDT, (slot + 1) % E1000_TX_RING);
}

int e1000_open(void)
{
    return g_e1000.present ? 0 : -1;
}

void e1000_get_mac(u8 mac[6])
{
    memcpy(mac, g_e1000.mac, 6);
}

int e1000_send(const u8 *frame, u32 len)
{
    unsigned slot;

    if (!g_e1000.present || !frame ||
        len < E1000_ETH_MIN_FRAME || len > E1000_RX_BUF_SIZE)
        return -1;

    /* tx_arm() reinitializes the whole descriptor (including clearing a
     * leftover DD from the previous owner of the slot), so no stale-ring
     * gate is needed: arm + bounded DD poll is the contract. */
    slot = g_e1000.tx_next;

    memcpy(g_e1000.dma->tx_buf, frame, len);
    cpu_mfence();
    e1000_tx_arm(slot, g_e1000.dma_phys + E1000_DMA_OFF_TX_BUF, (u16)len);

    /* The emulated controller drains the ring inside the TDT write;
     * hardware needs a bounded wait for the RS status instead. */
    for (unsigned i = 0; i < 100000; i++) {
        if (g_e1000.dma->tx[slot].upper & (E1000_TXD_STAT_DD))
            break;
    }
    if (!(g_e1000.dma->tx[slot].upper & (E1000_TXD_STAT_DD))) {
        pr_warn("e1000: tx completion lost (upper 0x%08x)\n",
                g_e1000.dma->tx[slot].upper);
        return -1;          /* likely sent anyway; retry later */
    }
    g_e1000.dma->tx[slot].upper = 0;    /* clean the slot for the next lap */

    g_e1000.tx_next = (slot + 1) % E1000_TX_RING;
    return 0;
}

int e1000_poll(int (*cb)(void *ctx, const u8 *frame, u32 len), void *ctx)
{
    int delivered = 0;
    unsigned base = g_e1000.rx_next;

    if (!g_e1000.present)
        return 0;

    /* The card hands out descriptors in strict ring order: walk from
     * the guest cursor (snapshot BEFORE the loop — rx_next drifting
     * inside the loop would skip every other descriptor), consume
     * every DD-set one, re-arm it (fresh status) and rewrite RDT so
     * the card sees the new tail. Frames longer than one 2 KiB buffer
     * cannot reach us: slirp keeps everything within a single one. */
    for (unsigned i = 0; i < E1000_RX_RING; i++) {
        unsigned want = (base + i) % E1000_RX_RING;
        struct e1000_rx_desc *d = &g_e1000.dma->rx[want];

        if (!(d->status & E1000_RXD_STAT_DD))
            break;              /* next buffers still owned by the HW */
        if ((d->status & E1000_RXD_STAT_EOP) && cb) {
            u32 len = d->length;
            if (len >= E1000_ETH_MIN_FRAME && len <= E1000_RX_BUF_SIZE)
                cb(ctx, g_e1000.dma->rx_buf[want], len);
        }
        delivered++;

        /* Re-arm: same buffer, status cleared, RDT notes the tail. */
        d->status = 0;
        d->errors = 0;
        __asm__ volatile("" ::: "memory");
        e1000_w32(E1000_REG_RDT, want);
        g_e1000.rx_next = (want + 1) % E1000_RX_RING;
    }
    return delivered;
}

/* ------------------------------------------------------------------ *
 *  PCI binding
 * ------------------------------------------------------------------ */

static const struct device_id e1000_ids[] = {    { 0x8086, 0x100e, 0 },      /* Intel 82540EM (QEMU -device e1000) */
    { 0, 0, 0 },
};

int e1000_probe(struct device *dev)
{
    struct pci_dev_info *inf = dev->private;
    u32 bar, cmd, ral;
    u64 frame;

    if (!inf)
        return -1;
    bar = inf->bars[0];
    if (bar & PCI_BAR_FLAG_IO)
        return -1;              /* this controller is MMIO-only */

    cmd = pci_config_read16(inf->bus, inf->dev, inf->func, PCI_REG_COMMAND);
    pci_config_write16(inf->bus, inf->dev, inf->func, PCI_REG_COMMAND,
                       (u16)(cmd | PCI_CMD_MEM_ENABLE | PCI_CMD_MASTER_ENABLE));

    g_e1000.mm = devmap_map((u64)(bar & ~0xFu), 0x20000u);
    if (!g_e1000.mm) {
        pr_warn("e1000: cannot map BAR0\n");
        return -1;
    }
    if (!(e1000_r32(E1000_REG_STATUS) & E1000_STATUS_LU)) {
        pr_warn("e1000: no carrier (STATUS link down)\n");
        return -1;
    }

    if (phys_alloc_block(4, &frame)) {
        pr_warn("e1000: no DMA block for rings\n");
        return -1;
    }
    g_e1000.dma_phys = frame << PAGE_SHIFT;
    if ((u64)(g_e1000.dma_phys + sizeof(struct e1000_dma)) > 0x100000000ull) {
        pr_warn("e1000: DMA block outside 32-bit range\n");
        phys_free_block(frame, 1);
        return -1;
    }
    g_e1000.dma = (struct e1000_dma *)phys_to_virt(g_e1000.dma_phys);
    {
        u32 *w = (u32 *)g_e1000.dma;
        u32 n = (u32)(sizeof(*g_e1000.dma) / 4u);
        for (u32 i = 0; i < n; i++)
            w[i] = 0;
    }

    /* The MAC the board booted with: QEMU keeps it in RAL/RAH (AV set),
     * mirroring how the emulated device carries MAC through reset. */
    ral = e1000_r32(E1000_REG_RAL);
    g_e1000.mac[0] = (u8)ral;
    g_e1000.mac[1] = (u8)(ral >> 8);
    g_e1000.mac[2] = (u8)(ral >> 16);
    g_e1000.mac[3] = (u8)(ral >> 24);
    ral = e1000_r32(E1000_REG_RAH);
    g_e1000.mac[4] = (u8)ral;
    g_e1000.mac[5] = (u8)(ral >> 8);

    /* TX ring: descriptors point at the shared scratch on demand. */
    e1000_w32(E1000_REG_TDBAL, (u32)(g_e1000.dma_phys + E1000_DMA_OFF_TX_DESC));
    e1000_w32(E1000_REG_TDBAH, (u32)(g_e1000.dma_phys >> 32));
    e1000_w32(E1000_REG_TDLEN, (u32)(E1000_TX_RING * 16u));
    e1000_w32(E1000_REG_TCTL,
              E1000_TCTL_EN | E1000_TCTL_PSP | (0xFu << 4) | (0x280u << 12));

    /* RX ring: pre-arm every descriptor with a 2 KiB buffer, then RDT
     * notes the last handed one; RCTL last so traffic starts armed. */
    e1000_w32(E1000_REG_RDBAL, (u32)(g_e1000.dma_phys + E1000_DMA_OFF_RX_DESC));
    e1000_w32(E1000_REG_RDBAH, (u32)(g_e1000.dma_phys >> 32));
    e1000_w32(E1000_REG_RDLEN, (u32)(E1000_RX_RING * 16u));
    for (u32 i = 0; i < E1000_RX_RING; i++) {
        struct e1000_rx_desc *d = &g_e1000.dma->rx[i];

        d->addr = g_e1000.dma_phys + E1000_DMA_OFF_RX_BUF +
                  (u64)i * E1000_RX_BUF_SIZE;
        d->length = 0;
        d->csum = 0;
        d->status = 0;
        d->errors = 0;
        d->special = 0;
    }
    e1000_w32(E1000_REG_RDT, E1000_RX_RING - 1);
    e1000_w32(E1000_REG_RCTL,
              E1000_RCTL_EN | E1000_RCTL_SZ_2048 | E1000_RCTL_BAM);

    g_e1000.rx_next = 0;
    g_e1000.tx_next = 0;
    g_e1000.present = 1;

    static const struct net_nic e1000_nic = {
        .name    = "e1000",
        .open    = e1000_open,
        .get_mac = e1000_get_mac,
        .send    = e1000_send,
        .poll    = e1000_poll,
    };
    net_nic_register(&e1000_nic);

    pr_info("e1000: %02u:%02u.%u MAC %02x:%02x:%02x:%02x:%02x:%02x bound\n",
            (unsigned)inf->bus, (unsigned)inf->dev, (unsigned)inf->func,
            g_e1000.mac[0], g_e1000.mac[1], g_e1000.mac[2],
            g_e1000.mac[3], g_e1000.mac[4], g_e1000.mac[5]);
    return 0;
}

static struct driver g_e1000_driver = {
    .name     = "e1000",
    .id_table = e1000_ids,
    .probe    = e1000_probe,
};

void e1000_driver_register(void)
{
    driver_register(&g_e1000_driver);
}
