/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/drivers/gpu/bga.c
 * Description: Bochs Graphics Adapter (BGA/VBE dispi) GPU driver.
 *             Sets a graphics mode through the DISPI registers (MMIO BAR
 *             at +0x500, or legacy 0x1ce/0x1cf I/O ports) and publishes
 *             the device's linear framebuffer as the kernel display.
 *             Verified against QEMU std VGA (1234:1111) and bochs-display
 *             using the documented register layout from QEMU's spec.
 * Added: 2026-11-23
 */
#include <drivers/bga.h>
#include <drivers/pci.h>
#include <drivers/drv_core.h>
#include <drivers/fb.h>
#include <core/core.h>
#include <core/core_printk.h>
#include <iru_string.h>
#include <x86_io.h>
#include <x86_devmap.h>

/* DISPI register indices (16-bit, addressed by index). */
#define BGA_ID                 0x00u
#define BGA_XRES               0x01u
#define BGA_YRES               0x02u
#define BGA_BPP                0x03u
#define BGA_ENABLE             0x04u
#define BGA_BANK               0x05u
#define BGA_VIRT_WIDTH         0x06u
#define BGA_VIRT_HEIGHT        0x07u
#define BGA_X_OFFSET           0x08u
#define BGA_Y_OFFSET           0x09u
#define BGA_FB_BASE_LOW        0x0au   /* v0xB0C3+ (video memory / 64K) */

#define BGA_ID_BASE            0xB0C0u
#define BGA_ENABLE_ENABLED     0x01u
#define BGA_ENABLE_LFB         0x40u   /* linear framebuffer (not 0x02!) */

/* Legacy I/O ports. */
#define BGA_IO_INDEX           0x1ceu
#define BGA_IO_DATA            0x1cfu

/* MMIO BAR: DISPI registers are at BAR2 + 0x500 + index*2 (16-bit). */
#define BGA_MMIO_DISPI_BASE    0x500u
#define BGA_MMIO_WINDOW        0x1000u

/* PCI IDs: Bochs/QEMU std VGA, and VirtualBox legacy adapter. */
static const struct device_id bga_ids[] = {
    { 0x1234, 0x1111, 0 },
    { 0x80ee, 0xbead, 0 },
    { 0, 0, 0 },
};

/* DISPI transport: either MMIO BAR or legacy I/O ports. */
enum bga_transport { BGA_TRANSPORT_NONE, BGA_TRANSPORT_MMIO, BGA_TRANSPORT_IO };
static enum bga_transport g_transport;
static volatile u8 *g_mmio;
static int     g_ready;
static u64     g_fb_phys, g_width, g_height, g_pitch;
static u32     g_bpp;

static u16 bga_read(u16 index)
{
    if (g_transport == BGA_TRANSPORT_MMIO)
        return *(volatile u16 *)(g_mmio + BGA_MMIO_DISPI_BASE + index * 2u);
    outw(BGA_IO_INDEX, index);
    return inw(BGA_IO_DATA);
}

static void bga_write(u16 index, u16 value)
{
    if (g_transport == BGA_TRANSPORT_MMIO) {
        *(volatile u16 *)(g_mmio + BGA_MMIO_DISPI_BASE + index * 2u) = value;
        return;
    }
    outw(BGA_IO_INDEX, index);
    outw(BGA_IO_DATA, value);
}

static int bga_is_device(u16 id)
{
    return (id & 0xFF00u) == 0xB000u;   /* family id, revision in low nibble */
}

int bga_present(void)
{
    return g_ready;
}

void bga_geometry(u64 *w, u64 *h, u64 *pitch, u32 *bpp)
{
    if (w) *w = g_width;
    if (h) *h = g_height;
    if (pitch) *pitch = g_pitch;
    if (bpp) *bpp = g_bpp;
}

u64 bga_framebuffer_phys(void)
{
    return g_fb_phys;
}

/* Set a mode and rebind the kernel framebuffer to the BGA scanout. */
int bga_set_mode(u64 width, u64 height, u32 bpp)
{
    u32 version;

    if (!g_ready)
        return -1;
    if (!width || width > 4096 || !height || height > 4096)
        return -1;
    if (bpp != 24 && bpp != 32)
        return -1;

    /* Ordering matters: disable first, program geometry, enable last.
     * Writing geometry while enabled is ignored/latched inconsistently. */
    bga_write(BGA_ENABLE, 0);
    bga_write(BGA_BPP, (u16)bpp);
    bga_write(BGA_XRES, (u16)width);
    bga_write(BGA_YRES, (u16)height);
    bga_write(BGA_BANK, 0);
    bga_write(BGA_VIRT_WIDTH, (u16)width);   /* stride in PIXELS */
    bga_write(BGA_VIRT_HEIGHT, (u16)height);
    bga_write(BGA_X_OFFSET, 0);
    bga_write(BGA_Y_OFFSET, 0);
    bga_write(BGA_ENABLE, BGA_ENABLE_ENABLED | BGA_ENABLE_LFB);

    g_width = bga_read(BGA_XRES);
    g_height = bga_read(BGA_YRES);
    g_bpp = bga_read(BGA_BPP);
    if (g_width != width || g_height != height || g_bpp != bpp) {
        pr_warn("bga: mode set failed (got %llux%llu bpp %u)\n",
                g_width, g_height, g_bpp);
        return -1;
    }
    g_pitch = g_width * (g_bpp / 8u);

    /* 32-bit BGA modes are XRGB (little-endian: blue at byte 0). */
    fb_init_early(g_fb_phys, g_width, g_height, g_pitch, g_bpp,
                  1, 16, 8, 8, 8, 0, 8);

    version = bga_read(BGA_ID) & 0x0Fu;
    pr_info("bga: GPU display %llux%llu bpp %u fb @ 0x%llx (dispi v%u, %s)\n",
            g_width, g_height, g_bpp, g_fb_phys, version,
            g_transport == BGA_TRANSPORT_MMIO ? "mmio" : "io");
    return 0;
}

static int bga_probe(struct device *dev)
{
    struct pci_dev_info *inf = dev->private;
    u32 bar, rbar, cmd;
    u16 id;

    if (!inf)
        return -1;

    /* BAR0 = linear framebuffer (VRAM). */
    bar = inf->bars[0];
    if (bar & PCI_BAR_FLAG_IO) {
        pr_warn("bga: BAR0 is I/O, cannot map linear framebuffer\n");
        return -1;
    }
    rbar = inf->bars[2];

    cmd = pci_config_read16(inf->bus, inf->dev, inf->func, PCI_REG_COMMAND);
    pci_config_write16(inf->bus, inf->dev, inf->func, PCI_REG_COMMAND,
                       cmd | PCI_CMD_MEM_ENABLE | PCI_CMD_MASTER_ENABLE);

    /* Prefer MMIO BAR2 (DISPI registers at +0x500), fall back to legacy
     * I/O ports. Choose the transport by which one actually reads a
     * valid Bochs ID back — never guess from BAR presence alone. */
    g_transport = BGA_TRANSPORT_NONE;
    if (rbar && !(rbar & PCI_BAR_FLAG_IO)) {
        g_mmio = (volatile u8 *)devmap_map(rbar & ~0xFu, BGA_MMIO_WINDOW);
        if (g_mmio) {
            g_transport = BGA_TRANSPORT_MMIO;
            id = bga_read(BGA_ID);
            if (!bga_is_device(id))
                g_transport = BGA_TRANSPORT_NONE;
        }
    }
    if (g_transport == BGA_TRANSPORT_NONE) {
        g_transport = BGA_TRANSPORT_IO;
        id = bga_read(BGA_ID);
        if (!bga_is_device(id))
            g_transport = BGA_TRANSPORT_NONE;
    }
    if (g_transport == BGA_TRANSPORT_NONE) {
        pr_warn("bga: no DISPI registers over MMIO or I/O ports\n");
        return -1;
    }

    g_fb_phys = bar & ~0xFu;
    g_fb_phys &= 0xFFFFFFFFull;   /* BAR0 is 32-bit for BGA */
    g_width = g_height = g_pitch = 0;
    g_bpp = 0;
    g_ready = 1;

    /* Take over the display with a 1024x768x32 mode. If the device
     * refuses, the bootloader's framebuffer stays in charge. */
    if (bga_set_mode(1024, 768, 32) != 0)
        pr_warn("bga: probe mode set failed; keeping bootloader fb\n");
    return 0;
}

static struct driver g_bga_driver = {
    .name = "bga",
    .id_table = bga_ids,
    .probe = bga_probe,
};

void bga_driver_register(void)
{
    driver_register(&g_bga_driver);
}
