/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/drivers/hda.h
 * Description: Intel HD-Audio (ICH6 HD-A 1.0) poll-mode playback driver API.
 * Added: 2026-10-22
 */
#ifndef DRIVERS_HDA_H
#define DRIVERS_HDA_H

#include <core/core_types.h>

/* Nonzero when the HD-A controller was probed on PCI and the link
 * answered (a codec is attached on serial address 0). */
int hda_present(void);

/* One-shot poll-mode playback.
 *
 * `pcm_phys` must be a fully contiguous physical buffer holding
 * little-endian PCM at the given rate/channels/bits (16-bit supported).
 * The driver programs the codec converter and the first OUTPUT stream
 * (SD slot 4) for a one-descriptor DMA round, runs it, polls
 * SD.LPIB until every byte has crossed, and stops the stream.
 * Returns bytes played, or a negative errno-style code. */
int hda_play(u64 pcm_phys, u32 bytes, u32 rate, u32 channels, u32 bits);

void hda_driver_register(void);

#endif
