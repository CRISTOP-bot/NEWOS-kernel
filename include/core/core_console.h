/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/core/core_console.h
 * Description: Kernel console services.
 * Added: 2026-10-17
 */
#ifndef CORE_CONSOLE_H
#define CORE_CONSOLE_H

#include <core/core_types.h>

/* Kernel console services.
 *
 * Input has two producers: the serial RX interrupt (COM1) and the PS/2
 * keyboard interrupt (anything typed in the QEMU/VGA window). Both feed
 * byte queues; higher layers (SYS_READ, the shell) consume single merged
 * bytes without pulling in a device driver. Serial is drained first so a
 * pasted serial burst keeps its ordering ahead of interleaved keystrokes.
 */

int core_console_rx_available(void);
int core_console_rx_pop(void);          /* -1 when empty */
int core_console_rx_peek(void);         /* oldest queued byte, -1 if none */

/* PS/2 keyboard pushes decoded bytes here (IRQ context). */
void core_console_kbd_push(u8 c);

/* Graphical-session input ownership.
 *
 * A full-screen GUI needs the PS/2 keyboard without also feeding the shell
 * behind it. While a session holds the grab, decoded bytes go to a private
 * ring read by SYS_KBD_READ instead of the console ring. Only one process
 * may own the grab; process teardown calls core_console_kbd_release_if() so
 * a crashed desktop can never leave the keyboard stolen. */
int core_console_kbd_grab(int pid, int on);
int core_console_kbd_gui_read(u8 *buf, int len);   /* non-blocking, 0 = empty */
void core_console_kbd_release_if(int pid);

/* Framebuffer text-console ownership.
 *
 * A userspace compositor draws directly through SYS_FBWRITE. While it owns
 * the panel, fbcon must not paint kernel logs, child stdout or the legacy
 * arrow over the compositor's pixels. The owner is released automatically
 * when its process exits, so a crashed desktop cannot hide the console. */
int core_console_fb_gui(int pid, int on);
int core_console_fb_gui_active(void);
void core_console_fb_gui_release_if(int pid);

#endif
