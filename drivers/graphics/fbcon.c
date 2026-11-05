/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/drivers/graphics/fbcon.c
 * Description: fbcon front end: escape parser, console write and backend.
 * Added: 2026-10-30
 */

/* Front end of the framebuffer text console: fbcon_putchar() runs the
 * VT/xterm escape state machine (ESC, CSI, charset designators, SGR) and
 * emits glyphs through the fbcon_draw.c painter; fbcon_write() is the
 * console backend that fb_init_early() registers. */
#include <drivers/tty_console.h>
#include <core/core_console.h>
#include "fb_priv.h"

static int fbcon_np;

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

struct console_device g_fbcon = {
    .name = "fbcon",
    .write = fbcon_write,
};
