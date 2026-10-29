/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/core/core_cmdline.c
 * Description: Boot-protocol strings live in loader-owned physical memory and can
 * Added: 2026-10-17
 */
#include <core/core_bootinfo.h>
#include <core/core_types.h>
#include <iru_string.h>

static char g_cmdline_buf[512];
const char *g_boot_cmdline = "";

void boot_capture_cmdline(const char *cmd)
{
    if (!cmd)
        return;

    /* Boot-protocol strings live in loader-owned physical memory and can
     * be reused by the PMM after early boot. Keep a kernel-owned copy so
     * /proc/cmdline and the test-mode checks never dereference freed or
     * recycled boot pages. */
    size_t n = strnlen(cmd, sizeof(g_cmdline_buf) - 1);
    memcpy(g_cmdline_buf, cmd, n);
    g_cmdline_buf[n] = '\0';
    g_boot_cmdline = g_cmdline_buf;
}

int boot_cmdline_has(const char *needle)
{
    const char *hay = g_boot_cmdline;
    size_t n = strlen(needle);

    if (!hay || !n)
        return 0;
    while (*hay) {
        while (*hay && (*hay == ' ' || *hay == '\t'))
            hay++;
        if (!strncmp(hay, needle, n) &&
            (hay[n] == '\0' || hay[n] == ' ' || hay[n] == '\t')) {
            return 1;
        }
        while (*hay && *hay != ' ') {
            if (*hay == '\t')
                break;
            hay++;
        }
    }
    return 0;
}

void boot_cmdline_read_value(const char *key, char *out, size_t out_size)
{
    const char *hay = g_boot_cmdline;
    size_t klen = strlen(key);
    if (!hay || !klen || !out || out_size == 0)
        return;
    out[0] = '\0';
    while (*hay) {
        while (*hay && (*hay == ' ' || *hay == '\t'))
            hay++;
        if (!strncmp(hay, key, klen) &&
            (hay[klen] == '=' || hay[klen] == '\0' || hay[klen] == ' ' || hay[klen] == '\t')) {
            if (hay[klen] == '=') {
                hay += klen + 1;
                size_t i = 0;
                while (*hay && *hay != ' ' && *hay != '\t' && i < out_size - 1) {
                    out[i++] = *hay++;
                }
                out[i] = '\0';
                return;
            }
            return;
        }
        while (*hay && *hay != ' ') {
            if (*hay == '\t')
                break;
            hay++;
        }
    }
}