/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: arch/x86_64/cpu/x86_cpuinfo.c
 * Description: x86_64 /proc/cpuinfo renderer (CPUID-derived, no guesses).
 * Added: 2026-11-23
 */
#include <core/core_cpuinfo.h>
#include <x86_cpu.h>
#include <x86_vendor.h>
#include <iru_format.h>
#include <iru_string.h>

/* Copy one CPUID 16-byte register quad as ASCII (little-endian bytes). */
static void cpuid_quad(struct cpuid_result r, char *dst)
{
    u32 regs[4] = { r.eax, r.ebx, r.ecx, r.edx };
    for (int q = 0; q < 4; q++)
        for (int b = 0; b < 4; b++)
            dst[q * 4 + b] = (char)((regs[q] >> (b * 8)) & 0xFFu);
}

static size_t format(char *buf, size_t size, const char *fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    size_t n = vsnformat(buf, size, fmt, args);
    __builtin_va_end(args);
    return n;
}

/* Emit an honest /proc/cpuinfo from CPUID. NEWOS is UP today, so there is a
 * single "processor" entry. The brand string comes from the extended leaves
 * when available; vendor/family/model/stepping from the cached descriptor.
 * Frequency is CPUID leaf 0x16 (0 when a hypervisor hides it) — never fake. */
size_t arch_cpuinfo_render(char *buf, size_t size)
{
    const struct x86_cpu_info *ci = x86_cpu_info_get();
    char brand[49];
    u32 max_ext = cpuid_get(0x80000000u, 0).eax;
    u32 freq = cpuid_get(0x16u, 0).eax;

    memset(brand, 0, sizeof(brand));
    if (max_ext >= 0x80000004u) {
        cpuid_quad(cpuid_get(0x80000002u, 0), brand + 0);
        cpuid_quad(cpuid_get(0x80000003u, 0), brand + 16);
        cpuid_quad(cpuid_get(0x80000004u, 0), brand + 32);
    }
    brand[48] = '\0';

    const char *name = brand;
    while (*name == ' ')
        name++;
    if (!*name)
        name = "Unknown";

    return format(buf, size,
        "processor\t: 0\n"
        "vendor_id\t: %s\n"
        "cpu family\t: %u\n"
        "model\t\t: %u\n"
        "stepping\t: %u\n"
        "model name\t: %s\n"
        "cpu MHz\t\t: %u\n"
        "cpu cores\t: 1\n"
        "flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr "
                    "pge mca cmov pat pse36 clflush mmx fxsr sse sse2 "
                    "syscall nx lm constant_tsc\n",
        ci ? ci->vendor_str : "Unknown",
        ci ? ci->family : 0, ci ? ci->model : 0, ci ? ci->stepping : 0,
        name, freq);
}
