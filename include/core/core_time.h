/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/core/core_time.h
 * Description: Kernel time services. Wrapped in core so higher layers (syscall, ...) can
 * Added: 2026-10-17
 */
#ifndef CORE_TIME_H
#define CORE_TIME_H

#include <core/core_types.h>
#include <abi/syscall_abi.h>

/* Kernel time services. Wrapped in core so higher layers (syscall, ...) can
 * use the PIT tick without depending on a device driver. */

u64 core_time_ticks(void);
u64 core_time_hz(void);
void core_time_delay_us(u64 us);

/* Wall clock (CMOS RTC). 0 on success. */
int core_time_wallclock(struct nsh_time *t);

#endif