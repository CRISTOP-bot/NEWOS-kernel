/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/mm/mm_debug.h
 * Description: NEWOS headers interface
 * Added: 2026-10-17
 */
#ifndef KERNEL_MEMDEBUG_H
#define KERNEL_MEMDEBUG_H

#include <core/core_types.h>

int  memory_debug_enabled(void);
void memory_debug_track(void *ptr, size_t size);
int  memory_debug_verify(const void *ptr, size_t size);
void memory_debug_dump(void);

#endif