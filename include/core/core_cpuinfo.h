/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/core/core_cpuinfo.h
 * Description: Architecture-neutral CPU description hook for procfs.
 * Added: 2026-11-23
 */
#ifndef CORE_CPUINFO_H
#define CORE_CPUINFO_H

#include <core/core_types.h>

/* Render a short /proc/cpuinfo body into `buf` (may be called repeatedly
 * with a short buffer; the caller handles VFS offsets). Returns the full
 * rendered length, or (size_t)-1 on error. Implemented by each arch. */
size_t arch_cpuinfo_render(char *buf, size_t size);

#endif
