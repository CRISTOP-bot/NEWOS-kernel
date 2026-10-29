/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/arch/x86_64/include/x86_pic.h
 * Description: NEWOS arch interface
 * Added: 2026-10-17
 */
#ifndef ARCH_X86_64_PIC_H
#define ARCH_X86_64_PIC_H

void x64_pic_init(void);
void x64_pic_send_eoi(int irq);
void x64_pic_set_mask(int irq, int masked);

#endif