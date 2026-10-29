/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/core/core_selftest.c
 * Description: Phase 2 unit tests: PMM phys wrappers, address-space lifecycle + CR3
 * Added: 2026-10-17
 */
#include <core/core_printk.h>
#include <drivers/pit_timer.h>
#include <process/sched.h>
#include <mm/mm_pmm.h>
#include <mm/mm_vmm.h>
#include <process/proc_elf.h>
#include <process/proc_process.h>
#include <syscall/syscall.h>
#include <fs/vfs.h>
#include <mm/mm_usercopy.h>
#include <mm/mm_heap.h>
#include <abi/syscall_abi.h>
#include <x86_frame.h>
#include <x86_cpu.h>
#include <x86_fpu.h>
#include <drivers/ata.h>
#include <drivers/hda.h>
#include <net/net_stack.h>
#include <iru_string.h>

/* Phase 2 unit tests: PMM phys wrappers, address-space lifecycle + CR3
 * switching, user-range validation/usercopy, ELF loading, and syscall
 * dispatch. Each returns 0 on success. */

/* ---------------- data helpers ---------------- */

static int load_file(const char *path, u8 **out, size_t *out_len)
{
    struct vfs_file *f = vfs_open(path, VFS_O_READ);
    if (!f)
        return -1;

    size_t cap = 4096, len = 0;
    u8 *buf = kmalloc(cap);
    if (!buf) {
        vfs_close(f);
        return -1;
    }
    for (;;) {
        if (len == cap) {
            cap *= 2;
            u8 *nb = kmalloc(cap);
            if (!nb) {
                kfree(buf);
                vfs_close(f);
                return -1;
            }
            memcpy(nb, buf, len);
            kfree(buf);
            buf = nb;
        }
        int n = vfs_read(f, buf + len, cap - len);
        if (n <= 0)
            break;
        len += (size_t)n;
    }
    vfs_close(f);

    *out = buf;
    *out_len = len;
    return 0;
}

/* ---------------- tests ---------------- */

static int ktest2_phys(void)
{
    if (!pmm_is_initialized())
        return -1;

    /* Alloc marks a frame used; freeing returns it to the free pool. */
    u64 f = PMM_INVALID_FRAME;
    if (phys_alloc_frame(&f) != 0 || f == PMM_INVALID_FRAME)
        return -1;
    if (phys_is_reserved(f) == 0) {
        phys_free_frame(f);
        return -1;
    }
    phys_free_frame(f);
    if (phys_is_reserved(f) != 0)
        return -1;

    /* Reserve/release a low frame (part of the kernel image reservation:
     * the first tracked frame 0x100000 is reserved at boot). */
    u64 base = 0x100000;
    if (phys_is_reserved(base >> PAGE_SHIFT) == 0)
        return -1;
    phys_release_range(base, base + 0x1000);
    if (phys_is_reserved(base >> PAGE_SHIFT) != 0)
        return -1;
    phys_reserve_range(base, base + 0x1000);
    if (phys_is_reserved(base >> PAGE_SHIFT) == 0)
        return -1;

    /* Block alloc/free round trip. */
    u64 b = PMM_INVALID_FRAME;
    if (phys_alloc_block(2, &b) != 0)
        return -1;
    if (phys_is_reserved(b) == 0) {
        phys_free_block(b, 2);
        return -1;
    }
    phys_free_block(b, 2);
    return 0;
}

static int ktest2_address_space(void)
{
    struct vmm_address_space *as;
    if (vmm_address_space_create("test-as", &as) != 0)
        return -1;

    /* Kernel half must be inherited. */
    u32 kflags = 0;
    if (!vmm_page_lookup(as, KERNEL_BASE_VA, NULL, &kflags))
        return -1;
    if (!(kflags & VMM_PRESENT))
        return -1;

    uintptr_t va = (uintptr_t)USER_SPACE_BASE;
    if (vmm_alloc_page(as, va, VMM_USER | VMM_WRITE | VMM_NX) != 0)
        goto fail;

    u32 flags = 0;
    if (!vmm_page_lookup(as, va, NULL, &flags))
        goto fail;
    if ((flags & (VMM_PRESENT | VMM_USER)) != (VMM_PRESENT | VMM_USER))
        goto fail;

    if (vmm_validate_user_range(as, va, PAGE_SIZE) != 0)
        goto fail;
    if (vmm_validate_user_range(as, va - 1, PAGE_SIZE) == 0)
        goto fail;
    if (vmm_validate_user_range(as, (uintptr_t)USER_SPACE_END - 512, 1024) == 0)
        goto fail;

    if (vmm_unmap_pages(as, va, 1) != 0)
        goto fail;

    /* Kernel-half mappings must be rejected. */
    if (vmm_alloc_page(as, KERNEL_BASE_VA, VMM_PRESENT) == 0)
        goto fail;

    vmm_address_space_destroy(as);
    return 0;

fail:
    vmm_address_space_destroy(as);
    return -1;
}

static int ktest2_usercopy(void)
{
    struct vmm_address_space *as;
    if (vmm_address_space_create("copy-as", &as) != 0)
        return -1;

    uintptr_t va = (uintptr_t)USER_SPACE_BASE;
    if (vmm_alloc_page(as, va, VMM_USER | VMM_WRITE | VMM_NX) != 0)
        goto fail;

    if (vmm_switch_to(as) != 0)
        goto fail;

    static const char msg[] = "usercopy-data";
    char out[32] = { 0 };

    /* Write the payload via kernel map, then read it back as a guest. */
    u32 f = 0;
    uintptr_t phys = 0;
    if (!vmm_page_lookup(as, va, &phys, &f))
        goto fail_back;
    memcpy((void *)phys_to_virt(phys), msg, sizeof(msg));
    if (copy_from_user(out, (const void *)va, sizeof(msg)) != 0)
        goto fail_back;
    if (memcmp(out, msg, sizeof(msg)) != 0)
        goto fail_back;
    if (copy_to_user((void *)va, msg, sizeof(msg)) != 0)
        goto fail_back;

    /* Invalid ranges must be refused, not copied. */
    if (copy_from_user(out, (const void *)(USER_SPACE_END - 64), 128) == 0)
        goto fail_back;

    if (vmm_switch_to(vmm_kernel_space()) != 0)
        goto fail;

    vmm_address_space_destroy(as);
    return 0;

fail_back:
    vmm_switch_to(vmm_kernel_space());
fail:
    vmm_address_space_destroy(as);
    return -1;
}

static int ktest2_elf(void)
{
    u8 *buf;
    size_t len;
    if (load_file("/bin/hello", &buf, &len) != 0)
        return -1;

    struct elf_image img = { .data = buf, .size = len };
    if (elf_validate(&img) != 0)
        goto fail;

    struct vmm_address_space *as;
    if (vmm_address_space_create("elf-as", &as) != 0)
        goto fail;

    if (elf_load_segments(&img, as) != 0)
        goto fail_as;
    if (elf_entry_valid(&img, as) != 0)
        goto fail_as;

    /* Entry page must be present, user, executable (no NX). */
    u32 flags = 0;
    if (!vmm_page_lookup(as, img.entry, NULL, &flags))
        goto fail_as;
    if (!(flags & VMM_PRESENT) || !(flags & VMM_USER))
        goto fail_as;
    if (flags & VMM_NX)
        goto fail_as;

    /* A garbage image must be rejected. */
    {
        u8 bad[64];
        memset(bad, 0x41, sizeof(bad));
        struct elf_image bimg = { .data = bad, .size = sizeof(bad) };
        if (elf_validate(&bimg) == 0)
            goto fail_as;
    }

    vmm_address_space_destroy(as);
    kfree(buf);
    return 0;

fail_as:
    vmm_address_space_destroy(as);
fail:
    kfree(buf);
    return -1;
}

static int ktest2_syscall(void)
{
    /* GETPID with no current process must fail cleanly. */
    if (process_current() != NULL)
        return -1;

    struct x64_iframe f;
    memset(&f, 0, sizeof(f));
    f.rax = SYS_GETPID;
    if (syscall_dispatch(&f) != SYSCALL_RET_ERROR)
        return -1;

    /* A real process: GETPID returns its pid, a bad fd is refused, and a
     * write through the serial console rounds-trips. */
    struct process *p = process_create_from_vfs("/bin/hello", "self-test");
    if (!p)
        return -1;

    process_current_set(p);
    f.rax = SYS_GETPID;
    f.rdi = f.rsi = f.rdx = 0;
    if (syscall_dispatch(&f) != (long)p->pid)
        goto fail;

    f.rax = SYS_WRITE;
    f.rdi = PROCESS_MAX_FDS + 1;        /* bogus fd */
    f.rsi = (u64)USER_SPACE_BASE;
    f.rdx = 4;
    if (syscall_dispatch(&f) != SYSCALL_RET_ERROR)
        goto fail;

    /* Valid write: map a page in the process's own address space with a
     * message and push it out via SYS_WRITE. */
    uintptr_t va = (uintptr_t)USER_SPACE_BASE + (uintptr_t)USER_SPACE_END / 2;
    va &= ~(uintptr_t)(PAGE_SIZE - 1);
    if (vmm_alloc_page(p->space, va, VMM_USER | VMM_WRITE | VMM_NX) != 0)
        goto fail;
    uintptr_t phys = 0;
    if (!vmm_page_lookup(p->space, va, &phys, NULL))
        goto fail;
    memcpy((void *)phys_to_virt(phys), "syscall-test\n", 13);

    if (vmm_switch_to(p->space) != 0)
        goto fail;
    f.rax = SYS_WRITE;
    f.rdi = STDOUT_FILENO;
    f.rsi = va;
    f.rdx = 13;
    if (syscall_dispatch(&f) != 13)
        goto fail_back;
    if (vmm_switch_to(vmm_kernel_space()) != 0)
        goto fail;

    process_current_set(NULL);
    process_free(p);
    return 0;

fail_back:
    vmm_switch_to(vmm_kernel_space());
fail:
    process_current_set(NULL);
    process_free(p);
    return -1;
}

/* ---------------- x87/SSE state ---------------- */

/* Three images plus the one the registers held on entry. All 16-byte aligned:
 * FXSAVE/FXRSTOR fault with #GP on a misaligned operand. */
static u8 fpu_live[X64_FPU_AREA_SIZE] __attribute__((aligned(X64_FPU_AREA_ALIGN)));
static u8 fpu_a[X64_FPU_AREA_SIZE] __attribute__((aligned(X64_FPU_AREA_ALIGN)));
static u8 fpu_b[X64_FPU_AREA_SIZE] __attribute__((aligned(X64_FPU_AREA_ALIGN)));

static u16 fpu_get_u16(const u8 *a, u64 off)
{
    u16 v;
    memcpy(&v, a + off, sizeof(v));
    return v;
}

static u32 fpu_get_u32(const u8 *a, u64 off)
{
    u32 v;
    memcpy(&v, a + off, sizeof(v));
    return v;
}

static void fpu_put_u16(u8 *a, u64 off, u16 v) { memcpy(a + off, &v, sizeof(v)); }
static void fpu_put_u32(u8 *a, u64 off, u32 v) { memcpy(a + off, &v, sizeof(v)); }
static void fpu_put_u64(u8 *a, u64 off, u64 v) { memcpy(a + off, &v, sizeof(v)); }

/* The fields the register bank must carry through a save/restore pair.
 * FSW and the FOP/FIP/FDP pointer fields are deliberately left out: the CPU
 * does not restore them as written, so comparing them would be flaky. */
static int fpu_cmp(const u8 *x, const u8 *y)
{
    if (fpu_get_u16(x, X64_FPU_FCW_OFF) != fpu_get_u16(y, X64_FPU_FCW_OFF))
        return -1;
    if (x[X64_FPU_FTW_OFF] != y[X64_FPU_FTW_OFF])
        return -1;
    if (fpu_get_u32(x, X64_FPU_MXCSR_OFF) != fpu_get_u32(y, X64_FPU_MXCSR_OFF))
        return -1;
    for (int i = 0; i < 16; i++)
        if (memcmp(x + X64_FPU_XMM_OFF + (u64)i * 16,
                   y + X64_FPU_XMM_OFF + (u64)i * 16, 16) != 0)
            return -1;
    for (int i = 0; i < 8; i++)
        if (memcmp(x + X64_FPU_ST_OFF + (u64)i * 16,
                   y + X64_FPU_ST_OFF + (u64)i * 16, 10) != 0)
            return -1;
    return 0;
}

static int ktest2_fpu(void)
{
    if (!x64_fpu_available()) {
        printk("fpu: CPU has no FXSR - state handling not testable here\n");
        return 0;
    }
    /* Every gcc x86-64 build emits SSE2 for float/double: without it no real
     * user binary can run, so a missing SSE2 is a failure of this milestone. */
    if (!x64_fpu_sse2())
        return -1;

    /* x64_fpu_enable() must have left the state accessible (TS clear is what
     * makes FP/SSE instructions execute instead of trapping #NM). */
    u64 cr0 = read_cr0();
    if ((cr0 & (X64_CR0_TS | X64_CR0_EM)) != 0 || !(cr0 & X64_CR0_MP))
        return -1;
    u64 want = X64_CR4_OSFXSR | X64_CR4_OSMMX;
    if ((read_cr4() & want) != want)
        return -1;

    /* The test rewrites the whole register bank; park it first. Phase 2 runs
     * before any user context exists, so nothing real is being clobbered. */
    x64_fpu_save(fpu_live);

    /* 1. The image every new thread starts from. */
    x64_fpu_area_init(fpu_a);
    if (fpu_get_u16(fpu_a, X64_FPU_FCW_OFF) != X64_FPU_FCW_INIT ||
        fpu_get_u32(fpu_a, X64_FPU_MXCSR_OFF) != X64_FPU_MXCSR_INIT ||
        fpu_a[X64_FPU_FTW_OFF] != X64_FPU_FTW_EMPTY)
        goto fail;
    for (int i = 0; i < 16; i++)
        for (int b = 0; b < 16; b++)
            if (fpu_a[X64_FPU_XMM_OFF + i * 16 + b] != 0)
                goto fail;

    /* 2. It must actually be loadable and stable (a bad image #GP's here). */
    x64_fpu_load(fpu_a);
    x64_fpu_save(fpu_b);
    if (fpu_cmp(fpu_a, fpu_b) != 0)
        goto fail;

    /* 3. A distinctive state, the case that matters for the context switch:
     * every XMM register and every x87 slot carrying its own bit pattern, a
     * non-default MXCSR and all tags valid (so the x87 slots really move). */
    x64_fpu_area_init(fpu_a);
    fpu_put_u32(fpu_a, X64_FPU_MXCSR_OFF, 0x1FA1);  /* DE+IE flags set */
    fpu_a[X64_FPU_FTW_OFF] = 0x00;                  /* all 8 tags valid */
    for (int i = 0; i < 16; i++) {
        u64 base = 0xF000000000000000ull + ((u64)i << 32);
        fpu_put_u64(fpu_a, X64_FPU_XMM_OFF + (u64)i * 16, base | 0x01234567ull);
        fpu_put_u64(fpu_a, X64_FPU_XMM_OFF + (u64)i * 16 + 8,
                    base + 0x89ABCDEFull);
    }
    for (int i = 0; i < 8; i++) {
        /* Valid normalized 80-bit number: integer bit set, bias 0x3FFF. */
        fpu_put_u64(fpu_a, X64_FPU_ST_OFF + (u64)i * 16,
                    0x8000000000000000ull | (u64)(i + 1));
        fpu_put_u16(fpu_a, X64_FPU_ST_OFF + (u64)i * 16 + 8, 0x3FFF);
    }
    x64_fpu_load(fpu_a);
    x64_fpu_save(fpu_b);
    if (fpu_cmp(fpu_a, fpu_b) != 0)
        goto fail;

    /* 4. Switching back and forth must not leak one context's numbers into
     * the other (this is the exact pair the epilogue runs per handoff). */
    x64_fpu_load(fpu_live);
    x64_fpu_load(fpu_a);
    x64_fpu_load(fpu_live);
    x64_fpu_save(fpu_b);
    if (fpu_cmp(fpu_live, fpu_b) != 0)
        goto fail;

    x64_fpu_load(fpu_live);
    return 0;

fail:
    x64_fpu_load(fpu_live);
    return -1;
}

/* ---------------- driver ---------------- */

static int ktest2_timer(void)
{
    u64 t0 = pit_ticks();
    time_delay_us(10000);
    u64 t1 = pit_ticks();

    if (t1 - t0 < 10)
        return -1;
    return 0;
}

/* SYS_OPEN / SYS_CLOSE / SYS_READ-through-VFS, driven through the real
 * dispatcher with a real user address space active (usercopy validated). */
static int ktest2_open_close(void)
{
    struct process *p = process_create_from_vfs("/bin/hello", "opentest");
    if (!p)
        return -1;

    process_current_set(p);

    /* One scratch user page: path strings at +0, data buffer at +256. */
    uintptr_t va = (uintptr_t)USER_SPACE_BASE + (uintptr_t)USER_SPACE_END / 2;
    va &= ~(uintptr_t)(PAGE_SIZE - 1);
    if (vmm_alloc_page(p->space, va, VMM_USER | VMM_WRITE | VMM_NX) != 0)
        goto fail;
    uintptr_t phys = 0;
    if (!vmm_page_lookup(p->space, va, &phys, NULL))
        goto fail;
    u8 *up = (u8 *)phys_to_virt(phys);
    const uintptr_t va_data = va + 256;

    if (vmm_switch_to(p->space) != 0)
        goto fail;

    struct x64_iframe f;
    memset(&f, 0, sizeof(f));

    /* Open an existing file and read it through the new SYS_READ path.
     * SYS_OPEN speaks the Linux O_* flags (the ABI userland and real
     * binaries use), so the test must not pass internal VFS_O_* here. */
    memcpy(up, "/etc/version", 13);
    f.rax = SYS_OPEN;
    f.rdi = va;
    f.rsi = NSH_O_RDONLY;
    long fd = syscall_dispatch(&f);
    if (fd < 3 || fd >= PROCESS_MAX_FDS)
        goto fail_back;

    memset(&f, 0, sizeof(f));
    f.rax = SYS_READ;
    f.rdi = (u64)fd;
    f.rsi = va_data;
    f.rdx = 32;
    long n = syscall_dispatch(&f);
    if (n <= 0)
        goto fail_back;
    {
        char out[40];
        memset(out, 0, sizeof(out));
        if (copy_from_user(out, (const void *)va_data, (size_t)n) != 0)
            goto fail_back;
        if (n < 5 || memcmp(out, "NEWOS", 5) != 0)
            goto fail_back;
    }

    /* Close works once; a second close of the same fd is refused. */
    memset(&f, 0, sizeof(f));
    f.rax = SYS_CLOSE;
    f.rdi = (u64)fd;
    if (syscall_dispatch(&f) != 0)
        goto fail_back;
    memset(&f, 0, sizeof(f));
    f.rax = SYS_CLOSE;
    f.rdi = (u64)fd;
    if (syscall_dispatch(&f) != SYSCALL_RET_ERROR)
        goto fail_back;

    /* O_CREATE round trip: write through SYS_WRITE, read back. */
    memcpy(up, "/tmp/open-test", 15);
    memset(&f, 0, sizeof(f));
    f.rax = SYS_OPEN;
    f.rdi = va;
    f.rsi = NSH_O_RDWR | NSH_O_CREAT;
    fd = syscall_dispatch(&f);
    if (fd < 0)
        goto fail_back;
    memcpy(up + 256, "hello-open", 10);
    memset(&f, 0, sizeof(f));
    f.rax = SYS_WRITE;
    f.rdi = (u64)fd;
    f.rsi = va_data;
    f.rdx = 10;
    if (syscall_dispatch(&f) != 10)
        goto fail_back;
    memset(&f, 0, sizeof(f));
    f.rax = SYS_CLOSE;
    f.rdi = (u64)fd;
    if (syscall_dispatch(&f) != 0)
        goto fail_back;

    memset(&f, 0, sizeof(f));
    f.rax = SYS_OPEN;
    f.rdi = va;                 /* still "/tmp/open-test" */
    f.rsi = NSH_O_RDONLY;
    fd = syscall_dispatch(&f);
    if (fd < 0)
        goto fail_back;
    memset(&f, 0, sizeof(f));
    f.rax = SYS_READ;
    f.rdi = (u64)fd;
    f.rsi = va_data;
    f.rdx = 32;
    n = syscall_dispatch(&f);
    if (n != 10)
        goto fail_back;
    {
        char out[16];
        memset(out, 0, sizeof(out));
        if (copy_from_user(out, (const void *)va_data, 10) != 0)
            goto fail_back;
        if (memcmp(out, "hello-open", 10) != 0)
            goto fail_back;
    }
    memset(&f, 0, sizeof(f));
    f.rax = SYS_CLOSE;
    f.rdi = (u64)fd;
    if (syscall_dispatch(&f) != 0)
        goto fail_back;

    /* Error cases: missing file without O_CREATE, bogus flags. */
    memcpy(up, "/no/such/file", 14);
    memset(&f, 0, sizeof(f));
    f.rax = SYS_OPEN;
    f.rdi = va;
    f.rsi = NSH_O_RDONLY;
    if (syscall_dispatch(&f) != SYSCALL_RET_ERROR)
        goto fail_back;
    memcpy(up, "/etc/version", 13);
    memset(&f, 0, sizeof(f));
    f.rax = SYS_OPEN;
    f.rdi = va;
    f.rsi = NSH_O_RDWR | NSH_O_WRONLY;     /* 2|1: accmode 3, reserved */
    if (syscall_dispatch(&f) != SYSCALL_RET_ERROR)
        goto fail_back;

    if (vmm_switch_to(vmm_kernel_space()) != 0)
        goto fail;
    process_current_set(NULL);
    process_free(p);
    return 0;

fail_back:
    vmm_switch_to(vmm_kernel_space());
fail:
    process_current_set(NULL);
    process_free(p);
    return -1;
}

/* Process lifetime without real CPU handoff (no timer context yet whose
 * preemption could run the child): create -> schedule -> zombie -> reclaim. */static int ktest2_proc_life(void)
{
    struct process *c =
        process_create_from_vfs("/bin/hello", "lifetest");
    if (!c)
        return -1;

    pid_t pid = c->pid;
    c->ppid = 999;
    sched_add_process(c);

    if (c->state != PROCESS_STATE_RUNNING)
        return -1;

    c->exit_code = 42;
    c->state = PROCESS_STATE_EXITED;
    sched_remove_runnable(c);
    if (sched_zombify(c) != 0)
        return -1;

    struct process *z = sched_zombie_find(pid);
    if (!z || z->exit_code != 42)
        return -1;

    sched_zombie_reclaim(z);
    return 0;
}

static int ktest2_ata(void)
{
    block_dev_t *d = ata_get_dev();
    if (!d) {
        printk("    ata-disk0: no IDE disk attached, skipped\n");
        return 0;
    }

    u8 orig[512], pattern[512], back[512];
    u64 lba = d->sector_count - 1;

    /* remember the sector, clobber it with a known pattern, read the
     * same data back through the layers again, then restore. */
    for (int i = 0; i < 512; i++)
        pattern[i] = (u8)(i ^ 0x5A);
    if (block_dev_read(d, lba, 1, orig) != 0)
        return -1;
    if (block_dev_write(d, lba, 1, pattern) != 0)
        return -1;
    if (block_dev_read(d, lba, 1, back) != 0)
        return -1;
    if (memcmp(back, pattern, sizeof(back)) != 0)
        return -1;
    if (block_dev_write(d, lba, 1, orig) != 0)
        return -1;

    printk("    ata-disk0: r/w verified on lba %llu of %s (%llu sectors)\n",
           lba, d->name, d->sector_count);
    return 0;
}

static int ktest2_fat32(void)
{
    if (!vfs_lookup("/fat")) {
        printk("    fat32-rw: no FAT32 volume mounted, skipped\n");
        return 0;
    }

    /* Create, write, close, reopen, verify in place. */
    struct vfs_file *f = vfs_open("/fat/NEWOS.TST",
                                  VFS_O_CREATE | VFS_O_WRITE);
    if (!f)
        return -1;
    const char *msg = "fat32 read-write: NEWOS was here\n";
    if (vfs_write(f, msg, strlen(msg)) != (int)strlen(msg))
        goto fail_file;
    if (vfs_close(f) != 0)
        return -1;

    char back[64];
    f = vfs_open("/fat/NEWOS.TST", VFS_O_READ);
    if (!f)
        return -1;
    int rn = vfs_read(f, back, sizeof(back));
    if (rn != (int)strlen(msg) || memcmp(back, msg, rn) != 0)
        goto fail_read;
    if (vfs_close(f) != 0)
        return -1;

    /* Nested directory + file inside, then delete in reverse order. */
    if (vfs_mkdir("/fat/SUBTST") != 0)
        return -1;
    struct vfs_file *g = vfs_open("/fat/SUBTST/INTEGER",
                                  VFS_O_CREATE | VFS_O_WRITE);
    if (!g)
        return -1;
    if (vfs_write(g, "42\n", 3) != 3 || vfs_close(g) != 0)
        return -1;
    if (vfs_unlink("/fat/SUBTST/INTEGER") != 0)
        return -1;
    if (vfs_unlink("/fat/SUBTST") != 0)
        return -1;
    if (vfs_lookup("/fat/SUBTST/INTEGER") || vfs_lookup("/fat/SUBTST"))
        return -1;                 /* both must be gone */

    if (vfs_unlink("/fat/NEWOS.TST") != 0)
        return -1;
    if (vfs_lookup("/fat/NEWOS.TST"))
        return -1;

    printk("    fat32-rw: file r/w + mkdir + unlink verified on /fat\n");
    return 0;

fail_read:
    vfs_close(f);
    return -1;

fail_file:
    vfs_close(f);
    return -1;
}

static int ktest2_iso9660(void)
{
    if (!vfs_lookup("/iso")) {
        printk("    iso9660-ro: no ISO 9660 volume mounted, skipped\n");
        return 0;
    }

    /* Root file round-trip. */
    struct vfs_file *f = vfs_open("/iso/HELLO.TXT", VFS_O_READ);
    if (!f) {
        printk("    iso9660-ro: open /iso/HELLO.TXT failed\n");
        return -1;
    }
    const char *msg = "NEWOS iso9660 rocks\n";
    char back[64];
    int rn = vfs_read(f, back, sizeof(back));
    if (vfs_close(f) != 0)
        return -1;
    if (rn != (int)strlen(msg) || memcmp(back, msg, rn) != 0) {
        printk("    iso9660-ro: HELLO.TXT mismatch (%d bytes)\n", rn);
        return -1;
    }

    /* Multi-block file (larger than one 2048-byte ISO block... and
     * larger than the 512-byte device sectors beneath it). */
    f = vfs_open("/iso/BIG.TXT", VFS_O_READ);
    if (!f) {
        printk("    iso9660-ro: open /iso/BIG.TXT failed\n");
        return -1;
    }
    u32 size = f->inode->size;
    if (size < 4096) {
        printk("    iso9660-ro: BIG.TXT size %u too small\n", size);
        vfs_close(f);
        return -1;
    }
    u8 *body = kmalloc(size);
    if (!body) {
        vfs_close(f);
        return -1;
    }
    int total = vfs_read(f, body, size);
    if (vfs_close(f) != 0)
        return -1;
    int ok = total == (int)size;
    for (int i = 0; ok && i < total; i++)
        if (body[i] != (u8)('0' + (i % 10)))
            ok = 0;
    kfree(body);
    if (!ok) {
        printk("    iso9660-ro: BIG.TXT read %d/%u bytes, pattern %s\n",
               total, size, ok ? "ok" : "broken");
        return -1;
    }

    /* Nested directory. */
    f = vfs_open("/iso/DOCS/INNER.TXT", VFS_O_READ);
    if (!f) {
        printk("    iso9660-ro: open /iso/DOCS/INNER.TXT failed\n");
        return -1;
    }
    const char *inner = "inner\n";
    rn = vfs_read(f, back, sizeof(back));
    if (vfs_close(f) != 0)
        return -1;
    if (rn != (int)strlen(inner) || memcmp(back, inner, rn) != 0) {
        printk("    iso9660-ro: INNER.TXT mismatch (%d bytes)\n", rn);
        return -1;
    }

    /* Read-only guards: mkdir, create and unlink must all refuse. */
    if (vfs_mkdir("/iso/NAH") == 0) {
        printk("    iso9660-ro: mkdir was accepted (must refuse)\n");
        return -1;
    }
    if (vfs_create("/iso/NAH.TXT") == 0) {
        printk("    iso9660-ro: create was accepted (must refuse)\n");
        return -1;
    }
    if (vfs_unlink("/iso/HELLO.TXT") == 0) {
        printk("    iso9660-ro: unlink was accepted (must refuse)\n");
        return -1;
    }

    printk("    iso9660-ro: files + nested dir verified on /iso\n");
    return 0;
}

static int ktest2_romfs(void)
{
    if (!vfs_lookup("/rom")) {
        printk("    romfs-ro: no romfs volume mounted, skipped\n");
        return 0;
    }

    /* Root file round-trip. */
    struct vfs_file *f = vfs_open("/rom/HELLO.TXT", VFS_O_READ);
    if (!f) {
        printk("    romfs-ro: open /rom/HELLO.TXT failed\n");
        return -1;
    }
    const char *msg = "NEWOS romfs rocks\n";
    char back[64];
    int rn = vfs_read(f, back, sizeof(back));
    if (vfs_close(f) != 0) {
        printk("    romfs-ro: close HELLO.TXT failed\n");
        return -1;
    }
    if (rn != (int)strlen(msg) || memcmp(back, msg, rn) != 0) {
        printk("    romfs-ro: HELLO.TXT content mismatch (%d bytes)\n", rn);
        return -1;
    }

    /* Hard-linked entry shares data with its target. */
    f = vfs_open("/rom/LINK.TXT", VFS_O_READ);
    if (!f) {
        printk("    romfs-ro: open /rom/LINK.TXT failed\n");
        return -1;
    }
    rn = vfs_read(f, back, sizeof(back));
    if (vfs_close(f) != 0)
        return -1;
    if (rn != (int)strlen(msg) || memcmp(back, msg, rn) != 0) {
        printk("    romfs-ro: LINK.TXT content mismatch (%d bytes)\n", rn);
        return -1;
    }

    /* Multi-block file: the read path splits over the chunk cap. */
    f = vfs_open("/rom/BIG.TXT", VFS_O_READ);
    if (!f) {
        printk("    romfs-ro: open /rom/BIG.TXT failed\n");
        return -1;
    }
    u32 size = f->inode->size;
    if (size < 4096) {
        printk("    romfs-ro: BIG.TXT size %u too small\n", size);
        vfs_close(f);
        return -1;
    }
    u8 *body = kmalloc(size);
    if (!body) {
        vfs_close(f);
        return -1;
    }
    int total = 0;
    while (total < (int)size) {
        int n = vfs_read(f, body + total, size - total);
        if (n <= 0)
            break;
        total += n;
    }
    if (vfs_close(f) != 0)
        return -1;
    int ok = total == (int)size;
    for (int i = 0; ok && i < total; i++)
        if (body[i] != (u8)('0' + (i % 10)))
            ok = 0;
    kfree(body);
    if (!ok) {
        printk("    romfs-ro: BIG.TXT read %d/%u bytes, pattern %s\n",
               total, size, ok ? "ok" : "broken");
        return -1;
    }

    /* Nested directory + read-only guards. */
    f = vfs_open("/rom/DOCS/INNER.TXT", VFS_O_READ);
    if (!f) {
        printk("    romfs-ro: open /rom/DOCS/INNER.TXT failed\n");
        return -1;
    }
    const char *inner = "inner\n";
    rn = vfs_read(f, back, sizeof(back));
    if (vfs_close(f) != 0)
        return -1;
    if (rn != (int)strlen(inner) || memcmp(back, inner, rn) != 0) {
        printk("    romfs-ro: INNER.TXT mismatch (%d bytes)\n", rn);
        return -1;
    }
    if (vfs_mkdir("/rom/NAH") == 0) {
        printk("    romfs-ro: mkdir was accepted (must refuse)\n");
        return -1;
    }
    if (vfs_create("/rom/NAH.TXT") == 0) {
        printk("    romfs-ro: create was accepted (must refuse)\n");
        return -1;
    }
    if (vfs_unlink("/rom/HELLO.TXT") == 0) {
        printk("    romfs-ro: unlink was accepted (must refuse)\n");
        return -1;
    }

    printk("    romfs-ro: files + hardlink + nested dir verified on /rom\n");
    return 0;
}

static int ktest2_net_ping(void)
{
    if (!net_up()) {
        printk("    net-ping: no NIC attached, skipped\n");
        return 0;
    }

    static const char payload[] = "NEWOS net-ping";
    u8 reply[64];
    u32 rlen = 0;

    if (net_icmp_ping(net_ip_gw(), payload, sizeof(payload) - 1,
                      reply, &rlen, 3000000) != 0)
        return -1;                 /* gateway must echo us */
    if (rlen != sizeof(payload) - 1 || memcmp(reply, payload, rlen) != 0)
        return -1;                 /* payload must come back intact */

    printk("    net-ping: gw echoed %u bytes back\n", rlen);
    return 0;
}

/* A4 sine, 0.25 s, generated integer-only: the quarter-wave table holds
 * quarter-wave table holds A*sin(p*pi/128) for p=0..64, and the frame
 * loop walks a 32-bit phase accumulator at 440 Hz / 48 kHz. */
static const s16 hda_quarter[65] = {
    0, 491, 981, 1471, 1960, 2448, 2935, 3419, 3902, 4382, 4860,
    5334, 5806, 6274, 6738, 7198, 7654, 8105, 8551, 8992, 9428,
    9858, 10282, 10700, 11111, 11516, 11914, 12305, 12688, 13063,
    13431, 13791, 14142, 14485, 14819, 15144, 15460, 15767, 16064,
    16352, 16629, 16897, 17155, 17402, 17638, 17864, 18080, 18284,
    18478, 18660, 18831, 18991, 19139, 19276, 19401, 19514, 19616,
    19706, 19784, 19850, 19904, 19946, 19976, 19994, 20000
};

static s16 hda_sine_sample(u32 phase)
{
    u32 idx = (phase >> 24) & 0xffu;
    u32 pos = idx & 0x3fu;
    s16 base = (idx >> 6) & 1u ? hda_quarter[64u - pos]
                               : hda_quarter[pos];
    return (idx >> 6) & 2u ? (s16)-base : base;
}

static int ktest2_hda(void)
{
    if (!hda_present()) {
        printk("    hda: no HD-Audio codec attached, skipped\n");
        return 0;
    }

    /* 0.25 s at 48 kHz stereo 16-bit = 12000 frames = 48000 bytes in
     * one contiguous physical block; the frames alternate between a
     * mild chirp and a simpler vibrato pattern to exercise the ring. */
    u64 frame;
    if (phys_alloc_block(4, &frame)) {
        printk("    hda: no 64K physical block for PCM\n");
        return -1;
    }
    u64 pcm_phys = frame << 12;
    s16 *pcm = (s16 *)phys_to_virt(pcm_phys);
    u32 step = (u32)(440ull * 0x100000000ull / 48000ull);
    u32 phase = 0;
    for (u32 i = 0; i < 12000u; i++) {
        s16 v = hda_sine_sample(phase);
        pcm[2u * i] = v;
        pcm[2u * i + 1u] = v;
        phase += step;
    }

    long played = hda_play(pcm_phys, 48000, 48000, 2, 16);
    if (played != 48000) {
        printk("    hda: playback returned %ld (expected 48000)\n", played);
        return -1;
    }

    printk("    hda: A4 sine, 0.25 s @ 48 kHz delivered through SD4\n");
    return 0;
}

static int ktest2_net_e1000(void)
{
    if (net_start_nic("e1000") != 0) {
        printk("    net-e1000: no e1000 NIC attached, skipped\n");
        return 0;
    }

    static const char payload[] = "NEWOS e1000 ping";
    u8 reply[64];
    u32 rlen = 0;
    int rc = 0;

    if (net_icmp_ping(net_ip_gw(), payload, sizeof(payload) - 1,
                      reply, &rlen, 3000000) != 0)
        rc = -1;                   /* gateway must echo us via e1000 */
    else if (rlen != sizeof(payload) - 1 ||
             memcmp(reply, payload, rlen) != 0)
        rc = -1;                   /* payload must come back intact */
    else
        printk("    net-e1000: gw echoed %u bytes back\n", rlen);

    /* Restore the boot NIC so later stages see the usual stack. */
    net_start_nic(NULL);
    return rc;
}

struct ktest2 {
    const char *name;
    int (*fn)(void);
};

static const struct ktest2 ktests2[] = {
    { "pmm-phys",    ktest2_phys        },
    { "address-spaces", ktest2_address_space },
    { "usercopy",    ktest2_usercopy    },
    { "elf-loader",  ktest2_elf         },
    { "syscalls",    ktest2_syscall     },
    { "open-close",  ktest2_open_close  },
    { "timer-pit",   ktest2_timer       },
    { "proc-life",   ktest2_proc_life   },
    { "fpu-state",   ktest2_fpu         },
    { "ata-disk0",   ktest2_ata         },
    { "fat32-rw",    ktest2_fat32       },
    { "iso9660-ro",  ktest2_iso9660     },
    { "romfs-ro",    ktest2_romfs       },
    { "net-ping",    ktest2_net_ping    },
    { "net-e1000",   ktest2_net_e1000   },
    { "hda",         ktest2_hda         },
};

int run_phase2_tests(void)
{
    int failures = 0;

    printk("=== NEWOS PHASE 2 TESTS ===\n");
    for (size_t i = 0; i < sizeof(ktests2) / sizeof(ktests2[0]); i++) {
        int rc = ktests2[i].fn();
        printk("  [%s] %s%s\n", rc == 0 ? "PASS" : "FAIL",
               ktests2[i].name, rc ? "  <-- failure" : "");
        if (rc)
            failures++;
    }
    printk("=== %zu tests, %d failures ===\n",
           sizeof(ktests2) / sizeof(ktests2[0]), failures);
    return failures;
}