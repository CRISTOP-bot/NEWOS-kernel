; ============================================================================
; NEWOS x86_64 process/thread primitives
; ============================================================================
; Context switches, the user-entry iretq path and stack teleports used to
; move kernel boot onto a dedicated kernel-half stack.
;
; struct thread layout (offset names shared with C via thread.h):
;   0   process *
;   8   kernel_stack_top
;   16  entry_sp        (points at an iretq frame on the kernel stack)
;   24  cr3             (physical PML4 for the thread's address space)
;   32  sp              (scratch saved stack pointer)
; ============================================================================

; void x64_context_switch(u64 *from, u64 to)
;   Save callee-saved registers + RSP to *from, then restore from `to`
;   (a frame previously captured by this same function) and return.
[GLOBAL x64_context_switch]
x64_context_switch:
    push rbp
    mov  rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov  [rdi], rsp
    mov  rsp, rsi
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp
    ret

; void x64_context_save(u64 *from)
;   Like the save half of x64_context_switch, but returns to the caller
;   instead of restoring another frame. This captures the current RSP
;   (e.g. as a resume point) without switching.
[GLOBAL x64_context_save]
x64_context_save:
    push rbp
    mov  rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov  [rdi], rsp
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp
    ret

; void x64_enter_user(struct thread *t)
;   Never returns. Loads the thread's CR3, switches RSP to the prepared
;   iretq frame on the thread kernel stack and returns to user mode.
;
;   Unlike a scheduler resume (which rebuilds the whole iframe through the
;   interrupt epilogue), this is the very first entry. iretq restores only
;   rip/cs/rflags/rsp/ss, so the SysV entry registers must be loaded here or
;   the first process would see leftover kernel values instead of the
;   thread_set_user_args() pair. struct x64_iframe places rdi at entry_sp-64
;   and rsi at entry_sp-56.
[GLOBAL x64_enter_user]
x64_enter_user:
    mov  rax, [rdi + 24]        ; t->cr3
    mov  cr3, rax
    mov  rsp, [rdi + 16]        ; t->entry_sp (&iframe.rip)
    mov  rdi, [rsp - 64]        ; iframe.rdi = argc
    mov  rsi, [rsp - 56]        ; iframe.rsi = argv
    iretq

; void x64_goto_stack(u64 new_rsp, void (*fn)(u64), u64 arg)
;   Teleport the kernel onto `new_rsp` (kernel-half VA, 16-aligned) and call
;   fn(arg). fn is expected not to return.
[GLOBAL x64_goto_stack]
x64_goto_stack:
    mov  rsp, rdi
    and  rsp, -16
    mov  rdi, rdx
    call rsi
.cli:
    cli
.hang:
    hlt
    jmp .hang