/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/drivers/graphics/fb.c
 * Description: Linear framebuffer over a Limine graphics mode.
 * Added: 2026-10-17
 */
#include <drivers/graphics.h>
#include <drivers/tty_console.h>
#include <core/core.h>
#include <core/core_console.h>
#include <core/core_printk.h>
#include <x86_cpu.h>
#include <x86_mmu.h>
#include <iru_string.h>
#include "fb_priv.h"

/* Linear framebuffer over a Limine graphics mode.
 *
 * Mapping: a private 1 GiB kernel-half window at PML4[507] (just below
 * the direct map) is backed by one static PDPT + one static PD using
 * 2 MiB pages with PCD|PWT (uncacheable). The panel is only ever touched
 * through this window, never through the write-back direct-map alias.
 * Framebuffers larger than the window, unaligned bases, or non-RGB
 * modes are refused and the legacy VGA text path stays in charge. */

#define FB_PML4_INDEX   507u
#define FB_VIRT_BASE    (0xFFFFFD8000000000ull)
#define FB_MAX_2M_PAGES 512u
#define FB_2M           (2u * 1024u * 1024u)

static u64 fb_pdpt[512] __attribute__((aligned(4096)));
static u64 fb_pd[512] __attribute__((aligned(4096)));

int g_present;
static u64 g_phys;
u64 g_w, g_h, g_pitch;
u32 g_bpp;
static u8 g_rs, g_rn, g_gs, g_gn, g_bs, g_bn;
u8 *g_fb;

u32 fb_pack(u8 r, u8 g, u8 b)
{
    u32 v = 0;
    if (g_rn)
        v |= (u32)(((u32)r >> (8 - g_rn)) << g_rs);
    if (g_gn)
        v |= (u32)(((u32)g >> (8 - g_gn)) << g_gs);
    if (g_bn)
        v |= (u32)(((u32)b >> (8 - g_bn)) << g_bs);
    return v;
}

static void fb_write_px(u64 x, u64 y, u32 px)
{
    u8 *p = g_fb + y * g_pitch + x * (g_bpp / 8u);
    if (g_bpp == 32) {
        p[0] = (u8)(px & 0xFFu);
        p[1] = (u8)((px >> 8) & 0xFFu);
        p[2] = (u8)((px >> 16) & 0xFFu);
        p[3] = (u8)((px >> 24) & 0xFFu);
    } else {    /* 24 */
        p[0] = (u8)(px & 0xFFu);
        p[1] = (u8)((px >> 8) & 0xFFu);
        p[2] = (u8)((px >> 16) & 0xFFu);
    }
}

void fb_putpixel(u64 x, u64 y, u32 xrgb)
{
    u8 r, g, b;
    if (!g_present || x >= g_w || y >= g_h)
        return;
    r = (u8)((xrgb >> 16) & 0xFFu);
    g = (u8)((xrgb >> 8) & 0xFFu);
    b = (u8)(xrgb & 0xFFu);
    fb_write_px(x, y, fb_pack(r, g, b));
}

void fb_fill_rect(u64 x, u64 y, u64 w, u64 h, u32 xrgb)
{
    u64 i, j;
    u8 r, g, b;
    u32 px;
    if (!g_present || !w || !h || x >= g_w || y >= g_h)
        return;
    if (x + w > g_w)
        w = g_w - x;
    if (y + h > g_h)
        h = g_h - y;
    r = (u8)((xrgb >> 16) & 0xFFu);
    g = (u8)((xrgb >> 8) & 0xFFu);
    b = (u8)(xrgb & 0xFFu);
    px = fb_pack(r, g, b);
    for (i = 0; i < h; i++)
        for (j = 0; j < w; j++)
            fb_write_px(x + j, y + i, px);
}

void fb_clear(u32 xrgb)
{
    if (!g_present)
        return;
    fb_fill_rect(0, 0, g_w, g_h, xrgb);
}

/* Exact in-memory XRGB8888 layout: red bits 16..23, green 8..15, blue 0..7,
 * no shifting needed. When this holds, a blit is a plain per-row u32 copy
 * instead of per-pixel unpack/pack/store; the desktop and glxgears submit
 * damage rectangles every frame, and this loop is their hot path. */
static int fb_argb_identity(void)
{
    return g_bpp == 32 && g_rs == 16 && g_rn == 8 &&
           g_gs == 8 && g_gn == 8 && g_bs == 0 && g_bn == 8;
}

long fb_blit_rows(u64 x, u64 y, u64 w, u64 h, const u32 *rows)
{
    u64 i, j;
    if (!g_present || !rows || !w || !h || x >= g_w || y >= g_h)
        return -1;
    if (x + w > g_w || y + h > g_h)
        return -1;
    if (w > 2048u || h > 2048u)
        return -1;
    if (fb_argb_identity()) {
        for (j = 0; j < h; j++) {
            u32 *dst = (u32 *)(g_fb + (y + j) * g_pitch + x * 4u);
            const u32 *src = rows + (size_t)j * (size_t)w;
            for (i = 0; i < w; i++)
                dst[i] = src[i];
        }
        return (long)(w * h);
    }
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            u32 v = rows[j * w + i];
            u8 r = (u8)((v >> 16) & 0xFFu);
            u8 g = (u8)((v >> 8) & 0xFFu);
            u8 b = (u8)(v & 0xFFu);
            fb_write_px(x + i, y + j, fb_pack(r, g, b));
        }
    }
    return (long)(w * h);
}

int fb_present(void)
{
    return g_present;
}

void fb_geometry(u64 *w, u64 *h, u64 *pitch, u32 *bpp)
{
    if (w)
        *w = g_w;
    if (h)
        *h = g_h;
    if (pitch)
        *pitch = g_pitch;
    if (bpp)
        *bpp = g_bpp;
}

void fb_init_early(u64 phys, u64 width, u64 height, u64 pitch, u32 bpp,
                   u8 model, u8 rs, u8 rn, u8 gs, u8 gn, u8 bs, u8 bn)
{
    u64 bytes, pages, i;
    u64 base;

    if (!phys || !width || !height || !pitch)
        return;
    if (model != 1 || (bpp != 32 && bpp != 24))
        return;
    if (phys & (FB_2M - 1u))
        return;   /* 2 MiB pages need an aligned base; else stay on text */

    bytes = pitch * height;
    pages = (bytes + FB_2M - 1u) / FB_2M;
    if (!pages || pages > FB_MAX_2M_PAGES)
        return;

    for (i = 0; i < 512; i++) {
        fb_pdpt[i] = 0;
        fb_pd[i] = 0;
    }
    for (i = 0; i < pages; i++) {
        fb_pd[i] = (phys + i * FB_2M) | X64_PAGE_PRESENT | X64_PAGE_WRITE |
                   X64_PAGE_PCD | X64_PAGE_PWT | X64_PAGE_HUGE;
    }
    fb_pdpt[0] = virt_to_phys((uintptr_t)fb_pd) | X64_PAGE_PRESENT |
                 X64_PAGE_WRITE;
    /* Reach the PML4 through the direct map, not through its link-time
     * address: the early tables sit at a physical-low address, so a
     * high-half RIP-relative displacement cannot reach them (truncated
     * R_X86_64_PC32 under -mcmodel=kernel). Same idiom as x86_paging.c. */
    u64 *pml4 = (u64 *)phys_to_virt(virt_to_phys((uintptr_t)x64_pml4));
    pml4[FB_PML4_INDEX] = virt_to_phys((uintptr_t)fb_pdpt) |
                          X64_PAGE_PRESENT | X64_PAGE_WRITE;
    /* The identity window is gone by now: reach the framebuffer through the
     * 4 GiB direct map, not its raw physical address. */
    base = FB_VIRT_BASE;
    for (i = 0; i < pages; i++)
        invlpg(base + i * FB_2M);
    cpu_mfence();

    g_phys = phys;
    g_w = width;
    g_h = height;
    g_pitch = pitch;
    g_bpp = bpp;
    g_rs = rs;
    g_rn = rn;
    g_gs = gs;
    g_gn = gn;
    g_bs = bs;
    g_bn = bn;
    g_fb = (u8 *)(uintptr_t)base;
    g_present = 1;

    fbcon_cols = width / 8u;
    fbcon_rows = height / 8u;
    if (!fbcon_cols)
        fbcon_cols = 1;
    if (!fbcon_rows)
        fbcon_rows = 1;
    fbcon_cc = fbcon_cr = 0;
    fbcon_esc = fbcon_csi = fbcon_priv = fbcon_charset = 0;
    fbcon_fgi = 7;
    fbcon_bgi = 0;
    fbcon_bold = 0;
    fbcon_rev = 0;
    fbcon_visible = 1;
    fbcon_cur_drawn = 0;
    fbcon_save_cc = fbcon_save_cr = 0;
    fb_grid_ok = (fbcon_cols * fbcon_rows <= FB_GRID_MAX) ? 1 : 0;
    if (fb_grid_ok) {
        u16 attr = fbcon_attr_pack(7, 0);

        memset(fb_grid_buf, ' ', (size_t)fbcon_cols * fbcon_rows);
        for (u64 i = 0; i < fbcon_cols * fbcon_rows; i++)
            fb_grid_attr[i] = attr;
    }
    fbm_x = fbm_y = -1;

    fb_clear(FBCON_BG);
    console_register(&g_fbcon);
    pr_info("fb: %llux%llu pitch %llu bpp %u, UC window %p\n",
            width, height, pitch, (unsigned)bpp, (void *)base);
}