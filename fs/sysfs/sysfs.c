#include <fs/vfs.h>
#include <fs/sysfs/sysfs.h>
#include <core/core_printk.h>
#include <core/core_version.h>
#include <drivers/drv_core.h>
#include <drivers/pci.h>
#include <mm/mm_heap.h>
#include <iru_format.h>
#include <iru_string.h>

/* sysfs: the read-only kernel/device tree mounted at /sys.
 *
 * This is intentionally a narrow, useful slice rather than a Linux sysfs
 * clone: /sys/kernel exposes release identity, and /sys/bus/pci/devices
 * lists every PCI function discovered by the device model with its vendor,
 * device and class IDs. Writable attributes and driver bind/unbind are
 * future work; the VFS refuses unlink/rename below synthetic trees. */

enum sysfs_kind {
    SYSFS_OSTYPE,
    SYSFS_OSRELEASE,
    SYSFS_VERSION,
    SYSFS_PCI_VENDOR,
    SYSFS_PCI_DEVICE,
    SYSFS_PCI_CLASS,
};

struct sysfs_attr {
    enum sysfs_kind kind;
    struct device *dev;
};

struct sysfs_pci_ctx {
    struct vfs_inode *devices;
};

static size_t sysfs_format(char *buf, size_t size, const char *fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    size_t n = vsnformat(buf, size, fmt, args);
    __builtin_va_end(args);
    if (n >= size)
        n = size ? size - 1 : 0;
    return n;
}

static size_t sysfs_render(const struct sysfs_attr *attr, char *buf,
                           size_t size)
{
    switch (attr->kind) {
    case SYSFS_OSTYPE:
        return sysfs_format(buf, size, "NEWOS\n");
    case SYSFS_OSRELEASE:
        return sysfs_format(buf, size, "%s\n", NEWOS_VERSION);
    case SYSFS_VERSION:
        return sysfs_format(buf, size, "NEWOS %s (x86_64)\n",
                            NEWOS_VERSION);
    case SYSFS_PCI_VENDOR:
        return sysfs_format(buf, size, "0x%04x\n",
                            (u64)attr->dev->vendor);
    case SYSFS_PCI_DEVICE:
        return sysfs_format(buf, size, "0x%04x\n",
                            (u64)attr->dev->device);
    case SYSFS_PCI_CLASS:
        return sysfs_format(buf, size, "0x%06x\n",
                            (u64)attr->dev->class);
    }
    return (size_t)-1;
}

static int sysfs_read(struct vfs_file *f, void *buf, size_t len, size_t *got)
{
    *got = 0;

    if (!f || !f->inode || !f->inode->private)
        return -1;

    const struct sysfs_attr *attr = f->inode->private;
    if (attr->dev == NULL && attr->kind >= SYSFS_PCI_VENDOR)
        return -1;

    char content[128];
    size_t n = sysfs_render(attr, content, sizeof(content));
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

static void sysfs_destroy(struct vfs_inode *inode)
{
    if (!inode)
        return;
    kfree(inode->private);
    inode->private = NULL;
}

static struct inode_ops sysfs_file_ops = {
    .read    = sysfs_read,
    .destroy = sysfs_destroy,
};

static struct inode_ops sysfs_dir_ops;
static struct inode_ops sysfs_root_ops;

static struct vfs_inode *sysfs_add_dir(struct vfs_inode *parent,
                                       const char *name)
{
    return vfs_alloc_inode(parent, name, VFS_MODE_DIR | 0555,
                           &sysfs_dir_ops, NULL);
}

static struct vfs_inode *sysfs_add_attr(struct vfs_inode *dir,
                                        const char *name,
                                        enum sysfs_kind kind,
                                        struct device *dev)
{
    struct sysfs_attr *attr = kmalloc(sizeof(*attr));
    if (!attr)
        return NULL;
    attr->kind = kind;
    attr->dev = dev;

    struct vfs_inode *in =
        vfs_alloc_inode(dir, name, VFS_MODE_REG | 0444,
                        &sysfs_file_ops, attr);
    if (!in) {
        kfree(attr);
        return NULL;
    }
    return in;
}

static void sysfs_add_pci(struct device *dev, void *arg)
{
    struct sysfs_pci_ctx *ctx = (struct sysfs_pci_ctx *)arg;
    if (!ctx || !ctx->devices || !dev || !dev->bus || !dev->bus->name)
        return;
    if (strcmp(dev->bus->name, "pci") != 0)
        return;

    const struct pci_dev_info *info =
        (const struct pci_dev_info *)dev->private;
    if (!info)
        return;

    char name[32];
    size_t n = sysfs_format(name, sizeof(name), "%04x:%02x:%02x.%x",
                            (u64)0, (u64)info->bus, (u64)info->dev,
                            (u64)info->func);
    if (n == 0)
        return;

    struct vfs_inode *dir = sysfs_add_dir(ctx->devices, name);
    if (!dir)
        return;
    sysfs_add_attr(dir, "vendor", SYSFS_PCI_VENDOR, dev);
    sysfs_add_attr(dir, "device", SYSFS_PCI_DEVICE, dev);
    sysfs_add_attr(dir, "class",  SYSFS_PCI_CLASS,  dev);
}

static struct vfs_inode *g_sys_root;

struct vfs_inode *sysfs_mount(struct vfs_inode *parent)
{
    if (g_sys_root)
        return g_sys_root;

    struct vfs_inode *root = vfs_lookup("/sys");
    if (!root) {
        if (!parent)
            return NULL;
        root = vfs_alloc_inode(parent, "sys", VFS_MODE_DIR | 0555,
                               &sysfs_root_ops, NULL);
    } else {
        root->ops = &sysfs_root_ops;
        root->private = NULL;
        root->mode = VFS_MODE_DIR | 0555;
    }
    if (!root)
        return NULL;

    struct vfs_inode *kernel_dir = sysfs_add_dir(root, "kernel");
    if (kernel_dir) {
        sysfs_add_attr(kernel_dir, "ostype",   SYSFS_OSTYPE,   NULL);
        sysfs_add_attr(kernel_dir, "osrelease", SYSFS_OSRELEASE, NULL);
        sysfs_add_attr(kernel_dir, "version",  SYSFS_VERSION,  NULL);
    }

    struct vfs_inode *bus = sysfs_add_dir(root, "bus");
    struct vfs_inode *pci = bus ? sysfs_add_dir(bus, "pci") : NULL;
    struct vfs_inode *devices = pci ? sysfs_add_dir(pci, "devices") : NULL;
    if (devices) {
        struct sysfs_pci_ctx ctx = { .devices = devices };
        device_model_for_each(sysfs_add_pci, &ctx);
    }

    g_sys_root = root;
    pr_info("sysfs: mounted /sys (kernel identity, PCI inventory)\n");
    return root;
}

void sysfs_init(void)
{
    sysfs_mount(vfs_root());
}
