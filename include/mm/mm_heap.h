/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/mm/mm_heap.h
 * Description: Every pointer below is at least 16-byte aligned (HEAP_ALIGN), which is
 * Added: 2026-10-17
 */
#ifndef MM_HEAP_KMALLOC_H
#define MM_HEAP_KMALLOC_H

#include <core/core_types.h>

void  *kmalloc(size_t size);
void  *kzalloc(size_t size);
void  *krealloc(void *ptr, size_t size);
void   kfree(void *ptr);
void   kheap_dump(void);

/* Every pointer below is at least 16-byte aligned (HEAP_ALIGN), which is
 * what an FXSAVE image in struct thread needs. */

/* Early heap bootstrap. */
void   kheap_init(void *arena, size_t size);
void   kheap_init_early(void);

#endif