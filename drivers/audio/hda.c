/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/drivers/audio/hda.c
 * Description: Intel HD-Audio (ICH6 HD-A 1.0) poll-mode playback driver API.
 * Added: 2026-10-22
 */
#include <drivers/hda.h>
#include <drivers/pci.h>
#include <drivers/drv_core.h>
#include <core/core.h>
#include <core/core_printk.h>
#include <mm/mm_pmm.h>
#include <iru_string.h>
#include <x86_devmap.h>
#include <x86_cpu.h>

/* Intel HD-Audio controller driver (the ICH6 register plane QEMU models
 * in hw/audio/intel-hda.c, which itself mirrors Linux hda_intel.c).
 *
 * Poll-mode only: CORB commands are pushed and their RIRB answers read
 * through register polling; the playback stream is a single-descriptor
 * Buffer Descriptor List whose completion is watched through SD.LPIB.
 * No interrupts, no DMA position buffer, no capture — this file is the
 * honest minimum that runs a real codec output end to end:
 *
 *   probe:  PCI BAR0 MMIO map -> controller power-up -> STATESTS codec
 *           link check -> CORB/RIRB rings -> GET_SUBSYSTEM_ID echo
 *   play:   codec converter format + stream tag -> SD (slot 4) BDL
 *           setup -> run -> LPIB poll -> stop
 *
 * Verified against QEMU `-device intel-hda -device hda-duplex`. Real HD-A
 * hardware notes, to close later: DMA buffers live in write-back direct
 * map RAM (a coherent/bounce strategy is pending, like the PCnet DMA)
 * and CORBSIZE is left at reset value on purpose — QEMU's ring wrap is a
 * fixed 256 entries regardless of the size register, and real ICH6
 * firmware presets share that geometry. */

#define HDA_REG_GCAP        0x00u   /* u16: version + stream inventory */
#define HDA_REG_VMAJ        0x03u   /*  u8 */
#define HDA_REG_GCTL        0x08u   /* u32: bit0 reset */
#define HDA_GCTL_RESET      (1u << 0)
#define HDA_REG_STATESTS    0x0eu   /* u16: per-codec link-presenter bits */

/* command/response rings (offsets per ICH6 HD-A 1.0) */
#define HDA_REG_CORBLBASE   0x40u   /* u32, 128-byte aligned */
#define HDA_REG_CORBUBASE   0x44u   /* u32 */
#define HDA_REG_CORBWP      0x48u   /* u16; writing it fires the dequeue */
#define HDA_REG_CORBRP      0x4au   /* u16; bit15 = reset pointer */
#define HDA_REG_CORBCTL     0x4cu   /* u8;  bit1 = DMA run */
#define HDA_REG_CORBSTS     0x4du   /* u8;  w1c */
#define HDA_CORBCTL_RUN     (1u << 1)
#define HDA_REG_RIRBLBASE   0x50u   /* u32, 128-byte aligned */
#define HDA_REG_RIRBUBASE   0x54u   /* u32 */
#define HDA_REG_RIRBWP      0x58u   /* u16; bit15 = reset pointer */
#define HDA_REG_RINTCNT     0x5au   /* u16; >=1 required or QEMU stalls the ring */
#define HDA_REG_RIRBCTL     0x5cu   /* u8;  bit0 IRQ, bit1 DMA */
#define HDA_RBCTL_DMA_EN    (1u << 1)
#define HDA_RBCTL_IRQ_EN    (1u << 0)
#define HDA_REG_RIRBSTS     0x5du   /* u8 w1c: bit0 response, bit2 overrun */
#define HDA_RBSTS_IRQ       (1u << 0)

/* stream slots: base 0x80 + n*0x20; slots 0-3 input, 4-7 output */
#define HDA_SD_BASE(n)      (0x80u + (u32)(n) * 0x20u)
#define HDA_SD_CTL          0x00u   /* u32: bit0 reset, bit1 run, tag [23:20] */
#define HDA_SD_STS          0x03u   /*  u8 w1c: 0x10 descerr 0x08 fifoerr 0x04 done */
#define HDA_SD_STS_FIFORDY  0x20u   /*  high after reset-clear */
#define HDA_SD_LPIB         0x04u   /* u32: bytes DMAed in the current pass */
#define HDA_SD_CBL          0x08u   /* u32: total bytes */
#define HDA_SD_LVI          0x0cu   /* u16: last valid descriptor index */
#define HDA_SD_FORMAT       0x12u   /* u16: AC_FMT word */
#define HDA_SD_BDLPL        0x18u   /* u32: BDL address low, 128-byte aligned */
#define HDA_SD_BDLPU        0x1cu   /* u32: BDL address high */

/* stream format word (AC_FMT, per Linux hda_codec.h): [3:0] channels-1,
 * [6:4] sample bits (1 = 16-bit), [14] 44.1k base, [15] non-PCM type. */
#define HDA_FMT_BITS_16     (1u << 4)
#define HDA_FMT_BASE_44K    (1u << 14)
#define HDA_FMT_TYPE_PCM    0u

/* BDL descriptor (16 bytes) */
struct hda_bdl_entry {
    u64 addr;
    u32 len;
    u32 flags;          /* bit0 = IOC (none here: poll mode) */
};

/* codec verbs (subset used at probe/link-time) */
#define HDA_VERB_GET_SUBSYSTEM  0xf20u  /* 12-bit: answers this codec's model id */

/* CORB command layout: [31:28] cad, [27:20] nid, [23:0] verb+payload.
 * 12-bit verbs carry payload at [7:0], 4-bit verbs at [11:0]. */
#define HDA_CORB(nid, data)  ((u32)(nid) << 20 | (u32)(data))
#define HDA_VERB12(verb, payload)   (((u32)(verb) << 8) | ((u32)(payload) & 0xffu))
#define HDA_VERB4(verb, payload)    (((u32)(verb) << 16) | ((u32)(payload) & 0xfffu))

#define HDA_NID_DAC_OUT    0x02u   /* hda-duplex audio-out converter  */
#define HDA_VERB_SET_FMT   0x2u    /* converter stream format   */
#define HDA_VERB_SET_TAG   0x706u  /* converter stream/channel id    */
#define HDA_QEMU_VENDOR    0x1af4u /* QEMU codec subsystem vendor */

#define HDA_STREAM_SLOT_OUT0  4u    /* first OUTPUT stream slot (SDO0) */
#define HDA_STREAM_TAG        1u    /* arbitrary nonzero stream id */

#define HDA_CORB_ENTRIES   256u
#define HDA_RIRB_ENTRIES   256u

#define HDA_POLL_SHORT     4000000u    /* link-level ops: sub-second even on TCG */
#define HDA_POLL_PLAY      24000000u   /* full playback pass, TCG-safe bound */

/* one 4 KiB frame holds CORB | RIRB | BDL (all 128-byte aligned) */
struct hda_dma_frame {
    u32 corb[HDA_CORB_ENTRIES];          /* 1024 bytes  */
    u32 rirb[HDA_RIRB_ENTRIES * 2];      /* pairs: response, extra */
    struct hda_bdl_entry bdl;
};

#define HDA_FRAME_BDL_OFF  3072u   /* 1024 (CORB) + 2048 (RIRB), 128-aligned */

static struct hda_state {
    int present;
    u8 *mm;                 /* controller BAR0 through the device window */
    u64 db_phys;            /* struct hda_dma_frame, physical */
    struct hda_dma_frame *db;
    u16 rirb_read;          /* RIRBWP value last consumed */
} g_hda;

/* --- register helpers --- */

static inline u32 hda_r32(u32 off)
{
    return *(volatile u32 *)(g_hda.mm + off);
}

static inline void hda_w32(u32 off, u32 v)
{
    *(volatile u32 *)(g_hda.mm + off) = v;
}

static inline u16 hda_r16(u32 off)
{
    return *(volatile u16 *)(g_hda.mm + off);
}

static inline void hda_w16(u32 off, u16 v)
{
    *(volatile u16 *)(g_hda.mm + off) = v;
}

static inline u8 hda_r8(u32 off)
{
    return *(volatile u8 *)(g_hda.mm + off);
}

static inline void hda_w8(u32 off, u8 v)
{
    *(volatile u8 *)(g_hda.mm + off) = v;
}

/* --- codec link --- */

/* Push one CORB command and wait for its solicited RIRB response.
 * Every consumed entry clears RIRBSTS.RESP so the controller keeps
 * dequeuing (QEMU parks corb_run while rirb_count == RINTCNT). */
static int hda_verb(u32 cmd, u32 *resp_out)
{
    u16 wp, rwp;
    u32 i, resp, ex;

    if (!g_hda.mm || !g_hda.db)
        return -1;
    /* The controller consumes at (rp+1) and stops at rp==wp, so the
     * command goes at (wp+1) and CORBWP is then set to that slot. */
    wp = (u16)((hda_r16(HDA_REG_CORBWP) & 0xffu) + 1u) & 0xffu;
    g_hda.db->corb[wp] = cmd;
    hda_w16(HDA_REG_CORBWP, wp);

    for (i = 0; i < HDA_POLL_SHORT; i++) {
        rwp = (u16)(hda_r16(HDA_REG_RIRBWP) & 0xffu);
        if (rwp == g_hda.rirb_read)
            continue;
        g_hda.rirb_read = rwp;
        resp = g_hda.db->rirb[(u32)rwp * 2u];
        ex = g_hda.db->rirb[(u32)rwp * 2u + 1u];
        /* w1c: retiring the response frees the controller for the
         * next command (and re-kicks the dequeue machinery). */
        hda_w8(HDA_REG_RIRBSTS, (u8)HDA_RBSTS_IRQ);
        if (resp_out)
            *resp_out = resp;
        if ((ex & 0xfu) != 0)
            return -1;      /* answered by a different codec address */
        return 0;
    }
    return -1;              /* the link went silent */
}

/* --- codec converter configuration --- */

static u16 hda_fmt_word(u32 rate, u32 channels, u32 bits)
{
    u16 word;

    if (bits != 16 || channels < 1u || channels > 2u)
        return 0;
    /* Only the two format bases are wired: 48 kHz exact (mult/div 0)
     * and 44.1 kHz base. Other rates need the mult/div ladder; the
     * honest answer is to refuse rather than emit silent garbage. */
    if (rate == 48000u)
        word = 0;
    else if (rate == 44100u)
        word = (u16)HDA_FMT_BASE_44K;
    else
        return 0;
    word |= (u16)(HDA_FMT_TYPE_PCM | HDA_FMT_BITS_16);
    word |= (u16)(channels - 1u);      /* AC_FMT channel nibble */
    return word;
}

/* --- playback --- */

int hda_play(u64 pcm_phys, u32 bytes, u32 rate, u32 channels, u32 bits)
{
    u32 sd = HDA_SD_BASE(HDA_STREAM_SLOT_OUT0);
    u32 ctl, lpib, prev, i;
    u16 fmt;

    if (!g_hda.present || !g_hda.db)
        return -1;
    if (bytes == 0 || (bytes & 3u))
        return -1;              /* whole frames required */
    fmt = hda_fmt_word(rate, channels, bits);
    if (!fmt)
        return -1;

    /* codec side: converter format, then stream tag (stream must match
     * the SD tag below or the codec never sees the DMA traffic). */
    if (hda_verb(HDA_CORB(HDA_NID_DAC_OUT,
                          HDA_VERB4(HDA_VERB_SET_FMT, fmt)), NULL))
        return -1;
    if (hda_verb(HDA_CORB(HDA_NID_DAC_OUT,
                          HDA_VERB12(HDA_VERB_SET_TAG,
                                     (HDA_STREAM_TAG << 4) | 0u)), NULL))
        return -1;

    /* controller side: reset dance, then the full stream programming.
     * parse_bdl runs exactly when run flips 0->1, so CBL/LVI/BDL are
     * settled before the last write. */
    ctl = hda_r32(sd + HDA_SD_CTL) & ~((1u << 20) | 3u);
    hda_w32(sd + HDA_SD_CTL, ctl | 1u);    /* reset */
    for (i = 0; i < HDA_POLL_SHORT; i++)
        if ((hda_r8(sd + HDA_SD_STS) & HDA_SD_STS_FIFORDY))
            break;
    hda_w32(sd + HDA_SD_CTL, ctl);          /* clear reset */

    hda_w16(sd + HDA_SD_FORMAT, fmt);
    hda_w32(sd + HDA_SD_CBL, bytes);
    hda_w16(sd + HDA_SD_LVI, 0);            /* one descriptor */
    hda_w32(sd + HDA_SD_BDLPL,
            (u32)(g_hda.db_phys + HDA_FRAME_BDL_OFF));
    hda_w32(sd + HDA_SD_BDLPU, (u32)(g_hda.db_phys >> 32));
    g_hda.db->bdl.addr = pcm_phys;
    g_hda.db->bdl.len = bytes;
    g_hda.db->bdl.flags = 0;

    cpu_mfence();
    hda_w32(sd + HDA_SD_CTL, ctl | (1u << 20) | 2u);  /* tag 1, run */

    prev = 0;
    for (i = 0; i < HDA_POLL_PLAY; i++) {
        lpib = hda_r32(sd + HDA_SD_LPIB);
        if (lpib == bytes)
            break;
        if (prev != 0 && lpib < prev)
            break;              /* wrapped past the end: bytes crossed */
        prev = lpib;
    }

    ctl = hda_r32(sd + HDA_SD_CTL) & ~((1u << 20) | 2u);
    hda_w32(sd + HDA_SD_CTL, ctl);          /* stop: codec notified false */

    if (i == HDA_POLL_PLAY)
        return -1;              /* DMA never completed */
    return (int)bytes;
}

/* --- PCI binding --- */

static const struct device_id hda_ids[] = {
    { 0x8086, 0x2668, 0 },      /* Intel ICH6 (QEMU -device intel-hda) */
    { 0x8086, 0x293e, 0 },      /* Intel ICH9 (QEMU -machine q35) */
    { 0, 0, 0 },
};

int hda_present(void)
{
    return g_hda.present;
}

static int hda_probe(struct device *dev)
{
    struct pci_dev_info *inf = dev->private;
    u32 bar, cmd, resp, i;
    u64 frame;
    u16 major;

    if (!inf)
        return -1;
    bar = inf->bars[0];
    if (bar & PCI_BAR_FLAG_IO)
        return -1;              /* this controller is MMIO-only */
    cmd = pci_config_read16(inf->bus, inf->dev, inf->func, PCI_REG_COMMAND);
    pci_config_write16(inf->bus, inf->dev, inf->func, PCI_REG_COMMAND,
                       (u16)(cmd | PCI_CMD_MEM_ENABLE | PCI_CMD_MASTER_ENABLE));

    g_hda.mm = devmap_map((u64)(bar & ~0xFu), 0x4000u);
    if (!g_hda.mm) {
        pr_warn("hda: cannot map BAR0\n");
        return -1;
    }

    major = hda_r8(HDA_REG_VMAJ);
    if (major != 1u) {
        printk("hda: unsupported spec version 0x%02x\n", major);
        return -1;
    }

    if (phys_alloc_block(0, &frame)) {
        pr_warn("hda: no DMA frame for link rings\n");
        return -1;
    }
    g_hda.db_phys = frame << PAGE_SHIFT;
    g_hda.db = (struct hda_dma_frame *)phys_to_virt(g_hda.db_phys);
    {
        u32 *w = (u32 *)g_hda.db;
        u32 n = (u32)(sizeof(*g_hda.db) / 4u);
        for (i = 0; i < n; i++)
            w[i] = 0;
    }

    /* Controller power-up: clearing reset triggers the device cold reset
     * (codecs re-present on the link), then release. */
    hda_w32(HDA_REG_GCTL, hda_r32(HDA_REG_GCTL) & ~HDA_GCTL_RESET);
    for (i = 0; i < 4096u; i++)
        cpu_mfence();           /* cold-reset settle: QEMU is synchronous,
                                 * real ICH6 datasheet wants ~500 µs */
    hda_w32(HDA_REG_GCTL, hda_r32(HDA_REG_GCTL) | HDA_GCTL_RESET);
    if (!(hda_r16(HDA_REG_STATESTS) & 1u)) {
        printk("hda: no codec presented on the link\n");
        return -1;
    }

    /* Rings. The wrap in the emulated queue is a fixed 256 entries, so
     * the rings are sized to the full 256-entry ICH6 maximum. */
    hda_w32(HDA_REG_CORBLBASE, (u32)g_hda.db_phys);
    hda_w32(HDA_REG_CORBUBASE, (u32)(g_hda.db_phys >> 32));
    hda_w16(HDA_REG_CORBRP, (u16)(1u << 15));
    hda_w16(HDA_REG_CORBRP, 0);
    hda_w16(HDA_REG_CORBWP, 0);
    hda_w32(HDA_REG_RIRBLBASE, (u32)(g_hda.db_phys + 1024u));
    hda_w32(HDA_REG_RIRBUBASE, (u32)(g_hda.db_phys >> 32));
    hda_w16(HDA_REG_RIRBWP, (u16)(1u << 15));
    hda_w16(HDA_REG_RIRBWP, 0);
    hda_w16(HDA_REG_RINTCNT, 0xff);         /* required before the first verb */
    hda_w8(HDA_REG_CORBCTL, (u8)HDA_CORBCTL_RUN);
    hda_w8(HDA_REG_RIRBCTL, (u8)(HDA_RBCTL_DMA_EN | HDA_RBCTL_IRQ_EN));

    /* Link echo: the emulated codec answers GET_SUBSYSTEM_ID with its
     * model id; the echo proves the full CORB->RIRB round trip works. */
    if (hda_verb(HDA_CORB(0u, HDA_VERB12(HDA_VERB_GET_SUBSYSTEM, 0u)), &resp) ||
        (resp >> 16) != HDA_QEMU_VENDOR) {
        printk("hda: codec link echo failed (resp 0x%08x)\n", resp);
        return -1;
    }

    g_hda.present = 1;
    printk("hda: HD-Audio 1.0 controller at %02x:%x.%x, codec link up "
           "(subsystem 0x%08x)\n",
           inf->bus, inf->dev, inf->func, resp);
    return 0;
}

static struct driver g_hda_driver = {
    .name     = "hda",
    .id_table = hda_ids,
    .probe    = hda_probe,
};

void hda_driver_register(void)
{
    driver_register(&g_hda_driver);
}
