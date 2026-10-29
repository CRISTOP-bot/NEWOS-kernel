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

static int g_present;
static u64 g_phys, g_w, g_h, g_pitch;
static u32 g_bpp;
static u8 g_rs, g_rn, g_gs, g_gn, g_bs, g_bn;
static u8 *g_fb;

static u32 fb_pack(u8 r, u8 g, u8 b)
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

/* --- fbcon: 8x8 text console on the panel ------------------------------- */

#include "font8x8_basic.h"

#define FBCON_BG 0x000000u

static u64 fbcon_cols, fbcon_rows;
static u64 fbcon_cc, fbcon_cr;
static int fbcon_esc, fbcon_csi, fbcon_priv, fbcon_charset;
static u64 fbcon_p[8];
static int fbcon_np;
static int fbcon_fgi = 7, fbcon_bgi;    /* SGR indices into the 256 palette */
static int fbcon_bold, fbcon_rev;
static int fbcon_visible = 1, fbcon_cur_drawn;
static u64 fbcon_cur_cc, fbcon_cur_cr;
static u64 fbcon_save_cc, fbcon_save_cr;

/* Text grid: one char plus its palette attribute per cell. The mouse arrow
 * restores what it covers and the cursor is painted as reverse video, so
 * both need the colors of the cell underneath, not the current SGR state. */
#define FB_GRID_MAX (160u * 128u)
static char fb_grid_buf[FB_GRID_MAX];
static u16 fb_grid_attr[FB_GRID_MAX];
static int fb_grid_ok;

/* 16 ANSI colors, VGA-flavoured to match the text console. */
static const u32 fbcon_vga[16] = {
    0x000000u, 0xAA0000u, 0x00AA00u, 0xAA5500u,
    0x0000AAu, 0xAA00AAu, 0x00AAAAu, 0xAAAAAAu,
    0x555555u, 0xFF5555u, 0x55FF55u, 0xFFFF55u,
    0x5555FFu, 0xFF55FFu, 0x55FFFFu, 0xFFFFFFu,
};

static u32 fbcon_pal256(int idx)
{
    if (idx < 0)
        idx = 0;
    idx &= 0xFF;
    if (idx < 16)
        return fbcon_vga[idx];
    if (idx < 232) {
        static const u8 lv[6] = { 0, 95, 135, 175, 215, 255 };
        int n = idx - 16;
        u32 r = lv[n / 36];
        u32 g = lv[(n / 6) % 6];
        u32 b = lv[n % 6];
        return (r << 16) | (g << 8) | b;
    }
    {
        u32 v = (u32)(8 + (idx - 232) * 10);
        return (v << 16) | (v << 8) | v;
    }
}

/* Attribute layout: foreground index in bits 15..8, background in 7..0.
 * Bold and reverse are folded in when the cell is painted, so restoring a
 * cell does not depend on the SGR state at restore time. */
static u16 fbcon_attr_pack(int fgi, int bgi)
{
    return (u16)((((u32)fgi & 0xFFu) << 8) | ((u32)bgi & 0xFFu));
}

static void fbcon_attr_get(int *fgi, int *bgi)
{
    int f = fbcon_fgi, b = fbcon_bgi;

    if (fbcon_bold && f < 8)
        f += 8;
    if (fbcon_rev) {
        int t = f;
        f = b;
        b = t;
    }
    *fgi = f;
    *bgi = b;
}

/* Raw cell paint: does not touch the text grid, so the cursor overlay can
 * draw inverted colors without corrupting the character underneath. */
static void fbcon_cell_px(u64 cc, u64 cr, char ch, u32 fg, u32 bg)
{
    u8 *row;
    u64 r;
    u8 glyph;

    if (cc >= fbcon_cols || cr >= fbcon_rows)
        return;
    glyph = (ch >= 0x20 && ch < 0x7F) ? (u8)ch : (u8)'?';
    row = g_fb + cr * 8u * g_pitch + cc * 8u * (g_bpp / 8u);
    for (r = 0; r < 8; r++) {
        u8 bits = (u8)font8x8_basic[(size_t)glyph][r];
        u64 c;
        for (c = 0; c < 8; c++) {
            u32 px = (bits & (u8)(0x01u << c)) ? fg : bg;
            u8 rr = (u8)((px >> 16) & 0xFFu);
            u8 gg = (u8)((px >> 8) & 0xFFu);
            u8 bb = (u8)(px & 0xFFu);
            u8 *p = row + r * g_pitch + c * (g_bpp / 8u);
            u32 out = fb_pack(rr, gg, bb);
            if (g_bpp == 32) {
                p[0] = (u8)(out & 0xFFu);
                p[1] = (u8)((out >> 8) & 0xFFu);
                p[2] = (u8)((out >> 16) & 0xFFu);
                p[3] = 0;
            } else {
                p[0] = (u8)(out & 0xFFu);
                p[1] = (u8)((out >> 8) & 0xFFu);
                p[2] = (u8)((out >> 16) & 0xFFu);
            }
        }
    }
}

static void fbcon_cell(u64 cc, u64 cr, char ch, int fgi, int bgi)
{
    if (cc >= fbcon_cols || cr >= fbcon_rows)
        return;
    if (fb_grid_ok) {
        size_t i = (size_t)(cr * fbcon_cols + cc);
        fb_grid_buf[i] = ch;
        fb_grid_attr[i] = fbcon_attr_pack(fgi, bgi);
    }
    fbcon_cell_px(cc, cr, ch, fbcon_pal256(fgi), fbcon_pal256(bgi));
}

static void fbcon_cell_cur(u64 cc, u64 cr, char ch)
{
    int fgi, bgi;

    fbcon_attr_get(&fgi, &bgi);
    fbcon_cell(cc, cr, ch, fgi, bgi);
}

/* Repaint one grid cell with the colors it was written with. */
static void fbcon_restore_cell(u64 cc, u64 cr)
{
    size_t i;
    u16 a;

    if (cc >= fbcon_cols || cr >= fbcon_rows || !fb_grid_ok)
        return;
    i = (size_t)(cr * fbcon_cols + cc);
    a = fb_grid_attr[i];
    fbcon_cell_px(cc, cr, fb_grid_buf[i], fbcon_pal256((int)(a >> 8)),
                  fbcon_pal256((int)(a & 0xFFu)));
}

static void fbcon_redraw_row(u64 cr)
{
    u64 cc;

    if (!fb_grid_ok || cr >= fbcon_rows)
        return;
    for (cc = 0; cc < fbcon_cols; cc++)
        fbcon_restore_cell(cc, cr);
}

static void fbcon_cursor_erase(void)
{
    if (!fbcon_cur_drawn)
        return;
    fbcon_cur_drawn = 0;
    fbcon_restore_cell(fbcon_cur_cc, fbcon_cur_cr);
}

static void fbcon_cursor_draw(void)
{
    size_t i;
    u16 a;
    int f, b;

    if (!fbcon_visible || !fb_grid_ok || fbcon_cur_drawn)
        return;
    if (fbcon_cc >= fbcon_cols || fbcon_cr >= fbcon_rows)
        return;
    i = (size_t)(fbcon_cr * fbcon_cols + fbcon_cc);
    a = fb_grid_attr[i];
    f = (int)(a >> 8);
    b = (int)(a & 0xFFu);
    /* Swap ink and paper: a space becomes a solid block and a glyph turns
     * into classic reverse-video. */
    fbcon_cell_px(fbcon_cc, fbcon_cr, fb_grid_buf[i],
                  fbcon_pal256(b), fbcon_pal256(f));
    fbcon_cur_cc = fbcon_cc;
    fbcon_cur_cr = fbcon_cr;
    fbcon_cur_drawn = 1;
}

static void fbcon_scroll(void)
{
    u64 rowbytes = g_pitch * 8u;
    u64 body = (fbcon_rows - 1u) * rowbytes;
    u8 *top = g_fb;
    int fgi, bgi;
    u16 attr;
    size_t row;

    if (fbcon_rows < 2)
        return;
    memmove(top, top + rowbytes, (size_t)body);
    fbcon_attr_get(&fgi, &bgi);
    fb_fill_rect(0, (fbcon_rows - 1u) * 8u, g_w, 8u,
                 fbcon_pal256(bgi));
    if (fb_grid_ok) {
        row = (size_t)fbcon_cols;
        memmove(fb_grid_buf, fb_grid_buf + row,
                (size_t)(fbcon_rows - 1) * row);
        memmove(fb_grid_attr, fb_grid_attr + row,
                (size_t)(fbcon_rows - 1) * row * sizeof(u16));
        memset(fb_grid_buf + (fbcon_rows - 1) * row, ' ', row);
        attr = fbcon_attr_pack(fgi, bgi);
        for (u64 i = 0; i < fbcon_cols; i++)
            fb_grid_attr[(fbcon_rows - 1) * row + i] = attr;
    }
}

static void fbcon_hide_cursor(void)
{
    fbcon_cursor_erase();
}

static void fbcon_newline(void)
{
    fbcon_hide_cursor();
    fbcon_cc = 0;
    if (++fbcon_cr >= fbcon_rows) {
        fbcon_cr = fbcon_rows - 1;
        fbcon_scroll();
    }
}

static void fbcon_show_cursor(void)
{
    fbcon_cursor_draw();
}

static void fbcon_erase_line(int mode)
{
    int fgi, bgi;
    u64 cc;

    fbcon_hide_cursor();
    fbcon_attr_get(&fgi, &bgi);
    if (mode == 2) {
        for (cc = 0; cc < fbcon_cols; cc++)
            fbcon_cell(cc, fbcon_cr, ' ', fgi, bgi);
    } else if (mode == 1) {
        for (cc = 0; cc <= fbcon_cc && cc < fbcon_cols; cc++)
            fbcon_cell(cc, fbcon_cr, ' ', fgi, bgi);
    } else {
        for (cc = fbcon_cc; cc < fbcon_cols; cc++)
            fbcon_cell(cc, fbcon_cr, ' ', fgi, bgi);
    }
}

static void fbcon_erase_display(int mode)
{
    int fgi, bgi;
    u16 attr;
    u64 r, c;

    fbcon_hide_cursor();
    fbcon_attr_get(&fgi, &bgi);
    if (mode == 2) {
        fb_clear(fbcon_pal256(bgi));
        if (fb_grid_ok) {
            attr = fbcon_attr_pack(fgi, bgi);
            memset(fb_grid_buf, ' ',
                   (size_t)fbcon_cols * fbcon_rows);
            for (r = 0; r < fbcon_cols * fbcon_rows; r++)
                fb_grid_attr[r] = attr;
        }
        fbcon_cc = fbcon_cr = 0;
        return;
    }
    if (mode == 1) {
        for (r = 0; r < fbcon_cr; r++)
            for (c = 0; c < fbcon_cols; c++)
                fbcon_cell(c, r, ' ', fgi, bgi);
        for (c = 0; c <= fbcon_cc && c < fbcon_cols; c++)
            fbcon_cell(c, fbcon_cr, ' ', fgi, bgi);
    } else {
        for (c = fbcon_cc; c < fbcon_cols; c++)
            fbcon_cell(c, fbcon_cr, ' ', fgi, bgi);
        for (r = fbcon_cr + 1; r < fbcon_rows; r++)
            for (c = 0; c < fbcon_cols; c++)
                fbcon_cell(c, r, ' ', fgi, bgi);
    }
}

static void fbcon_move_up(u64 n)
{
    fbcon_hide_cursor();
    fbcon_cr = (fbcon_cr > n) ? fbcon_cr - n : 0;
}

static void fbcon_move_down(u64 n)
{
    fbcon_hide_cursor();
    fbcon_cr += n;
    if (fbcon_cr >= fbcon_rows)
        fbcon_cr = fbcon_rows - 1;
}

static void fbcon_move_left(u64 n)
{
    fbcon_hide_cursor();
    fbcon_cc = (fbcon_cc > n) ? fbcon_cc - n : 0;
}

static void fbcon_move_right(u64 n)
{
    fbcon_hide_cursor();
    fbcon_cc += n;
    if (fbcon_cc >= fbcon_cols)
        fbcon_cc = fbcon_cols - 1;
}

static void fbcon_insert_lines(u64 n)
{
    int fgi, bgi;
    u16 attr;
    size_t row, avail;
    u64 r;

    fbcon_hide_cursor();
    if (!fb_grid_ok || !n)
        return;
    avail = (size_t)(fbcon_rows - fbcon_cr);
    if (n > avail)
        n = avail;
    row = (size_t)fbcon_cols;
    memmove(fb_grid_buf + (fbcon_cr + n) * row,
            fb_grid_buf + fbcon_cr * row, (avail - n) * row);
    memmove(fb_grid_attr + (fbcon_cr + n) * row,
            fb_grid_attr + fbcon_cr * row, (avail - n) * row * sizeof(u16));
    fbcon_attr_get(&fgi, &bgi);
    attr = fbcon_attr_pack(fgi, bgi);
    for (r = fbcon_cr; r < fbcon_cr + n; r++) {
        memset(fb_grid_buf + r * row, ' ', row);
        for (u64 c = 0; c < fbcon_cols; c++)
            fb_grid_attr[r * row + c] = attr;
    }
    for (r = fbcon_cr; r < fbcon_rows; r++)
        fbcon_redraw_row(r);
}

static void fbcon_delete_lines(u64 n)
{
    int fgi, bgi;
    u16 attr;
    size_t row, avail;
    u64 r;

    fbcon_hide_cursor();
    if (!fb_grid_ok || !n)
        return;
    avail = (size_t)(fbcon_rows - fbcon_cr);
    if (n > avail)
        n = avail;
    row = (size_t)fbcon_cols;
    memmove(fb_grid_buf + fbcon_cr * row,
            fb_grid_buf + (fbcon_cr + n) * row, (avail - n) * row);
    memmove(fb_grid_attr + fbcon_cr * row,
            fb_grid_attr + (fbcon_cr + n) * row,
            (avail - n) * row * sizeof(u16));
    fbcon_attr_get(&fgi, &bgi);
    attr = fbcon_attr_pack(fgi, bgi);
    for (r = fbcon_rows - n; r < fbcon_rows; r++) {
        memset(fb_grid_buf + r * row, ' ', row);
        for (u64 c = 0; c < fbcon_cols; c++)
            fb_grid_attr[r * row + c] = attr;
    }
    for (r = fbcon_cr; r < fbcon_rows; r++)
        fbcon_redraw_row(r);
}

static void fbcon_insert_chars(u64 n)
{
    int fgi, bgi;
    u16 attr;
    size_t row;
    u64 c;

    fbcon_hide_cursor();
    if (!fb_grid_ok || !n)
        return;
    if (n > fbcon_cols - fbcon_cc)
        n = fbcon_cols - fbcon_cc;
    row = (size_t)(fbcon_cr * fbcon_cols);
    memmove(fb_grid_buf + row + fbcon_cc + n,
            fb_grid_buf + row + fbcon_cc,
            (size_t)(fbcon_cols - fbcon_cc - n));
    memmove(fb_grid_attr + row + fbcon_cc + n,
            fb_grid_attr + row + fbcon_cc,
            (size_t)(fbcon_cols - fbcon_cc - n) * sizeof(u16));
    fbcon_attr_get(&fgi, &bgi);
    attr = fbcon_attr_pack(fgi, bgi);
    for (c = fbcon_cc; c < fbcon_cc + n; c++) {
        fb_grid_buf[row + c] = ' ';
        fb_grid_attr[row + c] = attr;
    }
    fbcon_redraw_row(fbcon_cr);
}

static void fbcon_delete_chars(u64 n)
{
    int fgi, bgi;
    u16 attr;
    size_t row;
    u64 c;

    fbcon_hide_cursor();
    if (!fb_grid_ok || !n)
        return;
    if (n > fbcon_cols - fbcon_cc)
        n = fbcon_cols - fbcon_cc;
    row = (size_t)(fbcon_cr * fbcon_cols);
    memmove(fb_grid_buf + row + fbcon_cc,
            fb_grid_buf + row + fbcon_cc + n,
            (size_t)(fbcon_cols - fbcon_cc - n));
    memmove(fb_grid_attr + row + fbcon_cc,
            fb_grid_attr + row + fbcon_cc + n,
            (size_t)(fbcon_cols - fbcon_cc - n) * sizeof(u16));
    fbcon_attr_get(&fgi, &bgi);
    attr = fbcon_attr_pack(fgi, bgi);
    for (c = fbcon_cols - n; c < fbcon_cols; c++) {
        fb_grid_buf[row + c] = ' ';
        fb_grid_attr[row + c] = attr;
    }
    fbcon_redraw_row(fbcon_cr);
}

static void fbcon_erase_chars(u64 n)
{
    int fgi, bgi;
    u64 c;

    fbcon_hide_cursor();
    fbcon_attr_get(&fgi, &bgi);
    for (c = fbcon_cc; c < fbcon_cc + n && c < fbcon_cols; c++)
        fbcon_cell(c, fbcon_cr, ' ', fgi, bgi);
}

static void fbcon_sgr(int n)
{
    int k;

    for (k = 0; k < n; k++) {
        int v = (int)fbcon_p[k];

        if (v == 0) {
            fbcon_fgi = 7;
            fbcon_bgi = 0;
            fbcon_bold = 0;
            fbcon_rev = 0;
        } else if (v == 1) {
            fbcon_bold = 1;
        } else if (v == 22) {
            fbcon_bold = 0;
        } else if (v == 7) {
            fbcon_rev = 1;
        } else if (v == 27) {
            fbcon_rev = 0;
        } else if (v >= 30 && v <= 37) {
            fbcon_fgi = v - 30;
        } else if (v == 39) {
            fbcon_fgi = 7;
        } else if (v >= 90 && v <= 97) {
            fbcon_fgi = v - 90 + 8;
        } else if (v >= 40 && v <= 47) {
            fbcon_bgi = v - 40;
        } else if (v == 49) {
            fbcon_bgi = 0;
        } else if (v >= 100 && v <= 107) {
            fbcon_bgi = v - 100 + 8;
        } else if ((v == 38 || v == 48) && k + 2 < n &&
                   fbcon_p[k + 1] == 5) {
            if (v == 38)
                fbcon_fgi = (int)(fbcon_p[k + 2] & 0xFFu);
            else
                fbcon_bgi = (int)(fbcon_p[k + 2] & 0xFFu);
            k += 2;
        } else if ((v == 38 || v == 48) && k + 4 < n &&
                   fbcon_p[k + 1] == 2) {
            /* 24-bit color: skip r;g;b instead of misreading them as SGR
             * codes (a literal 0 would reset the whole attribute). */
            k += 4;
        }
    }
}

static void fbcon_putchar(char c)
{
    if (!g_present)
        return;

    if (fbcon_esc) {
        if (fbcon_charset) {
            fbcon_charset = 0;      /* second byte of a designation */
            fbcon_esc = 0;
            return;
        }
        if (!fbcon_csi) {
            if (c == '[') {
                fbcon_csi = 1;
                fbcon_priv = 0;
                fbcon_np = 0;
                for (int i = 0; i < 8; i++)
                    fbcon_p[i] = 0;
            } else if (c == '(' || c == ')' || c == '#' || c == '%' ||
                       c == '*' || c == '+' || c == '-' || c == '.') {
                fbcon_charset = 1;  /* ESC ( B / ESC ) 0 and friends */
            } else if (c == '7') {
                fbcon_save_cc = fbcon_cc;
                fbcon_save_cr = fbcon_cr;
                fbcon_esc = 0;
            } else if (c == '8') {
                fbcon_hide_cursor();
                fbcon_cc = fbcon_save_cc;
                fbcon_cr = fbcon_save_cr;
                if (fbcon_cc >= fbcon_cols)
                    fbcon_cc = fbcon_cols - 1;
                if (fbcon_cr >= fbcon_rows)
                    fbcon_cr = fbcon_rows - 1;
                fbcon_esc = 0;
                fbcon_cursor_draw();
            } else if (c == 'D') {
                fbcon_esc = 0;
                fbcon_newline();
                fbcon_cursor_draw();
            } else {
                fbcon_esc = 0;
            }
            return;
        }
        if (c >= '0' && c <= '9') {
            if (fbcon_np < 7)
                fbcon_p[fbcon_np] = fbcon_p[fbcon_np] * 10u +
                                    (u64)(c - '0');
            return;
        }
        if (c == ';') {
            if (fbcon_np < 7)
                fbcon_np++;
            return;
        }
        if (c == '?') {
            fbcon_priv = 1;
            return;
        }
        if (c >= 0x20 && c <= 0x2F)
            return;                 /* intermediate byte */
        if (c < 0x40 || c > 0x7E) {
            fbcon_esc = fbcon_csi = 0;
            return;
        }
        fbcon_esc = fbcon_csi = 0;
        {
            int n = fbcon_np + 1;
            int p0 = (int)fbcon_p[0];

            if (n > 8)
                n = 8;
            switch (c) {
            case 'A':
                fbcon_move_up(p0 ? (u64)p0 : 1);
                break;
            case 'B':
                fbcon_move_down(p0 ? (u64)p0 : 1);
                break;
            case 'C':
                /* The nsh line editor ends every redraw with an ESC[C
                 * run, so a step must move only, never erase. */
                fbcon_move_right(p0 ? (u64)p0 : 1);
                break;
            case 'D':
                fbcon_move_left(p0 ? (u64)p0 : 1);
                break;
            case 'E':
                fbcon_move_down(p0 ? (u64)p0 : 1);
                fbcon_cc = 0;
                break;
            case 'F':
                fbcon_move_up(p0 ? (u64)p0 : 1);
                fbcon_cc = 0;
                break;
            case 'G':
                fbcon_hide_cursor();
                fbcon_cc = p0 ? (u64)p0 - 1 : 0;
                if (fbcon_cc >= fbcon_cols)
                    fbcon_cc = fbcon_cols - 1;
                break;
            case 'd':
                fbcon_hide_cursor();
                fbcon_cr = p0 ? (u64)p0 - 1 : 0;
                if (fbcon_cr >= fbcon_rows)
                    fbcon_cr = fbcon_rows - 1;
                break;
            case 'H':
            case 'f': {
                u64 r = fbcon_p[0] ? fbcon_p[0] - 1 : 0;
                u64 col = fbcon_p[1] ? fbcon_p[1] - 1 : 0;

                fbcon_hide_cursor();
                fbcon_cr = (r < fbcon_rows) ? r : fbcon_rows - 1;
                fbcon_cc = (col < fbcon_cols) ? col : fbcon_cols - 1;
                break;
            }
            case 'J':
                fbcon_erase_display(p0);
                break;
            case 'K':
                fbcon_erase_line(p0);
                break;
            case 'L':
                fbcon_insert_lines(p0 ? (u64)p0 : 1);
                break;
            case 'M':
                fbcon_delete_lines(p0 ? (u64)p0 : 1);
                break;
            case 'P':
                fbcon_delete_chars(p0 ? (u64)p0 : 1);
                break;
            case '@':
                fbcon_insert_chars(p0 ? (u64)p0 : 1);
                break;
            case 'X':
                fbcon_erase_chars(p0 ? (u64)p0 : 1);
                break;
            case 'm':
                fbcon_sgr(n);
                break;
            case 's':
                fbcon_save_cc = fbcon_cc;
                fbcon_save_cr = fbcon_cr;
                break;
            case 'u':
                fbcon_hide_cursor();
                fbcon_cc = fbcon_save_cc;
                fbcon_cr = fbcon_save_cr;
                if (fbcon_cc >= fbcon_cols)
                    fbcon_cc = fbcon_cols - 1;
                if (fbcon_cr >= fbcon_rows)
                    fbcon_cr = fbcon_rows - 1;
                break;
            case 'h':
            case 'l':
                if (fbcon_priv && p0 == 25) {
                    fbcon_visible = (c == 'h');
                    if (!fbcon_visible)
                        fbcon_hide_cursor();
                }
                break;
            default:
                /* Unsupported CSI (scroll regions, device reports...). */
                break;
            }
        }
        for (int i = 0; i < 8; i++)
            fbcon_p[i] = 0;
        fbcon_np = 0;
        fbcon_cursor_draw();
        return;
    }
    if (c == 0x1B) {
        fbcon_esc = 1;
        return;
    }

    if (c == '\n') {
        fbcon_newline();
    } else if (c == '\r') {
        fbcon_hide_cursor();
        fbcon_cc = 0;
    } else if (c == '\b' || c == 0x7F) {
        if (fbcon_cc > 0) {
            fbcon_hide_cursor();
            fbcon_cc--;
            fbcon_cell_cur(fbcon_cc, fbcon_cr, ' ');
        }
    } else if (c == '\t') {
        do {
            fbcon_hide_cursor();
            if (++fbcon_cc >= fbcon_cols) {
                fbcon_cc = 0;
                if (++fbcon_cr >= fbcon_rows) {
                    fbcon_cr = fbcon_rows - 1;
                    fbcon_scroll();
                }
            }
        } while (fbcon_cc % 8u);
    } else if ((unsigned char)c < 0x20) {
        /* BEL, SO/SI charset shifts, VT and FF have no cell of their own.
         * ncurses emits SI after every sgr0 on the Linux console; painting
         * it as '?' scattered garbage over full-screen applications. */
    } else {
        fbcon_hide_cursor();
        fbcon_cell_cur(fbcon_cc, fbcon_cr, c);
        if (++fbcon_cc >= fbcon_cols) {
            fbcon_cc = 0;
            if (++fbcon_cr >= fbcon_rows) {
                fbcon_cr = fbcon_rows - 1;
                fbcon_scroll();
            }
        }
    }
    fbcon_show_cursor();
}

static void fbcon_write(struct console_device *dev, char c)
{
    (void)dev;
    /* A userspace compositor owns the panel while this flag is set.  Kernel
     * logs and child stdout still reach the serial console and klog ring,
     * but must never paint over compositor pixels. */
    if (core_console_fb_gui_active())
        return;
    fbcon_putchar(c);
}

static struct console_device g_fbcon = {
    .name = "fbcon",
    .write = fbcon_write,
};

/* --- mouse arrow overlay ------------------------------------------------
 * The arrow is drawn directly into the panel (it is not part of the
 * grid). Moving it restores the grid cells underneath the old rectangle,
 * so only what fbcon itself printed survives; userland blits (desktop)
 * repaint at their own cadence and simply cover the arrow. */

#define FBM_W 12
#define FBM_H 16
static const char *fbm_arrow[FBM_H] = {
    "B...........",
    "BB..........",
    "BWB.........",
    "BWWB........",
    "BWWWB.......",
    "BWWWWB......",
    "BWWWWWB.....",
    "BWWWWWWB....",
    "BWWWBBBB....",
    "BBWB........",
    "BWB.........",
    "B..B........",
    "...BB.......",
    ".....B.B....",
    ".....BBB....",
    "......BB....",
};
static int fbm_x = -1, fbm_y = -1;

static void fb_rect_restore(int x, int y, int w, int h)
{
    u64 c0 = (u64)x / 8u, c1 = (u64)(x + w - 1) / 8u;
    u64 r0 = (u64)y / 8u, r1 = (u64)(y + h - 1) / 8u;
    u64 cc, cr;

    for (cr = r0; cr <= r1 && cr < fbcon_rows; cr++)
        for (cc = c0; cc <= c1 && cc < fbcon_cols; cc++)
            fbcon_restore_cell(cc, cr);
}

void fb_mouse_place(int x, int y, int buttons)
{
    int r, c;

    (void)buttons;
    if (!g_present || !fb_grid_ok)
        return;
    /* The GUI compositor draws its own cursor into a staging buffer.  The
     * kernel arrow would be a second pointer and its grid restore would
     * overwrite compositor pixels, so it stays off while GUI owns fbcon. */
    if (core_console_fb_gui_active())
        return;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if ((u64)x > g_w - 1)
        x = (int)g_w - 1;
    if ((u64)y > g_h - 1)
        y = (int)g_h - 1;

    if (fbm_x >= 0)
        fb_rect_restore(fbm_x, fbm_y, FBM_W, FBM_H);
    fbm_x = x;
    fbm_y = y;
    for (r = 0; r < FBM_H; r++) {
        for (c = 0; c < FBM_W; c++) {
            char px = fbm_arrow[r][c];
            if (px == '.')
                continue;
            fb_putpixel((u64)(x + c), (u64)(y + r),
                        px == 'W' ? 0xFFFFFFu : 0x000000u);
        }
    }
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