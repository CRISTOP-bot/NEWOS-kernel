/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/drivers/graphics/fbcon_csi.c
 * Description: fbcon CSI escape commands: cursor moves, erase and edits.
 * Added: 2026-10-30
 */

/* CSI (ESC [ ... final byte) command handlers for the framebuffer text
 * console: cursor movement, line/character erase, line/character insert
 * and delete, and SGR attribute parsing. fbcon_putchar() in fbcon.c owns
 * the escape state machine and dispatches here. */
#include <drivers/fb.h>
#include <iru_string.h>
#include "fb_priv.h"

void fbcon_erase_line(int mode)
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

void fbcon_erase_display(int mode)
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

void fbcon_move_up(u64 n)
{
    fbcon_hide_cursor();
    fbcon_cr = (fbcon_cr > n) ? fbcon_cr - n : 0;
}

void fbcon_move_down(u64 n)
{
    fbcon_hide_cursor();
    fbcon_cr += n;
    if (fbcon_cr >= fbcon_rows)
        fbcon_cr = fbcon_rows - 1;
}

void fbcon_move_left(u64 n)
{
    fbcon_hide_cursor();
    fbcon_cc = (fbcon_cc > n) ? fbcon_cc - n : 0;
}

void fbcon_move_right(u64 n)
{
    fbcon_hide_cursor();
    fbcon_cc += n;
    if (fbcon_cc >= fbcon_cols)
        fbcon_cc = fbcon_cols - 1;
}

void fbcon_insert_lines(u64 n)
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

void fbcon_delete_lines(u64 n)
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

void fbcon_insert_chars(u64 n)
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

void fbcon_delete_chars(u64 n)
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

void fbcon_erase_chars(u64 n)
{
    int fgi, bgi;
    u64 c;

    fbcon_hide_cursor();
    fbcon_attr_get(&fgi, &bgi);
    for (c = fbcon_cc; c < fbcon_cc + n && c < fbcon_cols; c++)
        fbcon_cell(c, fbcon_cr, ' ', fgi, bgi);
}

void fbcon_sgr(int n)
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
