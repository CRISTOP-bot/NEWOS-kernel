/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/drivers/block/ata.c
 * Description: Legacy-PIO ATA driver: primary master at ISA-legacy ports only, fully
 * Added: 2026-10-22
 */
#include <drivers/ata.h>
#include <x86_io.h>
#include <core/core_printk.h>
#include <iru_string.h>

/* Legacy-PIO ATA driver: primary master at ISA-legacy ports only, fully
 * polling (nIEN set, no IRQ14 handler ever fires), no DMA, no PCI setup.
 * LBA28 serves every request until the disk ends below 128 GiB; LBA48 is
 * used once a request crosses that limit, when the drive advertises it.
 * Transfers run at most 255 sectors per command; each sector is moved as
 * 256 plain `inw`/`outw` words (no string instructions, the kernel is
 * freestanding -mgeneral-regs-only). */

#define ATA_WORD_PER_SECTOR 256
#define ATA_SECTOR_SIZE     512

/* TCG and real hardware both finish commands in microseconds; the cap is
 * a hang-guard, not a functional timeout. io_wait stamps pace the loop
 * on real iron where status reads are ~100 ns apart. */
#define ATA_SPIN_CAP 4000000

/* --- register block, primary bus ------------------------------------------ */
#define ATA_DATA         0x1F0
#define ATA_ERROR        0x1F1
#define ATA_SECCNT       0x1F2
#define ATA_LBA_LOW      0x1F3
#define ATA_LBA_MID      0x1F4
#define ATA_LBA_HIGH     0x1F5
#define ATA_DEVICE       0x1F6
#define ATA_STATUS       0x1F7
#define ATA_CONTROL      0x3F6

/* copy of ATA_STATUS without side effects */
#define ATA_ALT_STATUS   ATA_CONTROL

#define ATA_ST_BSY       0x80
#define ATA_ST_DRQ       0x08
#define ATA_ST_DF        0x20
#define ATA_ST_ERR       0x01
#define ATA_ST_READY     0x40

#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_READ28   0x20
#define ATA_CMD_WRITE28  0x30
#define ATA_CMD_READ48   0x24
#define ATA_CMD_WRITE48  0x34

#define ATA_CTRL_nIEN    0x02

#define ATA_DEV_MASTER   0xE0

static block_dev_t g_ata_dev;
static u16 identify_words[ATA_WORD_PER_SECTOR];
static const char *g_ata_model = "unknown";

static int ata_wait_clear(u8 mask)
{
    for (int i = 0; i < ATA_SPIN_CAP; i++) {
        u8 st = inb(ATA_ALT_STATUS);
        if (!(st & mask))
            return 0;
        if (st & (ATA_ST_ERR | ATA_ST_DF))
            return -1;
        if ((i & 255) == 255)
            io_wait();
    }
    return -1;
}

static int ata_wait_drq(void)
{
    for (int i = 0; i < ATA_SPIN_CAP; i++) {
        u8 st = inb(ATA_ALT_STATUS);
        if (st & ATA_ST_DRQ)
            return 0;
        if (st & (ATA_ST_ERR | ATA_ST_DF))
            return -1;
        if ((i & 255) == 255)
            io_wait();
    }
    return -1;
}

static void ata_strerror(const char *what, u64 lba, u32 count)
{
    u8 st = inb(ATA_STATUS);
    u8 err = inb(ATA_ERROR);
    printk("ata: %s lba=%llu count=%u failed: st=%02x err=%02x\n",
           what, lba, count, st, err);
}

static int ata_pio_xfer(u64 lba, u32 count, const u16 *wsrc, u16 *rdst)
{
    int write = (wsrc != NULL);
    int use48 = (lba + count - 1) > 0x0FFFFFFFull;

    if (ata_wait_clear(ATA_ST_BSY) != 0) {
        ata_strerror(write ? "write" : "read", lba, count);
        return -1;
    }

    outb(ATA_DEVICE, ATA_DEV_MASTER);
    /* ~400ns settle so the device register lands before address setup. */
    io_wait();
    io_wait();
    io_wait();
    io_wait();

    if (ata_wait_clear(ATA_ST_BSY) != 0) {
        ata_strerror(write ? "write" : "read", lba, count);
        return -1;
    }

    if (use48) {
        /* LBA48 address setup order: count-high, LBA bytes 31:24, then
         * count-low and the remaining LBA bytes (count <= 255 here, so
         * the high half is always 0). */
        outb(ATA_SECCNT, 0);
        outb(ATA_LBA_LOW,  (u8)(lba >> 24));
        outb(ATA_LBA_MID,  0);
        outb(ATA_LBA_HIGH, 0);
        outb(ATA_SECCNT, (u8)(count & 0xFF));
        outb(ATA_LBA_LOW,  (u8)(lba));
        outb(ATA_LBA_MID,  (u8)(lba >> 8));
        outb(ATA_LBA_HIGH, (u8)(lba >> 16));
    } else {
        outb(ATA_SECCNT, (u8)(count & 0xFF));
        outb(ATA_LBA_LOW,  (u8)(lba));
        outb(ATA_LBA_MID,  (u8)(lba >> 8));
        outb(ATA_LBA_HIGH, (u8)(lba >> 16));
        outb(ATA_DEVICE, ATA_DEV_MASTER |
                         (u8)((lba >> 24) & 0x0F));
    }
    outb(ATA_STATUS, use48 ? (u8)(write ? ATA_CMD_WRITE48 : ATA_CMD_READ48)
                           : (u8)(write ? ATA_CMD_WRITE28 : ATA_CMD_READ28));

    for (u32 s = 0; s < count; s++) {
        if (ata_wait_clear(ATA_ST_BSY) != 0 || ata_wait_drq() != 0) {
            ata_strerror(write ? "write" : "read",
                         lba + s, count - s);
            return -1;
        }

        for (int i = 0; i < ATA_WORD_PER_SECTOR; i++) {
            if (write)
                outw(ATA_DATA, wsrc[s * ATA_WORD_PER_SECTOR + i]);
            else
                rdst[s * ATA_WORD_PER_SECTOR + i] = inw(ATA_DATA);
        }

        /* Drives de-assert DRQ between sectors; BSY may pulse. Wait for
         * the tail of the sector transfer before the next round. */
        if (s + 1 < count && ata_wait_clear(ATA_ST_BSY) != 0) {
            ata_strerror(write ? "write" : "read",
                         lba + s, count - s);
            return -1;
        }
    }

    /* The final status must be clean: BSY clear, no ERR/DF. */
    if (ata_wait_clear(ATA_ST_BSY) != 0 ||
        (inb(ATA_STATUS) & (ATA_ST_ERR | ATA_ST_DF)) != 0) {
        ata_strerror(write ? "write" : "read", lba, count);
        return -1;
    }
    return 0;
}

static int ata_read_sectors(block_dev_t *dev, u64 sector_start, u32 count,
                            u8 *buffer)
{
    (void)dev;
    u32 done = 0;
    while (done < count) {
        u32 chunk = count - done;
        if (chunk > 255)
            chunk = 255;
        if (ata_pio_xfer(sector_start + done, chunk, NULL,
                         (u16 *)(buffer + done * ATA_SECTOR_SIZE)) != 0)
            return -1;
        done += chunk;
    }
    return 0;
}

static int ata_write_sectors(block_dev_t *dev, u64 sector_start, u32 count,
                             const u8 *buffer)
{
    (void)dev;
    u32 done = 0;
    while (done < count) {
        u32 chunk = count - done;
        if (chunk > 255)
            chunk = 255;
        if (ata_pio_xfer(sector_start + done, chunk,
                         (const u16 *)(buffer + done * ATA_SECTOR_SIZE),
                         NULL) != 0)
            return -1;
        done += chunk;
    }
    return 0;
}

static void ata_identify_trim_model(void)
{
    /* Words 27..46 hold the model name, byte-swapped inside each word. */
    static char model[41];
    for (int i = 0; i < 20; i++) {
        u16 w = identify_words[27 + i];
        model[i * 2]     = (char)(w >> 8);
        model[i * 2 + 1] = (char)(w & 0xFF);
    }
    model[40] = '\0';
    int last = 40;
    while (last > 0 && model[last - 1] == ' ')
        model[--last] = '\0';
    g_ata_model = model;
}

int ata_init(void)
{
    outb(ATA_CONTROL, ATA_CTRL_nIEN);

    outb(ATA_DEVICE, ATA_DEV_MASTER);
    io_wait();
    if (ata_wait_clear(ATA_ST_BSY) != 0)
        return -1;

    u8 st = inb(ATA_STATUS);
    if (st == 0)
        return -1;                      /* nothing on the bus */

    u8 mid = inb(ATA_LBA_MID);
    u8 high = inb(ATA_LBA_HIGH);
    if ((mid == 0x14 && high == 0xEB) ||    /* ATAPI */
        (mid == 0x69 && high == 0x96) ||    /* SATAPI */
        (mid == 0x3C && high == 0xC3)) {    /* SATA port multiplier world */
        printk("ata: primary master is not a plain ATA disk\n");
        return -1;
    }

    outb(ATA_SECCNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_STATUS, ATA_CMD_IDENTIFY);

    st = inb(ATA_STATUS);
    if (st == 0)
        return -1;
    if ((st & (ATA_ST_ERR | ATA_ST_DF)) != 0 || ata_wait_drq() != 0) {
        printk("ata: IDENTIFY DEVICE failed (st=%02x)\n", st);
        return -1;
    }
    for (int i = 0; i < ATA_WORD_PER_SECTOR; i++)
        identify_words[i] = inw(ATA_DATA);
    ata_wait_clear(ATA_ST_BSY);

    if (!(identify_words[49] & 0x0100)) {
        printk("ata: primary master does not advertise LBA\n");
        return -1;
    }

    u64 sectors;
    if (identify_words[83] & 0x0400) {
        sectors = (u64)identify_words[100] |
                  ((u64)identify_words[101] << 16) |
                  ((u64)identify_words[102] << 32) |
                  ((u64)identify_words[103] << 48);
    } else {
        sectors = (u64)identify_words[60] |
                  ((u64)identify_words[61] << 16);
    }
    if (sectors == 0)
        return -1;

    ata_identify_trim_model();

    g_ata_dev.name = "disk0";
    g_ata_dev.sector_size = ATA_SECTOR_SIZE;
    g_ata_dev.sector_count = sectors;
    g_ata_dev.read_sectors = ata_read_sectors;
    g_ata_dev.write_sectors = ata_write_sectors;

    printk("ata: disk0 %llu sectors (%llu MiB), %s, model '%s'\n",
           sectors, sectors / 2048,
           (identify_words[83] & 0x0400) ? "LBA48" : "LBA28", g_ata_model);
    return 0;
}

block_dev_t *ata_get_dev(void)
{
    return g_ata_dev.name ? &g_ata_dev : NULL;
}
