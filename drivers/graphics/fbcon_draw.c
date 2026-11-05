/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/drivers/graphics/fbcon_draw.c
 * Description: fbcon rendering layer: cells, cursor, scrolling and overlays.
 * Added: 2026-10-30
 */

/* Text-console rendering for the framebuffer panel: the 8x8 glyph painter,
 * the reverse-video cursor overlay, the row scroll and the mouse arrow
 * overlay all paint straight into the panel through fb.c's window. The
 * text grid (character + attribute per cell) lives here so the cursor and
 * the mouse can restore exactly what they covered. */
#include <drivers/fb.h>
#include <core/core_console.h>
#include <iru_string.h>
#include "fb_priv.h"

/* --- fbcon: 8x8 text console on the panel ------------------------------- */

#include "font8x8_basic.h"

u64 fbcon_cols, fbcon_rows;
u64 fbcon_cc, fbcon_cr;
int fbcon_esc, fbcon_csi, fbcon_priv, fbcon_charset;
u64 fbcon_p[8];
int fbcon_fgi = 7, fbcon_bgi;    /* SGR indices into the 256 palette */
int fbcon_bold, fbcon_rev;
int fbcon_visible = 1, fbcon_cur_drawn;
u64 fbcon_cur_cc, fbcon_cur_cr;
u64 fbcon_save_cc, fbcon_save_cr;

/* Text grid: one char plus its palette attribute per cell. The mouse arrow
 * restores what it covers and the cursor is painted as reverse video, so
 * both need the colors of the cell underneath, not the current SGR state. */
char fb_grid_buf[FB_GRID_MAX];
u16 fb_grid_attr[FB_GRID_MAX];
int fb_grid_ok;

/* 16 ANSI colors, VGA-flavoured to match the text console. */
static const u32 fbcon_vga[16] = {
    0x000000u, 0xAA0000u, 0x00AA00u, 0xAA5500u,
    0x0000AAu, 0xAA00AAu, 0x00AAAAu, 0xAAAAAAu,
    0x555555u, 0xFF5555u, 0x55FF55u, 0xFFFF55u,
    0x5555FFu, 0xFF55FFu, 0x55FFFFu, 0xFFFFFFu,
};

u32 fbcon_pal256(int idx)
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
u16 fbcon_attr_pack(int fgi, int bgi)
{
    return (u16)((((u32)fgi & 0xFFu) << 8) | ((u32)bgi & 0xFFu));
}

void fbcon_attr_get(int *fgi, int *bgi)
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

void fbcon_cell(u64 cc, u64 cr, char ch, int fgi, int bgi)
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

void fbcon_cell_cur(u64 cc, u64 cr, char ch)
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

void fbcon_redraw_row(u64 cr)
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

void fbcon_cursor_draw(void)
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

void fbcon_scroll(void)
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

void fbcon_hide_cursor(void)
{
    fbcon_cursor_erase();
}

void fbcon_newline(void)
{
    fbcon_hide_cursor();
    fbcon_cc = 0;
    if (++fbcon_cr >= fbcon_rows) {
        fbcon_cr = fbcon_rows - 1;
        fbcon_scroll();
    }
}

void fbcon_show_cursor(void)
{
    fbcon_cursor_draw();
}

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
int fbm_x = -1, fbm_y = -1;

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

void fb_mouse_scroll(s8 delta)
{
    int steps = (int)delta;
    int dir = (steps > 0) ? -1 : 1; /* wheel up = scroll text up (line -1) */
    int n = (steps < 0) ? -steps : steps;
    int r;
    int fgi, bgi;

    if (!g_present || !fb_grid_ok || n <= 0)
        return;
    /* Preserve current SGR so the scrolled region keeps colors. Bold and
     * reverse are already folded in by fbcon_attr_get(). */
    fbcon_attr_get(&fgi, &bgi);

    if (dir < 0) {
        /* Scroll up: drop top line, shift rest up. */
        for (r = 0; r < (int)fbcon_rows - 1; r++) {
            u64 row_src = (u64)(r + 1) * fbcon_cols;
            u64 row_dst = (u64)r * fbcon_cols;
            memmove(fb_grid_buf + row_dst, fb_grid_buf + row_src,
                    (size_t)fbcon_cols);
            memmove(fb_grid_attr + row_dst, fb_grid_attr + row_src,
                    (size_t)fbcon_cols * sizeof(u16));
        }
        /* Clear bottom line with current colors. */
        for (r = 0; r < (int)fbcon_cols; r++) {
            fb_grid_buf[(fbcon_rows - 1) * fbcon_cols + r] = ' ';
            fb_grid_attr[(fbcon_rows - 1) * fbcon_cols + r] =
                fbcon_attr_pack(fgi, bgi);
        }
    } else {
        /* Scroll down: drop bottom, shift rest down. */
        for (r = (int)fbcon_rows - 2; r >= 0; r--) {
            u64 row_src = (u64)r * fbcon_cols;
            u64 row_dst = (u64)(r + 1) * fbcon_cols;
            memmove(fb_grid_buf + row_dst, fb_grid_buf + row_src,
                    (size_t)fbcon_cols);
            memmove(fb_grid_attr + row_dst, fb_grid_attr + row_src,
                    (size_t)fbcon_cols * sizeof(u16));
        }
        for (r = 0; r < (int)fbcon_cols; r++) {
            fb_grid_buf[r] = ' ';
            fb_grid_attr[r] = fbcon_attr_pack(fgi, bgi);
        }
    }
    /* Redraw whole grid with current SGR. */
    fbcon_hide_cursor();
    for (r = 0; r < (int)fbcon_rows; r++)
        fbcon_redraw_row((u64)r);
    fbcon_cursor_draw();
}
