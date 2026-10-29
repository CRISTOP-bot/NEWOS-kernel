/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/fs/romfs/romfs.c
 * Description: romfs v1 read-only filesystem over block_dev_t. On-disk layout: a
 * Added: 2026-10-22
 */
#include <fs/romfs.h>
#include <fs/vfs.h>
#include <drivers/ata.h>
#include <mm/mm_heap.h>
#include <core/core_printk.h>
#include <iru_string.h>

/* romfs v1 read-only filesystem over block_dev_t. On-disk layout: a
 * "-rom1fs-" superblock (magic, image size, additive checksum, volume
 * name), then 16-byte-aligned header+name inodes. Every pointer is an
 * absolute byte offset into the image; the low nibble of the "next" word
 * carries the entry type plus an exec bit, and every dereference masks
 * those bits off. Format reference: the romfs 1.0 definition as written
 * by its authors and the Linux implementation (fs/romfs): additive 32-bit
 * checksum over min(size, 512) bytes that must sum to zero, directory
 * child chains hanging off the header's spec field and file data at
 * header_pos + padded-name length. */

#define ROMFS_MAXFN        128u            /* per-entry name cap */
#define ROMFS_MAXOFF       (64u * 1024 * 1024)
#define ROMFS_MAX_VALUE    (64u * 1024)    /* one file read chunk cap */
#define ROMFS_WALK_DEPTH   16              /* directory recursion cap */
#define ROMFS_DIR_ENTRIES  4096            /* chain cycle/safety cap */

#define ROMFH_TYPE_MASK    0x00000007u
#define ROMFH_HRD          0u
#define ROMFH_DIR          1u
#define ROMFH_REG          2u
#define ROMFH_EXEC         0x00000008u
#define ROMFH_OFF_MASK     0xFFFFFFF0u

#define ROMFS_META_HDR     16u

struct romfs_fs {
    block_dev_t *dev;
    u32 maxoff;                /* image size (from the superblock) */
    u32 namelen;               /* volume name length */
};

struct romfs_node {
    struct romfs_fs *fs;
    u64 data_off;              /* byte offset of file data in the image */
    u32 size;                  /* file size */
};

/* --- image access -------------------------------------------------------- */

/* Read an arbitrary byte span through a sector-aligned bounce. romfs
 * offsets are 16-byte aligned and the dev quantum is 512 or 2048 bytes,
 * so spans may be smaller than one sector on either side: round out to
 * full sectors and copy from inside the bounce. */
static int romfs_read_bytes(struct romfs_fs *fs, u64 off, u32 len, u8 *buf)
{
    if (!fs || !fs->dev || !fs->dev->read_sectors)
        return -1;
    if (len == 0 || off + len > fs->maxoff)
        return -1;

    u64 s0 = off / fs->dev->sector_size;
    u64 s1 = (off + len - 1) / fs->dev->sector_size;
    u32 span = (u32)(s1 - s0 + 1);

    u8 *bounce = kmalloc(span * fs->dev->sector_size);
    if (!bounce)
        return -1;
    int rc = fs->dev->read_sectors(fs->dev, s0, span, bounce);
    if (rc == 0)
        memcpy(buf, bounce + (off - s0 * fs->dev->sector_size), len);
    kfree(bounce);
    return rc;
}

/* Additive checksum over a byte range as BE32 words (short tails zero
 * padded). On-image values are stored as the two's complement so the
 * total sums to zero. */
static u32 romfs_csum(struct romfs_fs *fs, u64 off, u32 len)
{
    u32 sum = 0;

    while (len > 0) {
        u8 cell[512];
        u32 chunk = len > 512 ? 512 : len;
        u32 words = chunk / 4;          /* whole words only */
        if (romfs_read_bytes(fs, off, words * 4, cell) != 0)
            return 0xFFFFFFFFu;         /* error: impossible sum */
        for (u32 p = 0; p < words * 4; p += 4) {
            u32 w = ((u32)cell[p] << 24) | ((u32)cell[p + 1] << 16) |
                    ((u32)cell[p + 2] << 8) | (u32)cell[p + 3];
            sum += w;
        }
        /* A short tail (only possible at the image end) zero pads: the
         * bytes exist reads len round down to whole words. */
        off += words * 4;
        len -= words * 4;
        if (len > 0 && len < 4) {
            if (romfs_read_bytes(fs, off, len, cell) != 0)
                return 0xFFFFFFFFu;
            memset(cell + len, 0, 4 - len);
            u32 w = ((u32)cell[0] << 24) | ((u32)cell[1] << 16) |
                    ((u32)cell[2] << 8) | (u32)cell[3];
            sum += w;
            break;
        }
    }
    return sum;
}

struct romfs_hdr {
    u32 next;                  /* raw word: addr in high bits + type nibble*/
    u32 spec;                  /* type-specific value */
    u32 size;
    u32 type;                  /* next & 7 */
    u32 exec;                  /* next & 8 */
    u32 addr;                  /* next & ~15 */
    u32 spec_addr;             /* spec & ~15 */
    u16 name_len;
    u32 metasize;              /* 16 + name + NUL, padded to 16 */
};

/* Load the 16-byte header plus the name tail it owns (name scan stops at
 * the first NUL, capped at the format limit). */
static int romfs_read_hdr(struct romfs_fs *fs, u32 pos,
                          struct romfs_hdr *h)
{
    if (pos >= fs->maxoff || fs->maxoff - pos < ROMFS_META_HDR)
        return -1;

    u8 raw[ROMFS_META_HDR];
    if (romfs_read_bytes(fs, pos, sizeof(raw), raw) != 0)
        return -1;

    h->next = ((u32)raw[0] << 24) | ((u32)raw[1] << 16) |
              ((u32)raw[2] << 8) | (u32)raw[3];
    h->spec = ((u32)raw[4] << 24) | ((u32)raw[5] << 16) |
              ((u32)raw[6] << 8) | (u32)raw[7];
    h->size = ((u32)raw[8] << 24) | ((u32)raw[9] << 16) |
              ((u32)raw[10] << 8) | (u32)raw[11];

    h->type = h->next & ROMFH_TYPE_MASK;
    h->exec = (h->next & ROMFH_EXEC) ? 1u : 0u;
    h->addr = h->next & ROMFH_OFF_MASK;
    h->spec_addr = h->spec & ROMFH_OFF_MASK;

    u64 name_pos = (u64)pos + ROMFS_META_HDR;
    u32 nlen = 0;
    while (nlen < ROMFS_MAXFN) {
        if (name_pos + nlen >= fs->maxoff)
            return -1;
        u32 span = fs->maxoff - (u32)(name_pos + nlen);
        if (span > 64)
            span = 64;
        if (span > ROMFS_MAXFN - nlen)
            span = ROMFS_MAXFN - nlen;
        u8 cell[64];
        if (romfs_read_bytes(fs, name_pos + nlen, span, cell) != 0)
            return -1;
        u32 stop = span ? span : 64;
        for (u32 j = 0; j < stop; j++) {
            if (cell[j] == 0) {
                h->name_len = (u16)(nlen + j);
                h->metasize = (ROMFS_META_HDR + h->name_len + 1 + 15)
                              & ~15u;
                return 0;
            }
        }
        nlen += span;
        if (span < 64 && nlen < ROMFS_MAXFN) {
            /* chunk boundary reached without NUL: keep scanning until
             * the format cap or the frame run out */
        }
    }
    return -1;                     /* name never terminates: corrupt */
}

/* --- tree materialization ------------------------------------------------ */

static struct inode_ops romfs_file_ops;
static struct inode_ops romfs_dir_ops;

static int walk_dir(struct romfs_fs *fs, u32 dir_pos,
                    struct vfs_inode *vfsparent, u32 depth);

static int materialize(struct romfs_fs *fs, const char *name, u32 pos,
                       struct romfs_hdr *h, struct vfs_inode *vfsparent,
                       u32 depth);

/* One VFS child from an on-image header (the caller already matched the
 * name). Hard links redirect to their target with the link's name. */
static int materialize(struct romfs_fs *fs, const char *name, u32 pos,
                       struct romfs_hdr *h, struct vfs_inode *vfsparent,
                       u32 depth)
{
    u32 kind = h->type;

    if (kind == ROMFH_HRD) {
        u32 target = h->spec_addr;
        struct romfs_hdr th;
        if (target == 0 || target == pos || target >= fs->maxoff)
            return -1;
        if (romfs_read_hdr(fs, target, &th) != 0)
            return -1;
        /* One redirect level: chains of links Are not followed. */
        if (th.type == ROMFH_HRD)
            return -1;
        return materialize(fs, name, target, &th, vfsparent, depth);
    }

    if (kind == ROMFH_DIR) {
        if (h->name_len > 63)
            return -1;
        struct romfs_node *node = kzalloc(sizeof(*node));
        if (!node)
            return -1;
        node->fs = fs;
        node->data_off = 0;
        node->size = 0;
        struct vfs_inode *in = vfs_alloc_inode(vfsparent, name,
                                               VFS_MODE_DIR | 0755,
                                               &romfs_dir_ops, node);
        if (!in) {
            kfree(node);
            return -1;
        }
        if (walk_dir(fs, pos, in, depth + 1) != 0)
            return -1;
        return 0;                  /* dir children that landed stay */
    }

    if (kind == ROMFH_REG) {
        if (h->name_len > 63)
            return -1;
        struct romfs_node *node = kzalloc(sizeof(*node));
        if (!node)
            return -1;
        node->fs = fs;
        node->data_off = (u64)pos + h->metasize;
        node->size = h->size;
        u32 mode = VFS_MODE_REG | (h->exec ? 0755 : 0644);
        struct vfs_inode *in = vfs_alloc_inode(vfsparent, name, mode,
                                               &romfs_file_ops, node);
        if (!in) {
            kfree(node);
            return -1;
        }
        in->size = node->size;     /* VFS convention: files carry bytes */
        return 0;
    }

    return 1;                      /* SYM/BLK/CHR/SCK/FIF: skip quietly */
}

static int walk_dir(struct romfs_fs *fs, u32 dir_pos,
                    struct vfs_inode *vfsparent, u32 depth)
{
    if (depth > ROMFS_WALK_DEPTH)
        return -1;

    struct romfs_hdr dh;
    if (romfs_read_hdr(fs, dir_pos, &dh) != 0 || dh.spec_addr >= fs->maxoff)
        return -1;

    u32 child = dh.spec_addr;
    u32 steps = 0;
    while (child && child < fs->maxoff) {
        if (++steps > ROMFS_DIR_ENTRIES)
            return -1;             /* broken chain: bail, tree stays */

        struct romfs_hdr h;
        if (romfs_read_hdr(fs, child, &h) != 0)
            return -1;

        char name[64];
        if (h.name_len > 0 && h.name_len < sizeof(name)) {
            if (romfs_read_bytes(fs, child + ROMFS_META_HDR, h.name_len,
                                 (u8 *)name) != 0)
                return -1;
            name[h.name_len] = '\0';
            materialize(fs, name, child, &h, vfsparent, depth);
        }

        child = h.addr;
        if (child == dir_pos)
            break;                 /* looped back: done */
    }
    return 0;
}

/* --- ops ----------------------------------------------------------------- */

static int romfs_file_read(struct vfs_file *f, void *buf, size_t len,
                           size_t *got)
{
    struct romfs_node *n = f->inode->private;

    if (f->offset >= n->size) {
        *got = 0;
        return 0;
    }
    u64 end = n->size;
    if ((u64)f->offset + len < end)
        end = (u64)f->offset + len;

    /* Chunked past the bounce cap: each romfs_read_bytes round-trips a
     * sector-aligned bounce, so reads stay bounded allocations. */
    u8 *dst = buf;
    u64 pos = f->offset;
    size_t done = 0;
    while (pos < end) {
        u32 span = ROMFS_MAX_VALUE;
        if ((u64)span > end - pos)
            span = (u32)(end - pos);
        if (romfs_read_bytes(n->fs, n->data_off + pos, span, dst + done)
            != 0)
            break;
        done += span;
        pos += span;
    }
    *got = done;
    return 0;
}

static void romfs_destroy(struct vfs_inode *in)
{
    if (in && in->private)
        kfree(in->private);
}

static struct inode_ops romfs_file_ops = {
    .read     = romfs_file_read,
    .destroy  = romfs_destroy,
};

static struct inode_ops romfs_dir_ops = {
    .destroy  = romfs_destroy,
};

/* --- mount + init -------------------------------------------------------- */

int romfs_mount_tree(const char *path, block_dev_t *dev)
{
    if (!path || !dev || !dev->read_sectors)
        return -1;

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!*base || strlen(base) >= 64)
        return -1;

    struct romfs_fs *fs = kzalloc(sizeof(*fs));
    if (!fs)
        return -1;
    fs->dev = dev;
    /* The device bounds frame: tightened to the image size below. */
    fs->maxoff = (u32)dev->sector_count * dev->sector_size;

    u8 sb[512];
    if (romfs_read_bytes(fs, 0, 512, sb) != 0) {
        kfree(fs);
        return -1;
    }
    if (sb[0] != '-' || sb[1] != 'r' || sb[2] != 'o' || sb[3] != 'm' ||
        sb[4] != '1' || sb[5] != 'f' || sb[6] != 's' || sb[7] != '-') {
        kfree(fs);
        return -1;                 /* not romfs: quiet (probe disk) */
    }

    u32 size = ((u32)sb[8] << 24) | ((u32)sb[9] << 16) |
               ((u32)sb[10] << 8) | (u32)sb[11];
    if (size < 16 || size > fs->maxoff || size > ROMFS_MAXOFF) {
        kfree(fs);
        return -1;
    }
    fs->maxoff = size;

    u32 cs = romfs_csum(fs, 0, size < 512 ? size : 512);
    if (cs != 0) {
        kfree(fs);
        return -1;
    }

    u32 namelen = 0;
    while (namelen < 128 && sb[12 + namelen] != '\0')
        namelen++;
    fs->namelen = namelen;

    u32 root = (16 + namelen + 1 + 15) & ~15u;
    if (root >= fs->maxoff) {
        kfree(fs);
        return -1;
    }
    struct romfs_hdr dh;
    if (romfs_read_hdr(fs, root, &dh) != 0 || dh.type != ROMFH_DIR) {
        kfree(fs);
        return -1;
    }

    if (vfs_lookup(path)) {
        kfree(fs);
        return -1;                 /* already mounted */
    }

    /* Order matters: the root node materializes with its private data
     * already in place, because vfs_alloc_inode links instantly and a
     * later failure must NOT kfree an inode sitting in the tree. */
    struct romfs_node *rn = kzalloc(sizeof(*rn));
    if (!rn) {
        kfree(fs);
        return -1;
    }
    rn->fs = fs;
    rn->data_off = 0;
    rn->size = 0;

    struct vfs_inode *in = vfs_alloc_inode(vfs_root(), base,
                                           VFS_MODE_DIR | 0755,
                                           &romfs_dir_ops, rn);
    if (!in) {
        kfree(rn);
        kfree(fs);
        return -1;
    }

    if (walk_dir(fs, root, in, 1) != 0)
        printk("romfs: warning: %s tree incomplete, partial mount\n",
               path);

    printk("romfs: %s on %s: %u bytes, %u entries\n",
           path, dev->name, fs->maxoff, (unsigned)in->size);
    return 0;
}

void romfsfs_init(void)
{
    block_dev_t *dev = ata_get_dev();
    if (!dev)
        return;                    /* no disk, no mount */
    /* The disk may belong to any filesystems; the superblock picks the
     * owner, so a FAT32 or ISO 9660 disk simply refuses here. */
    if (romfs_mount_tree("/rom", dev) == 0)
        pr_info("romfs: /rom mounted from %s\n", dev->name);
}
