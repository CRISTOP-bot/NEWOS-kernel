/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/fs/iso9660/iso9660.c
 * Description: ISO 9660 read-only filesystem over block_dev_t. The parser walks the
 * Added: 2026-10-22
 */
#include <fs/iso9660.h>
#include <fs/vfs.h>
#include <drivers/ata.h>
#include <mm/mm_heap.h>
#include <core/core_printk.h>
#include <iru_string.h>

/* ISO 9660 read-only filesystem over block_dev_t. The parser walks the
 * primary volume descriptor (system-area fixed address LBA 16), follows
 * directory extents and one record's name field with the ";version"
 * suffix stripped, and materializes the whole tree as plain VFS children
 * exactly like the FAT32 mounter. Multi-extent files (the flag bit in
 * the directory record) are joined into one node with up to eight
 * extents. Rock Ridge, Joliet (SVD) and El Torito are deliberately not
 * parsed: ISO 9660 level names only, 2048-byte logical blocks. */

#define ISO_BLOCK       2048u   /* only supported logical block size */
#define ISO_PVD_LBA     16u     /* system area: 15*2048 zero bytes */
#define ISO_MAX_NAME    64u     /* VFS name cap (we reject longer) */
#define ISO_MAX_EXTENTS 8u
#define ISO_WALK_DEPTH  8u      /* the standard's own directory depth cap */
#define ISO_DIR_ENTRIES 4096u   /* safety bound for broken images */

/* Directory record flag bits. */
#define ISO_FLAG_HIDDEN 0x01u
#define ISO_FLAG_DIR    0x02u
#define ISO_FLAG_CONT   0x80u   /* multi-extent: more records follow */

struct iso_fs {
    block_dev_t *dev;
    u32 block_bytes;           /* logical block size, 2048 required */
    char label[33];
};

struct iso_extent {
    u64 lba;                   /* logical block number */
    u32 size;                  /* bytes of this extent */
};

struct iso_node {
    struct iso_fs *fs;
    u32 nextents;
    struct iso_extent ext[ISO_MAX_EXTENTS];
    u32 size;                  /* total file bytes */
    struct vfs_inode *in;      /* back-pointer: sizes grow with extents */
};

/* --- device read helpers -------------------------------------------------- */

/* One logical block from its LBA; the device quantum is a divisor of
 * 2048 (ata 512, the other iso-block case 2048), so each block maps to
 * whole device sectors. */
static int iso_read_block(struct iso_fs *fs, u64 lba, u8 buf[2048])
{
    u64 off = lba * (u64)fs->block_bytes;
    if (off / fs->dev->sector_size + ISO_BLOCK / fs->dev->sector_size >
        fs->dev->sector_count)
        return -1;
    return fs->dev->read_sectors(fs->dev, off / fs->dev->sector_size,
                                 ISO_BLOCK / fs->dev->sector_size, buf);
}

/* Bytes from (lba, off_in_extent): one-block bounces with slices, for
 * arbitrary (unaligned) read windows inside an extent. */
static int iso_read_span(struct iso_fs *fs, u64 lba, u64 off, u32 len,
                         u8 *buf)
{
    u64 done = 0;
    while (done < len) {
        u64 cursor = off + done;
        u64 blk = lba + cursor / ISO_BLOCK;
        u32 in_remaining = (u32)(ISO_BLOCK - cursor % ISO_BLOCK);
        u32 span = len - (u32)done;
        if (span > in_remaining)
            span = in_remaining;

        u8 cell[ISO_BLOCK];
        if (iso_read_block(fs, blk, cell) != 0)
            return -1;
        memcpy(buf + done, cell + cursor % ISO_BLOCK, span);
        done += span;
    }
    return 0;
}

/* --- descriptor parsing --------------------------------------------------- */

struct iso_rec {
    u32 rec_len;
    u64 lba;
    u32 size;
    u32 flags;
    u8 name_len;
    u8 name[64];               /* raw name bytes; entries above the VFS
                                  63-char cap are skipped, not fatal */
};

/* Parse one directory record. 0 = got it; 1 = end-of-sector padding
 * (rec_len == 0); -1 = malformed record. */
static int iso_parse_rec(const u8 *p, u32 avail, struct iso_rec *r)
{
    if (avail < 1)
        return -1;
    if (p[0] == 0)
        return 1;              /* rest of sector is padding */
    r->rec_len = p[0];
    if (avail < r->rec_len || r->rec_len < 34)
        return -1;
    r->lba = (u32)p[2] | ((u32)p[3] << 8) | ((u32)p[4] << 16) |
             ((u32)p[5] << 24);
    r->size = (u32)p[10] | ((u32)p[11] << 8) | ((u32)p[12] << 16) |
              ((u32)p[13] << 24);
    r->flags = p[25];
    r->name_len = p[32];
    if (r->name_len > sizeof(r->name) || (u32)r->name_len > r->rec_len - 33)
        return -1;
    memcpy(r->name, p + 33, r->name_len);
    return 0;}

/* ISO names carry a ";1" style version suffix; strip at the first ';'. */
static u8 iso_name_len(const u8 *raw, u8 raw_len)
{
    for (u8 j = 0; j < raw_len; j++) {
        if (raw[j] == ';')
            return j;
    }
    return raw_len;
}

/* --- tree materialization ------------------------------------------------- */

static struct inode_ops iso_file_ops;
static struct inode_ops iso_dir_ops;

static int walk_dir(struct iso_fs *fs, u64 dir_lba, u32 dir_size,
                    struct vfs_inode *vfsparent, u32 depth);

static int materialize(struct iso_fs *fs, const char *name,
                       const struct iso_rec *r, struct iso_node *node,
                       struct vfs_inode *vfsparent, u32 depth);

static void iso_node_add_extent(struct iso_node *n, u64 lba, u32 size)
{
    if (n->nextents >= ISO_MAX_EXTENTS)
        return;                    /* cap reached: file stays partial */
    n->ext[n->nextents].lba = lba;
    n->ext[n->nextents].size = size;
    n->nextents++;
    n->size += size;
    if (n->in)                     /* keep the VFS byte size in step */
        n->in->size = n->size;
}

/* Fresh child node from a parsed record (dir or file). */
static struct iso_node *iso_new_node(struct iso_fs *fs,
                                     const struct iso_rec *r)
{
    struct iso_node *node = kzalloc(sizeof(*node));
    if (!node)
        return NULL;
    node->fs = fs;
    iso_node_add_extent(node, r->lba, r->size);
    return node;
}

static int materialize(struct iso_fs *fs, const char *name,
                       const struct iso_rec *r, struct iso_node *node,
                       struct vfs_inode *vfsparent, u32 depth)
{
    u32 kind = (r->flags & ISO_FLAG_DIR) ? VFS_MODE_DIR : VFS_MODE_REG;
    u32 mode = kind | (kind == VFS_MODE_REG ? 0644 : 0755);

    struct vfs_inode *in = vfs_alloc_inode(
        vfsparent, name, mode,
        kind == VFS_MODE_DIR ? &iso_dir_ops : &iso_file_ops,
        node);
    if (!in) {
        kfree(node);
        return -1;
    }
    node->in = in;
    if (kind == VFS_MODE_REG)
        in->size = node->size;     /* VFS convention: files carry bytes;
                                     dirs count children (VFS-managed) */

    if (kind == VFS_MODE_DIR) {
        if (walk_dir(fs, r->lba, r->size, in, depth + 1) != 0)
            return -1;
    }
    return 0;
}

static int walk_dir(struct iso_fs *fs, u64 dir_lba, u32 dir_size,
                    struct vfs_inode *vfsparent, u32 depth)
{
    if (depth > ISO_WALK_DEPTH || dir_size == 0)
        return -1;

    /* Directory extents never split across blocks misaligned: read one
     * block at a time, records never cross the block boundary (padding
     * fills the tail). */
    u64 blocks = (dir_size + ISO_BLOCK - 1) / ISO_BLOCK;
    u32 steps = 0;
    struct iso_node *open_ext = NULL;   /* multi-extent in progress */
    char open_name[64];                 /* the name the CONT chain belongs
                                           to: a mismatched record means
                                           a broken image, drop it */

    for (u64 b = 0; b < blocks; b++) {
        u8 cell[ISO_BLOCK];
        if (iso_read_block(fs, dir_lba + b, cell) != 0)
            return -1;

        u32 pos = 0;
        while (pos < ISO_BLOCK) {
            struct iso_rec r;
            int rc = iso_parse_rec(cell + pos, ISO_BLOCK - pos, &r);
            if (rc < 0)
                return -1;     /* corrupt tree: VFS children so far stay */
            if (rc > 0) {
                pos = ISO_BLOCK;      /* interleaved padding: next block */
                break;
            }
            pos += r.rec_len;

            if (++steps > ISO_DIR_ENTRIES)
                return -1;
            /* "." .. "..": name bytes 0x00 / 0x01 of length 1. */
            if (r.name_len == 1 && (r.name[0] == 0 || r.name[0] == 1))
                continue;

            u8 plen = iso_name_len(r.name, r.name_len);
            char name[64];
            if (plen == 0 || plen >= sizeof(name) ||
                (r.flags & ISO_FLAG_HIDDEN)) {
                /* overlong or hidden: skip the entry (open_ext keeps) */
                continue;
            }
            memcpy(name, r.name, plen);
            name[plen] = '\0';

            if (open_ext) {
                /* Continuation record of a multi-extent file: same
                 * node, one more extent. The final chunk clears the
                 * CONT bit and closes the sequence. Only a matching
                 * name attaches: a mismatch marks a broken image and
                 * the sequence drops before more records are misread. */
                if (strlen(name) != strlen(open_name) ||
                    strcmp(name, open_name) != 0) {
                    open_ext = NULL;
                    printk("iso9660: warning: broken multi-extent chain "
                           "(%s / %s), dropping rest\n", open_name, name);
                    continue;
                }
                iso_node_add_extent(open_ext, r.lba, r.size);
                if (!(r.flags & ISO_FLAG_CONT))
                    open_ext = NULL;
                continue;
            }

            /* Fresh entry: dirs are single-extent by spec; files open
             * a multi-extent sequence when the CONT bit is set. The
             * VFS child mounts now, extents append into it later. */
            struct iso_node *node = iso_new_node(fs, &r);
            if (!node)
                return -1;
            if (materialize(fs, name, &r, node, vfsparent, depth) != 0)
                return -1;
            if (r.flags & ISO_FLAG_CONT) {
                memcpy(open_name, name, plen + 1);
                open_ext = node;
            }
        }
    }
    return 0;
}

/* --- ops ------------------------------------------------------------------ */

static int iso_file_read(struct vfs_file *f, void *buf, size_t len,
                         size_t *got)
{
    struct iso_node *n = f->inode->private;

    if (f->offset >= n->size) {
        *got = 0;
        return 0;
    }
    u64 end = n->size;
    if ((u64)f->offset + len < end)
        end = (u64)f->offset + len;

    u8 *dst = buf;
    u64 pos = f->offset;
    size_t done = 0;
    for (u32 e = 0; e < n->nextents && pos < end; e++) {
        u64 ext_start = 0;
        for (u32 j = 0; j < e; j++)
            ext_start += n->ext[j].size;
        u64 ext_end = ext_start + n->ext[e].size;
        if (pos >= ext_end)
            continue;
        u64 in_off = pos - ext_start;
        u32 span = (u32)(ext_end - pos);
        if ((u64)span > end - pos)
            span = (u32)(end - pos);
        if (span > (u32)((u64)n->ext[e].size - in_off))
            span = (u32)((u64)n->ext[e].size - in_off);
        if (iso_read_span(n->fs, n->ext[e].lba, in_off, span, dst + done)
            != 0)
            break;
        done += span;
        pos += span;
    }
    *got = done;
    return 0;
}

static void iso_destroy(struct vfs_inode *in)
{
    if (in && in->private)
        kfree(in->private);
}

static struct inode_ops iso_file_ops = {
    .read     = iso_file_read,
    .destroy  = iso_destroy,
};

static struct inode_ops iso_dir_ops = {
    .destroy  = iso_destroy,
};

/* --- mount + init --------------------------------------------------------- */

int iso9660_mount_tree(const char *path, block_dev_t *dev)
{
    if (!path || !dev || !dev->read_sectors)
        return -1;
    if (dev->sector_size == 0 || ISO_BLOCK % dev->sector_size != 0)
        return -1;                 /* 2048 must land on whole sectors */

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!*base || strlen(base) >= 64)
        return -1;

    struct iso_fs *fs = kzalloc(sizeof(*fs));
    if (!fs)
        return -1;
    fs->dev = dev;
    fs->block_bytes = ISO_BLOCK;

    u8 pvd[ISO_BLOCK];
    if (iso_read_block(fs, ISO_PVD_LBA, pvd) != 0) {
        kfree(fs);
        return -1;
    }
    if (pvd[0] != 1 || pvd[6] != 1 ||
        pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0' ||
        pvd[4] != '0' || pvd[5] != '1') {
        kfree(fs);
        return -1;                 /* not an ISO 9660 volume: quiet */
    }

    u32 lbs = (u32)pvd[128] | ((u32)pvd[129] << 8);
    if (lbs != ISO_BLOCK) {
        kfree(fs);
        return -1;                 /* only 2048-byte blocks supported */
    }

    memcpy(fs->label, pvd + 40, 32);
    fs->label[32] = '\0';

    /* Root directory record at PVD + 156. */
    u8 root_raw[34];
    memcpy(root_raw, pvd + 156, sizeof(root_raw));
    if (root_raw[0] < 34 || !(root_raw[25] & ISO_FLAG_DIR)) {
        kfree(fs);
        return -1;
    }
    u64 root_lba = (u32)root_raw[2] | ((u32)root_raw[3] << 8) |
                   ((u32)root_raw[4] << 16) | ((u32)root_raw[5] << 24);
    u32 root_size = (u32)root_raw[10] | ((u32)root_raw[11] << 8) |
                    ((u32)root_raw[12] << 16) | ((u32)root_raw[13] << 24);

    if (vfs_lookup(path)) {
        kfree(fs);
        return -1;                 /* already mounted */
    }

    struct iso_node *rnode = kzalloc(sizeof(*rnode));
    if (!rnode) {
        kfree(fs);
        return -1;
    }
    rnode->fs = fs;
    rnode->nextents = 1;
    rnode->ext[0].lba = root_lba;
    rnode->ext[0].size = root_size;
    rnode->size = root_size;

    struct vfs_inode *in = vfs_alloc_inode(vfs_root(), base,
                                           VFS_MODE_DIR | 0755,
                                           &iso_dir_ops, rnode);
    if (!in) {
        kfree(rnode);
        kfree(fs);
        return -1;
    }

    if (walk_dir(fs, root_lba, root_size, in, 1) != 0)
        printk("iso9660: warning: %s tree incomplete, partial mount\n",
               path);

    printk("iso9660: %s on %s: label \"%s\"\n", path, dev->name, fs->label);
    return 0;
}

void iso9660fs_init(void)
{
    block_dev_t *dev = ata_get_dev();
    if (!dev)
        return;                    /* no disk, no mount */
    if (iso9660_mount_tree("/iso", dev) == 0)
        pr_info("iso9660: /iso mounted from %s\n", dev->name);
}
