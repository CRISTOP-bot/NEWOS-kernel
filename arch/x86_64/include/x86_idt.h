/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/arch/x86_64/include/x86_idt.h
 * Description: Interrupt gate types
 * Added: 2026-10-17
 */
#ifndef ARCH_X86_64_IDT_H
#define ARCH_X86_64_IDT_H

#include <core/core_types.h>

#define IDT_ENTRIES 256

/* Interrupt gate types */
#define IDT_TYPE_INTERRUPT 0x0E
#define IDT_TYPE_TRAP      0x0F

void x64_idt_init(void);
void x64_idt_set_gate(int vec, void (*handler)(void), u8 dpl);
void x64_idt_reload(void);

void x64_exception_dispatch(int vec, void *frame);

#endif