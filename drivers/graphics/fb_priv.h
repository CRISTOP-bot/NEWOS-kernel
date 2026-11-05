/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/drivers/graphics/fb_priv.h
 * Description: Private glue between fb.c and its fbcon sibling files.
 * Added: 2026-10-30
 */
#ifndef DRIVERS_FB_PRIV_H
#define DRIVERS_FB_PRIV_H

#include <core/core_types.h>
#include <drivers/tty_console.h>

/* Private glue for the framebuffer driver split: fb.c owns the panel mode
 * and its uncacheable window, fbcon_draw.c owns the text grid plus the
 * painting of cells/cursor/overlays, fbcon_csi.c implements the CSI
 * escape commands and fbcon.c holds the escape parser and the console
 * backend. This header carries the state and helpers those files share;
 * it is driver-internal and part of no ABI. */

#define FBCON_BG 0x000000u
#define FB_GRID_MAX (160u * 128u)

/* Panel mode state, written once by fb_init_early() (fb.c). */
extern int g_present;
extern u64 g_w, g_h, g_pitch;
extern u32 g_bpp;
extern u8 *g_fb;

/* XRGB8888 packer shared with the fbcon renderer (fb.c). */
u32 fb_pack(u8 r, u8 g, u8 b);

/* fbcon console and cursor state (fbcon_draw.c). */
extern u64 fbcon_cols, fbcon_rows;
extern u64 fbcon_cc, fbcon_cr;
extern int fbcon_esc, fbcon_csi, fbcon_priv, fbcon_charset;
extern u64 fbcon_p[8];
extern int fbcon_fgi, fbcon_bgi;    /* SGR indices into the 256 palette */
extern int fbcon_bold, fbcon_rev;
extern int fbcon_visible, fbcon_cur_drawn;
extern u64 fbcon_cur_cc, fbcon_cur_cr;
extern u64 fbcon_save_cc, fbcon_save_cr;

/* Text grid: one char plus its palette attribute per cell. */
extern char fb_grid_buf[FB_GRID_MAX];
extern u16 fb_grid_attr[FB_GRID_MAX];
extern int fb_grid_ok;

/* Mouse arrow overlay state (fbcon_draw.c). */
extern int fbm_x, fbm_y;

/* Registered fbcon backend (fbcon.c), consumed by fb_init_early(). */
extern struct console_device g_fbcon;

/* Shared fbcon helpers. */
u32 fbcon_pal256(int idx);
u16 fbcon_attr_pack(int fgi, int bgi);
void fbcon_attr_get(int *fgi, int *bgi);
void fbcon_cell(u64 cc, u64 cr, char ch, int fgi, int bgi);
void fbcon_cell_cur(u64 cc, u64 cr, char ch);
void fbcon_redraw_row(u64 cr);
void fbcon_cursor_draw(void);
void fbcon_scroll(void);
void fbcon_hide_cursor(void);
void fbcon_newline(void);
void fbcon_show_cursor(void);
void fbcon_erase_line(int mode);
void fbcon_erase_display(int mode);
void fbcon_move_up(u64 n);
void fbcon_move_down(u64 n);
void fbcon_move_left(u64 n);
void fbcon_move_right(u64 n);
void fbcon_insert_lines(u64 n);
void fbcon_delete_lines(u64 n);
void fbcon_insert_chars(u64 n);
void fbcon_delete_chars(u64 n);
void fbcon_erase_chars(u64 n);
void fbcon_sgr(int n);

#endif
