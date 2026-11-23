/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/drivers/audio/audio_backend.c
 * Description: userland PCM playback backend for the HDA driver. Owns one
 *              contiguous DMA bounce buffer and validates/copies user PCM
 *              before handing it to hda_play(). Declared to the syscall
 *              layer through <syscall/syscall.h> (DRV may not be imported
 *              by SYSC directly).
 * Added: 2026-11-23
 */
#include <drivers/hda.h>
#include <core/core.h>
#include <mm/mm_pmm.h>
#include <mm/mm_usercopy.h>
#include <iru_string.h>

/* One audio stream at a time (a game owns the codec for its lifetime). */
#define AUDIO_ORDER      4                 /* 64 KiB contiguous */
#define AUDIO_BUF_BYTES  (1u << (12 + AUDIO_ORDER))

static u64     g_frame;        /* physical frame number, 0 = not open */
static u8     *g_dma;          /* virtual mapping of the DMA buffer */
static int     g_handle;       /* nonzero while open */

int audio_present(void)
{
    return hda_present();
}

long audio_open(void)
{
    if (g_handle)
        return -1;              /* already open */
    if (!hda_present())
        return -1;              /* no codec */
    if (phys_alloc_block(AUDIO_ORDER, &g_frame) != 0)
        return -1;
    g_dma = (u8 *)phys_to_virt(g_frame << 12);
    g_handle = 1;
    return g_handle;
}

/* Copy at most AUDIO_BUF_BYTES from user and play one synchronous round.
 * Format is fixed: 48000 Hz, stereo, 16-bit signed LE (what hda_play does). */
long audio_play(long handle, const void *user_pcm, u32 bytes)
{
    if (handle != g_handle || !g_dma || !user_pcm)
        return -1;
    if (bytes == 0 || (bytes & 3u) != 0)
        return -1;
    if (bytes > AUDIO_BUF_BYTES)
        bytes = AUDIO_BUF_BYTES;
    if (copy_from_user(g_dma, user_pcm, bytes) != 0)
        return -1;
    return hda_play(g_frame << 12, bytes, 48000, 2, 16);
}

long audio_close(long handle)
{
    if (handle != g_handle || !g_frame)
        return -1;
    /* Release the contiguous DMA block. */
    phys_free_block(g_frame, AUDIO_ORDER);
    g_frame = 0;
    g_dma = 0;
    g_handle = 0;
    return 0;
}
