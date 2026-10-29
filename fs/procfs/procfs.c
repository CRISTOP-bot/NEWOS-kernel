/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/fs/procfs/procfs.c
 * Description: procfs: the read-only kernel information filesystem mounted at /proc.
 * Added: 2026-10-17
 */
#include <fs/vfs.h>
#include <fs/procfs/procfs.h>
#include <core/core_bootinfo.h>
#include <core/core_printk.h>
#include <core/core_time.h>
#include <core/core_version.h>
#include <mm/mm_pmm.h>
#include <iru_format.h>
#include <iru_string.h>

/* procfs: the read-only kernel information filesystem mounted at /proc.
 *
 * The current implementation exposes the small, honest core of a live
 * /proc: identity, uptime, memory, mounts, filesystems and the boot
 * command line. Process listing still lives behind SYS_PS because the
 * process and filesystem layers are deliberately independent; adding a
 * process provider is future work (see ROADMAP.md, v0.4). */

enum procfs_kind {
    PROCFS_VERSION,
    PROCFS_CMDLINE,
    PROCFS_UPTIME,
    PROCFS_MEMINFO,
    PROCFS_FILESYSTEMS,
    PROCFS_MOUNTS,
};

struct procfs_file {
    const char *name;
    enum procfs_kind kind;
};

static const struct procfs_file procfs_files[] = {
    { "version",     PROCFS_VERSION     },
    { "cmdline",     PROCFS_CMDLINE     },
    { "uptime",      PROCFS_UPTIME      },
    { "meminfo",     PROCFS_MEMINFO     },
    { "filesystems", PROCFS_FILESYSTEMS },
    { "mounts",      PROCFS_MOUNTS      },
};

static const char procfs_filesystems[] =
    "nodev\ttmpfs\n"
    "nodev\tdevfs\n"
    "nodev\tproc\n"
    "nodev\tsysfs\n";

/* The synthetic root is a RAM-backed tree; these entries describe the
 * current fixed mount set. They are intentionally read-only. */
static const char procfs_mounts[] =
    "tmpfs / tmpfs rw 0 0\n"
    "devfs /dev devfs rw 0 0\n"
    "proc /proc proc ro 0 0\n"
    "sysfs /sys sysfs ro 0 0\n";

static size_t procfs_format(char *buf, size_t size, const char *fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    size_t n = vsnformat(buf, size, fmt, args);
    __builtin_va_end(args);
    if (n >= size)
        n = size ? size - 1 : 0;
    return n;
}

static size_t procfs_copy(char *buf, size_t size, const char *text)
{
    size_t n = strlen(text);
    if (n >= size)
        n = size ? size - 1 : 0;
    if (n)
        memcpy(buf, text, n);
    if (size)
        buf[n] = '\0';
    return n;
}

static size_t procfs_render(enum procfs_kind kind, char *buf, size_t size)
{
    switch (kind) {
    case PROCFS_VERSION:
        return procfs_format(buf, size, "NEWOS %s (x86_64)\n",
                             NEWOS_VERSION);

    case PROCFS_CMDLINE:
        return procfs_format(buf, size, "%s\n",
                             g_boot_cmdline ? g_boot_cmdline : "");

    case PROCFS_UPTIME: {
        u64 hz = core_time_hz();
        u64 ticks = core_time_ticks();
        u64 sec = hz ? ticks / hz : 0;
        u64 centi = hz ? ((ticks % hz) * 100) / hz : 0;
        return procfs_format(buf, size, "%llu.%02llu\n", sec, centi);
    }

    case PROCFS_MEMINFO: {
        u64 total_kb = pmm_total_frames() * 4;
        u64 free_kb = pmm_free_frames() * 4;
        u64 used_kb = total_kb >= free_kb ? total_kb - free_kb : 0;
        return procfs_format(
            buf, size,
            "MemTotal:       %llu kB\n"
            "MemFree:        %llu kB\n"
            "MemAvailable:   %llu kB\n"
            "MemUsed:        %llu kB\n"
            "Buffers:        0 kB\n"
            "Cached:         0 kB\n"
            "SwapTotal:      0 kB\n"
            "SwapFree:       0 kB\n",
            total_kb, free_kb, free_kb, used_kb);
    }

    case PROCFS_FILESYSTEMS:
        return procfs_copy(buf, size, procfs_filesystems);

    case PROCFS_MOUNTS:
        return procfs_copy(buf, size, procfs_mounts);
    }

    return (size_t)-1;
}

static int procfs_read(struct vfs_file *f, void *buf, size_t len, size_t *got)
{
    *got = 0;

    if (!f || !f->inode || !f->inode->private)
        return -1;

    const struct procfs_file *file = f->inode->private;
    char content[512];
    size_t n = procfs_render(file->kind, content, sizeof(content));
    if (n == (size_t)-1)
        return -1;

    if (f->offset >= n)
        return 0;

    size_t avail = n - f->offset;
    if (avail > len)
        avail = len;
    memcpy(buf, content + f->offset, avail);
    *got = avail;
    return 0;
}

static struct inode_ops procfs_file_ops = {
    .read = procfs_read,
};

static struct inode_ops procfs_root_ops = {
    /* The VFS core lists children directly; no custom readdir is needed. */
};

static struct vfs_inode *g_proc_root;

struct vfs_inode *procfs_mount(struct vfs_inode *parent)
{
    if (g_proc_root)
        return g_proc_root;

    /* initramfs already created /proc; replace its placeholder ops so the
     * path is a real procfs mount instead of an empty tmpfs directory. */
    struct vfs_inode *root = vfs_lookup("/proc");
    if (!root) {
        if (!parent)
            return NULL;
        root = vfs_alloc_inode(parent, "proc", VFS_MODE_DIR | 0555,
                               &procfs_root_ops, NULL);
    } else {
        root->ops = &procfs_root_ops;
        root->private = NULL;
        root->mode = VFS_MODE_DIR | 0555;
    }
    if (!root)
        return NULL;

    for (size_t i = 0; i < sizeof(procfs_files) / sizeof(procfs_files[0]);
         i++) {
        struct vfs_inode *in =
            vfs_alloc_inode(root, procfs_files[i].name,
                            VFS_MODE_REG | 0444, &procfs_file_ops,
                            (void *)&procfs_files[i]);
        if (!in) {
            pr_warn("procfs: failed to create /proc/%s\n",
                    procfs_files[i].name);
            return NULL;
        }
    }

    g_proc_root = root;
    pr_info("procfs: mounted /proc (%zu files)\n",
            sizeof(procfs_files) / sizeof(procfs_files[0]));
    return root;
}

void procfs_init(void)
{
    procfs_mount(vfs_root());
}
