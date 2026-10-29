/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/fs/fat32/fat32fs.c
 * Description: FAT32 read-write filesystem over a block_dev_t (primary-master ATA
 * Added: 2026-10-22
 */
#include <fs/fat32.h>
#include <fs/vfs.h>
#include <core/core.h>
#include <drivers/ata.h>
#include <mm/mm_heap.h>
#include <core/core_printk.h>
#include <iru_string.h>

/* FAT32 read-write filesystem over a block_dev_t (primary-master ATA
 * disk0 by default), mounted under /fat.
 *
 * Shape behind the VFS: directory inodes materialize their children in
 * the regular VFS children list when they are visited (mount walk for
 * what the disk already holds, hook work for runtime children), so
 * lookup and readdir stay in the VFS core. The child list stays in sync
 * with the dirent stream because create/mkdir/unlink all run through
 * inode_ops hooks that actually mutate the volume; unlink reaches the
 * disk through ops->destroy, which the VFS core already calls before
 * freeing the inode.
 *
 * Honest limits: the directory namespace is 8.3 (LFN entries are skipped
 * when listing - the file shows up under its short alias), mangled
 * names truncate to 8+3 without ~N suffix logic, FAT16 volumes are
 * refused (the driver demands the FAT32 minimum cluster count), writes
 * may overwrite at/before EOF or append strictly from EOF (no gap
 * punching beyond EOF), rename of volume files is refused by the VFS
 * core, and the FSInfo sector is not maintained. */

#define FAT32_DE_END        0x00   /* no valid entry beyond this slot */
#define FAT32_DE_DELETED    0xE5
#define FAT32_ATTR_RO       0x01
#define FAT32_ATTR_VOLUME   0x08
#define FAT32_ATTR_DIR      0x10
#define FAT32_ATTR_LFN      0x0F
#define FAT32_ATTR_ARCHIVE  0x20

#define FAT32_FLAG_BASE_LC  0x08   /* dirent byte 12: base is lowercase */
#define FAT32_FLAG_EXT_LC   0x10   /* dirent byte 12: extension lower */

#define FAT32_EOC_MIN       0x0FFFFFF8u
#define FAT32_FAT_FREE      0x00000000u

#define FAT32_MOUNT_DEPTH   12     /* recursion cap against corrupt chains */

struct fat32_fs {
    block_dev_t *dev;
    u32 bytes_per_sector;
    u32 sectors_per_cluster;
    u32 cluster_bytes;
    u32 fat_lba;               /* sector of FAT[0] */
    u32 fat_sectors;           /* sectors of a single FAT copy */
    u32 num_fats;
    u32 root_cluster;
    u32 data_lba;              /* sector address of cluster 2 */
    u32 total_clusters;
    u32 alloc_cursor;          /* rotating scan base for alloc_cluster */
};

/* Per-inode volume object. dir_chain_head/entry_index locate the inode's
 * own dirent so size/cluster updates can rewrite it; dir_chain_head == 0
 * means the node carries no dirent of its own (the mount root). */
struct fat32_node {
    struct fat32_fs *fs;
    u32 first_cluster;
    u64 size;                  /* files; directories use the children list */
    u32 dir_chain_head;
    u32 entry_index;
};

/* Tentative definitions; the real initializers sit at the bottom, after
 * every referenced function is visible. */
static struct inode_ops fat32_file_ops;
static struct inode_ops fat32_dir_ops;

static int attach_node(struct fat32_fs *fs, struct vfs_inode *vfsparent,
                       const u8 *de, u32 dir_chain_head, u32 entry_index,
                       int depth);
static int walk_directory(struct fat32_fs *fs, u32 chain_head,
                          struct vfs_inode *vfsparent, int depth);

/* --- ASCII + little-endian helpers --------------------------------------- */

static char ascii_up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static char ascii_down(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static u32 le16(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8);
}

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static void put_le16(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
}

static void put_le32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF);
    p[3] = (u8)((v >> 24) & 0xFF);
}

/* --- sector and FAT primitives ------------------------------------------- */

static int rd_sectors(struct fat32_fs *fs, u64 lba, u32 count, u8 *buf)
{
    return block_dev_read(fs->dev, lba, count, buf);
}

static int wr_sectors(struct fat32_fs *fs, u64 lba, u32 count,
                      const u8 *buf)
{
    return block_dev_write(fs->dev, lba, count, buf);
}

static u32 fat_eps_per_sector(struct fat32_fs *fs)
{
    return fs->bytes_per_sector / 4;
}

static int read_fat_entry(struct fat32_fs *fs, u32 clus, u32 *val)
{
    if (clus < 2 || clus >= fs->total_clusters + 2)
        return -1;

    u32 lba = fs->fat_lba + clus / fat_eps_per_sector(fs);
    u32 off = (clus % fat_eps_per_sector(fs)) * 4;
    u8 sec[512];

    if (fs->bytes_per_sector != sizeof(sec) ||
        rd_sectors(fs, lba, 1, sec) != 0)
        return -1;
    *val = le32(sec + off) & 0x0FFFFFFFu;
    return 0;
}

static int write_fat_entry(struct fat32_fs *fs, u32 clus, u32 val)
{
    if (clus < 2 || clus >= fs->total_clusters + 2)
        return -1;

    u32 lba = fs->fat_lba + clus / fat_eps_per_sector(fs);
    u32 off = (clus % fat_eps_per_sector(fs)) * 4;
    u8 sec[512];

    if (fs->bytes_per_sector != sizeof(sec) ||
        rd_sectors(fs, lba, 1, sec) != 0)
        return -1;
    put_le32(sec + off, val);

    /* Mirror across every FAT copy so the copies can never diverge. */
    for (u32 copy = 0; copy < fs->num_fats; copy++) {
        if (wr_sectors(fs, lba + (u64)copy * fs->fat_sectors, 1, sec))
            return -1;
    }
    return 0;
}

static int fat32_is_eoc(u32 v)
{
    return v >= FAT32_EOC_MIN;
}

/* Next cluster of the chain; 0 on error, broken link, or EOC. */
static u32 fat_next(struct fat32_fs *fs, u32 clus)
{
    u32 v = 0;
    if (read_fat_entry(fs, clus, &v) != 0)
        return 0;
    if (fat32_is_eoc(v))
        return 0;
    return v;
}

/* Claim a free cluster and pre-mark it EOC; 0 when the volume is full.
 * The scan rotates so repeated appends stop re-reading the used head. */
static u32 alloc_cluster(struct fat32_fs *fs)
{
    u32 idx = fs->alloc_cursor;
    u32 count = fs->total_clusters;

    for (u32 i = 0; i < count; i++) {
        u32 clus = 2 + (idx + i) % count;
        u32 v = 1;
        if (read_fat_entry(fs, clus, &v) != 0)
            return 0;
        if (v == FAT32_FAT_FREE) {
            if (write_fat_entry(fs, clus, FAT32_EOC_MIN) != 0)
                return 0;
            fs->alloc_cursor = (idx + i + 1) % count;
            return clus;
        }
    }
    return 0;
}

/* Mark every cluster from `head` on as free. Terminates at EOC; a loop
 * from a corrupt volume gives up after a volume-sized step budget. */
static void free_chain(struct fat32_fs *fs, u32 head)
{
    u32 clus = head;
    u32 guard = fs->total_clusters;
    while (clus >= 2 && guard--) {
        u32 nxt = fat_next(fs, clus);
        write_fat_entry(fs, clus, FAT32_FAT_FREE);
        if (nxt == 0)
            break;
        clus = nxt;
    }
}

static u32 cluster_lba(struct fat32_fs *fs, u32 clus)
{
    return fs->data_lba + (u64)(clus - 2) * fs->sectors_per_cluster;
}

static int read_cluster(struct fat32_fs *fs, u32 clus, u8 *buf)
{
    return rd_sectors(fs, cluster_lba(fs, clus),
                      fs->sectors_per_cluster, buf);
}

static int write_cluster(struct fat32_fs *fs, u32 clus, const u8 *buf)
{
    return wr_sectors(fs, cluster_lba(fs, clus),
                      fs->sectors_per_cluster, buf);
}

/* Cluster `steps` links into the chain starting at `head`; 0 when the
 * chain ends first. */
static u32 chain_at(struct fat32_fs *fs, u32 head, u64 steps)
{
    u32 clus = head;
    while (steps-- > 0) {
        clus = fat_next(fs, clus);
        if (clus < 2)
            return 0;
    }
    return clus;
}

/* --- directory stream ------------------------------------------------------ */

static u32 entries_per_cluster(struct fat32_fs *fs)
{
    return fs->cluster_bytes / 32;
}

/* Load dirent slot `index`: 1 and *de filled; 0 beyond the stream;
 * -1 on device error. */
static int dir_entry_read(struct fat32_fs *fs, u32 chain_head,
                          u32 index, u8 *de)
{
    u32 epc = entries_per_cluster(fs);
    u32 clus = chain_at(fs, chain_head, index / epc);
    if (clus < 2)
        return 0;

    u8 *buf = kmalloc(fs->cluster_bytes);
    if (!buf)
        return -1;
    if (read_cluster(fs, clus, buf) != 0) {
        kfree(buf);
        return -1;
    }
    memcpy(de, buf + (index % epc) * 32, 32);
    kfree(buf);
    return 1;
}

/* Patch dirent slot `index` with the 32 bytes held by `de`. */
static int dir_entry_write(struct fat32_fs *fs, u32 chain_head,
                           u32 index, const u8 *de)
{
    u32 epc = entries_per_cluster(fs);
    u32 clus = chain_at(fs, chain_head, index / epc);
    if (clus < 2)
        return -1;

    u8 *buf = kmalloc(fs->cluster_bytes);
    if (!buf)
        return -1;
    if (read_cluster(fs, clus, buf) != 0) {
        kfree(buf);
        return -1;
    }
    memcpy(buf + (index % epc) * 32, de, 32);
    int rc = write_cluster(fs, clus, buf);
    kfree(buf);
    return rc;
}

/* Take an end-marker slot (0x00): the entry is written there and every
 * slot after it in the same cluster is zeroed, otherwise stale
 * pre-terminator garbage would become listable. */
static int dir_entry_write_tail(struct fat32_fs *fs, u32 chain_head,
                                u32 index, const u8 *de)
{
    u32 epc = entries_per_cluster(fs);
    u32 clus = chain_at(fs, chain_head, index / epc);
    if (clus < 2)
        return -1;

    u8 *buf = kmalloc(fs->cluster_bytes);
    if (!buf)
        return -1;
    if (read_cluster(fs, clus, buf) != 0) {
        kfree(buf);
        return -1;
    }
    memcpy(buf + (index % epc) * 32, de, 32);
    for (u32 i = index % epc + 1; i < epc; i++)
        memset(buf + i * 32, 0, 32);
    int rc = write_cluster(fs, clus, buf);
    kfree(buf);
    return rc;
}

/* Find a usable dirent slot: 0 = a deleted (0xE5) entry inside an
 * existing cluster (write with dir_entry_write), 1 = an end-marker slot
 * inside an existing cluster (write with dir_entry_write_tail), 2 = the
 * stream is spent - the caller must append one directory cluster, then
 * the slot names its first entry. *slot gets the global entry index,
 * *chain_len the number of clusters the stream spans. -1 on error. */
static int find_free_slot(struct fat32_fs *fs, u32 chain_head,
                          u32 *slot, u32 *chain_len)
{
    u32 epc = entries_per_cluster(fs);
    u8 *buf = kmalloc(fs->cluster_bytes);
    if (!buf)
        return -1;

    u32 steps = 0;
    u32 clus = chain_head;
    while (clus >= 2) {
        if (read_cluster(fs, clus, buf) != 0) {
            kfree(buf);
            return -1;
        }
        for (u32 i = 0; i < epc; i++) {
            u8 b0 = buf[i * 32];
            if (b0 == FAT32_DE_END || b0 == FAT32_DE_DELETED) {
                kfree(buf);
                *slot = steps * epc + i;
                *chain_len = steps + 1;
                return b0 == FAT32_DE_DELETED ? 0 : 1;
            }
        }
        steps++;
        clus = fat_next(fs, clus);
    }
    kfree(buf);
    *slot = steps * epc;
    *chain_len = steps;
    return 2;
}

/* Append one zeroed cluster at the end of a directory chain. Returns
 * the new cluster, 0 on failure. */
static u32 grow_dir_chain(struct fat32_fs *fs, u32 chain_head,
                          u32 chain_len)
{
    if (chain_len == 0)
        return 0;
    u32 last = chain_at(fs, chain_head, chain_len - 1);
    if (last < 2)
        return 0;
    u32 fresh = alloc_cluster(fs);
    if (fresh < 2)
        return 0;

    u8 *zero = kmalloc(fs->cluster_bytes);
    if (!zero)
        return 0;
    memset(zero, 0, fs->cluster_bytes);
    int rc = write_cluster(fs, fresh, zero);
    kfree(zero);
    if (rc != 0)
        return 0;
    if (write_fat_entry(fs, last, fresh) != 0)
        return 0;
    return fresh;
}

/* Does an 11-byte 8.3 name already exist in the directory? */
static int dir_83_exists(struct fat32_fs *fs, u32 chain_head,
                         const char *name11)
{
    u32 epc = entries_per_cluster(fs);
    u8 *buf = kmalloc(fs->cluster_bytes);
    if (!buf)
        return -1;

    u32 clus = chain_head;
    while (clus >= 2) {
        if (read_cluster(fs, clus, buf) != 0) {
            kfree(buf);
            return -1;
        }
        for (u32 i = 0; i < epc; i++) {
            const u8 *de = buf + i * 32;
            if (de[0] == FAT32_DE_END)
                break;
            if (de[0] == FAT32_DE_DELETED || de[11] == FAT32_ATTR_LFN)
                continue;
            if (de[11] & FAT32_ATTR_VOLUME)
                continue;
            if (memcmp(de, name11, 11) == 0) {
                kfree(buf);
                return 1;
            }
        }
        clus = fat_next(fs, clus);
    }
    kfree(buf);
    return 0;
}

/* --- dirent field access ---------------------------------------------------- */

static u32 de_cluster(const u8 *de)
{
    return ((u32)le16(de + 20) << 16) | le16(de + 26);
}

static u32 de_size(const u8 *de)
{
    return le32(de + 28);
}

static int de_is_dir(const u8 *de)
{
    return (de[11] & FAT32_ATTR_DIR) != 0;
}

static void make_dirent(u8 *de, const char *name11, u8 attr,
                        u32 first_cluster, u32 size)
{
    memset(de, 0, 32);
    memcpy(de, name11, 11);
    de[11] = attr;
    put_le16(de + 20, first_cluster >> 16);
    put_le16(de + 26, first_cluster & 0xFFFF);
    put_le32(de + 28, size);
}

/* Render the 8.3 dirent name as a VFS name: trailing spaces trimmed,
 * extension joined with '.', NT lower-case flags respected, 0x05 in the
 * first byte restored to 0xE5 per spec. */
static void de_make_name(const u8 *de, char *out, size_t out_sz)
{
    int b = 0;
    while (b < 8 && de[b] != ' ')
        b++;
    int e = 0;
    while (e < 3 && de[8 + e] != ' ')
        e++;

    size_t n = 0;
    for (int i = 0; i < b && n + 1 < out_sz; i++) {
        char c = (char)de[i];
        if (i == 0 && de[0] == 0x05)
            c = (char)0xE5;
        if (de[12] & FAT32_FLAG_BASE_LC)
            c = ascii_down(c);
        out[n++] = c;
    }
    if (e > 0 && n + 1 < out_sz) {
        out[n++] = '.';
        for (int i = 0; i < e && n + 1 < out_sz; i++) {
            char c = (char)de[8 + i];
            if (de[12] & FAT32_FLAG_EXT_LC)
                c = ascii_down(c);
            out[n++] = c;
        }
    }
    out[n] = '\0';
}

static int fat32_83_valid(char c)
{
    c = ascii_up(c);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return 1;
    return strchr("!#$%&'()-@^_`{}~", c) != NULL;
}

/* VFS name -> 11-byte 8.3 dirent name. Returns 0 when it fits without
 * mangling, 1 when characters had to be dropped (duplicates in the
 * collapsed space are refused at create time), -1 when nothing
 * representable survives. */
static int build_83(const char *name, char dst[11])
{
    memset(dst, ' ', 11);

    const char *dot = strrchr(name, '.');
    int blen = dot ? (int)(dot - name) : (int)strlen(name);
    int elen = (dot && dot[1]) ? (int)strlen(dot + 1) : 0;
    if (dot == name)
        blen = 0;                  /* ".hidden": no usable base */
    if (blen == 0)
        return -1;

    int clean = 1;
    int b = 0;
    for (int i = 0; i < blen && b < 8; i++) {
        char c = ascii_up(name[i]);
        if (fat32_83_valid(c))
            dst[b++] = c;
        else
            clean = 0;
    }
    int e = 0;
    for (int i = 0; i < elen && e < 3; i++) {
        char c = ascii_up(dot[1 + i]);
        if (fat32_83_valid(c))
            dst[8 + e++] = c;
        else
            clean = 0;
    }
    if (blen > 8 || elen > 3)
        clean = 0;
    if (b == 0)
        return -1;
    return clean ? 0 : 1;
}

/* --- tree walking ---------------------------------------------------------- */

static int attach_node(struct fat32_fs *fs, struct vfs_inode *vfsparent,
                       const u8 *de, u32 dir_chain_head, u32 entry_index,
                       int depth)
{
    char name[64];
    de_make_name(de, name, sizeof(name));
    if (name[0] == '\0')
        return 0;                  /* not representable: skip, not fail */

    struct fat32_node *node = kmalloc(sizeof(*node));
    if (!node)
        return -1;
    node->fs = fs;
    node->first_cluster = de_cluster(de);
    node->size = de_size(de);
    node->dir_chain_head = dir_chain_head;
    node->entry_index = entry_index;

    u32 vmode = de_is_dir(de) ? (u32)(VFS_MODE_DIR | 0755)
                              : (u32)(VFS_MODE_REG | 0644);
    if (de[11] & FAT32_ATTR_RO)
        vmode &= ~0222u;

    struct vfs_inode *in = vfs_alloc_inode(
        vfsparent, name, vmode,
        de_is_dir(de) ? &fat32_dir_ops : &fat32_file_ops,
        node);
    if (!in) {
        kfree(node);
        return -1;
    }

    if (de_is_dir(de))
        return walk_directory(fs, de_cluster(de), in, depth + 1);
    return 0;
}

static int walk_directory(struct fat32_fs *fs, u32 chain_head,
                          struct vfs_inode *vfsparent, int depth)
{
    static const char dot11[11] = {'.', ' ', ' ', ' ', ' ', ' ',
                                   ' ', ' ', ' ', ' ', ' '};
    static const char dotdot11[11] = {'.', '.', ' ', ' ', ' ', ' ',
                                      ' ', ' ', ' ', ' ', ' '};

    if (depth > FAT32_MOUNT_DEPTH)
        return -1;                 /* corrupt .. chains must not loop */

    u32 epc = entries_per_cluster(fs);
    u8 *buf = kmalloc(fs->cluster_bytes);
    if (!buf)
        return -1;

    int rc = 0;
    u32 steps = 0;
    u32 clus = chain_head;
    while (clus >= 2 && rc == 0) {
        if (read_cluster(fs, clus, buf) != 0) {
            rc = -1;
            break;
        }
        u32 base = steps * epc;
        for (u32 i = 0; i < epc; i++) {
            const u8 *de = buf + i * 32;
            if (de[0] == FAT32_DE_END)
                goto done;
            if (de[0] == FAT32_DE_DELETED || de[11] == FAT32_ATTR_LFN)
                continue;
            if (de[11] & FAT32_ATTR_VOLUME)
                continue;
            if (memcmp(de, dot11, 11) == 0 ||
                memcmp(de, dotdot11, 11) == 0)
                continue;          /* VFS handles . and .. itself */
            rc = attach_node(fs, vfsparent, de, chain_head, base + i,
                             depth);
            if (rc != 0)
                break;
        }
        steps++;
        clus = fat_next(fs, clus);
    }
done:
    kfree(buf);
    return rc;
}

/* --- file semantics -------------------------------------------------------- */

static void patch_dirent_tail(struct fat32_node *node)
{
    u8 de[32];

    if (node->dir_chain_head == 0)
        return;
    if (dir_entry_read(node->fs, node->dir_chain_head,
                       node->entry_index, de) != 1)
        return;                    /* transient: data stays on the disk */
    put_le32(de + 28, (u32)node->size);
    put_le16(de + 20, node->first_cluster >> 16);
    put_le16(de + 26, node->first_cluster & 0xFFFF);
    dir_entry_write(node->fs, node->dir_chain_head,
                    node->entry_index, de);
}

static int fat32_file_read(struct vfs_file *f, void *buf, size_t len,
                           size_t *got)
{
    struct fat32_node *node = f->inode->private;
    struct fat32_fs *fs = node->fs;

    if (f->offset >= node->size) {
        *got = 0;
        return 0;
    }
    u64 end = MIN(node->size, f->offset + len);

    u8 *dst = buf;
    u64 pos = f->offset;
    size_t done = 0;

    while (pos < end) {
        u32 clus = chain_at(fs, node->first_cluster,
                            pos / fs->cluster_bytes);
        if (clus < 2)
            break;                 /* truncated chain: EOF mid-read */
        u32 off = (u32)(pos % fs->cluster_bytes);
        u32 span = fs->cluster_bytes - off;
        if ((u64)span > end - pos)
            span = (u32)(end - pos);

        u8 *cbuf = kmalloc(fs->cluster_bytes);
        if (!cbuf)
            break;
        int rc = read_cluster(fs, clus, cbuf);
        if (rc == 0)
            memcpy(dst + done, cbuf + off, span);
        kfree(cbuf);
        if (rc != 0)
            break;

        done += span;
        pos += span;
    }

    *got = done;
    return 0;
}

static int fat32_file_write(struct vfs_file *f, const void *buf,
                            size_t len, size_t *wrote)
{
    struct fat32_node *node = f->inode->private;
    struct fat32_fs *fs = node->fs;

    /* Overwrite at/before EOF or append strictly from EOF: byte ranges
     * that would punch a gap past EOF are not representable. */
    if (f->offset > node->size) {
        *wrote = 0;
        return -1;
    }

    const u8 *src = buf;
    u64 pos = f->offset;
    u64 end = f->offset + len;
    size_t done = 0;

    while (pos < end) {
        u64 idx = pos / fs->cluster_bytes;
        u32 clus = chain_at(fs, node->first_cluster, idx);

        if (clus < 2) {
            /* Only append-extend is possible here (idx >= 2 means the
             * chain was shorter than the write position, reachable
             * only when first_cluster == 0 and idx == 0). */
            if (idx != 0)
                break;
            u32 fresh = alloc_cluster(fs);
            if (fresh < 2)
                break;             /* volume full */
            u8 *zero = kmalloc(fs->cluster_bytes);
            if (!zero)
                break;
            memset(zero, 0, fs->cluster_bytes);
            int rc = write_cluster(fs, fresh, zero);
            kfree(zero);
            if (rc != 0)
                break;
            node->first_cluster = fresh;
            clus = fresh;
        }

        u32 off = (u32)(pos % fs->cluster_bytes);
        u32 span = fs->cluster_bytes - off;
        if ((u64)span > end - pos)
            span = (u32)(end - pos);

        u8 *cbuf = kmalloc(fs->cluster_bytes);
        if (!cbuf)
            break;
        int rc = 0;
        if (span < fs->cluster_bytes)
            rc = read_cluster(fs, clus, cbuf);    /* read-modify-write */
        else
            memset(cbuf, 0, fs->cluster_bytes);
        if (rc == 0) {
            memcpy(cbuf + off, src + done, span);
            rc = write_cluster(fs, clus, cbuf);
        }
        kfree(cbuf);
        if (rc != 0)
            break;

        done += span;
        pos += span;
    }

    if (done == 0) {
        *wrote = 0;
        return -1;
    }

    node->size = MAX(node->size, pos);
    f->inode->size = node->size;
    patch_dirent_tail(node);
    *wrote = done;
    return 0;
}

static int fat32_file_truncate(struct vfs_inode *inode, size_t size)
{
    struct fat32_node *node = inode->private;
    struct fat32_fs *fs = node->fs;

    u32 needed = (size == 0 ? 0
                            : (u32)((size + fs->cluster_bytes - 1) /
                                    fs->cluster_bytes));

    u64 cur_len = 0;
    {
        u32 c = node->first_cluster;
        while (c >= 2) {
            cur_len++;
            c = fat_next(fs, c);
        }
    }

    if (needed > cur_len) {
        /* Grow with zeroed clusters linked onto the existing tail. */
        u32 last = (cur_len == 0) ? 0
                                  : chain_at(fs, node->first_cluster,
                                             cur_len - 1);
        while (cur_len < needed) {
            u32 fresh = alloc_cluster(fs);
            if (fresh < 2)
                return -1;         /* partial growth: size unchanged */
            u8 *zero = kmalloc(fs->cluster_bytes);
            if (!zero)
                return -1;
            memset(zero, 0, fs->cluster_bytes);
            int rc = write_cluster(fs, fresh, zero);
            kfree(zero);
            if (rc != 0)
                return -1;
            if (last >= 2) {
                if (write_fat_entry(fs, last, fresh) != 0)
                    return -1;
            } else {
                node->first_cluster = fresh;
            }
            last = fresh;
            cur_len++;
        }
    } else if (needed < cur_len) {
        if (needed == 0) {
            free_chain(fs, node->first_cluster);
            node->first_cluster = 0;
        } else {
            u32 keep_last = chain_at(fs, node->first_cluster,
                                     needed - 1);
            if (keep_last < 2)
                return -1;
            u32 rest = fat_next(fs, keep_last);
            if (write_fat_entry(fs, keep_last, FAT32_EOC_MIN) != 0)
                return -1;
            if (rest >= 2)
                free_chain(fs, rest);
        }
    }

    node->size = size;
    inode->size = size;
    patch_dirent_tail(node);
    return 0;
}

/* The on-disk footprint dies here: unlink reached this hook after the
 * VFS core dropped the inode from its directory list. Files free the
 * cluster chain; directories free the subdirectory chain (the VFS core
 * refuses non-empty directories, so nothing is orphaned). */
static void fat32_destroy(struct vfs_inode *inode)
{
    struct fat32_node *node = inode->private;
    if (!node)
        return;
    struct fat32_fs *fs = node->fs;

    if (node->first_cluster >= 2)
        free_chain(fs, node->first_cluster);

    if (node->dir_chain_head != 0) {
        u8 de[32];
        if (dir_entry_read(fs, node->dir_chain_head,
                           node->entry_index, de) == 1) {
            de[0] = FAT32_DE_DELETED;
            dir_entry_write(fs, node->dir_chain_head,
                            node->entry_index, de);
        }
    }
    kfree(node);
    inode->private = NULL;
}

/* --- directory hooks -------------------------------------------------------- */

/* Materialize a fresh regular file on the volume (vfs_create reached
 * this hook after its own duplicate check; the 8.3 view carries the
 * authority here and a second name that collapses into the same short
 * entry is refused). */
static struct vfs_inode *fat32_dir_create(struct vfs_inode *parent,
                                          const char *name, u32 mode)
{
    struct fat32_node *pnode = parent->private;
    struct fat32_fs *fs = pnode->fs;

    (void)mode;                    /* vfs_create passes REG|0644 */

    char name11[11];
    if (build_83(name, name11) < 0)
        return NULL;

    int dup = dir_83_exists(fs, pnode->first_cluster, name11);
    if (dup != 0)
        return NULL;               /* 1 = exists, -1 = device error */

    u32 slot, chain_len;
    int frc = find_free_slot(fs, pnode->first_cluster, &slot, &chain_len);
    if (frc < 0)
        return NULL;
    if (frc == 2) {
        u32 fresh = grow_dir_chain(fs, pnode->first_cluster, chain_len);
        if (fresh < 2)
            return NULL;           /* volume full */
    }

    u8 de[32];
    make_dirent(de, name11, FAT32_ATTR_ARCHIVE, 0, 0);
    int wc = (frc == 1)
                 ? dir_entry_write_tail(fs, pnode->first_cluster, slot, de)
                 : dir_entry_write(fs, pnode->first_cluster, slot, de);
    if (wc != 0)
        return NULL;

    struct fat32_node *node = kmalloc(sizeof(*node));
    if (!node)
        return NULL;
    node->fs = fs;
    node->first_cluster = 0;
    node->size = 0;
    node->dir_chain_head = pnode->first_cluster;
    node->entry_index = slot;
    struct vfs_inode *in = vfs_alloc_inode(parent, name,
                                           VFS_MODE_REG | 0644,
                                           &fat32_file_ops, node);
    if (!in) {
        kfree(node);
        return NULL;
    }
    return in;
}

static int fat32_dir_mkdir(struct vfs_inode *parent, const char *name)
{
    struct fat32_node *pnode = parent->private;
    struct fat32_fs *fs = pnode->fs;

    char name11[11];
    if (build_83(name, name11) < 0)
        return -1;

    int dup = dir_83_exists(fs, pnode->first_cluster, name11);
    if (dup != 0)
        return -1;

    u32 slot, chain_len;
    int frc = find_free_slot(fs, pnode->first_cluster, &slot, &chain_len);
    if (frc < 0)
        return -1;
    if (frc == 2) {
        u32 fresh = grow_dir_chain(fs, pnode->first_cluster, chain_len);
        if (fresh < 2)
            return -1;
    }

    u32 fresh = alloc_cluster(fs);
    if (fresh < 2)
        return -1;                 /* volume full */

    /* The new directory opens with . and .. (classic self- and parent
     * references); the rest of the cluster stays the 0x00 terminator. */
    u8 *cbuf = kmalloc(fs->cluster_bytes);
    if (!cbuf)
        return -1;
    memset(cbuf, 0, fs->cluster_bytes);
    char dot11[11], dotdot11[11];
    memset(dot11, ' ', 11);
    memset(dotdot11, ' ', 11);
    dot11[0] = '.';
    dotdot11[0] = '.';
    dotdot11[1] = '.';
    make_dirent(cbuf + 0, dot11, FAT32_ATTR_DIR, fresh, 0);
    make_dirent(cbuf + 32, dotdot11, FAT32_ATTR_DIR,
                pnode->first_cluster, 0);
    int rc = write_cluster(fs, fresh, cbuf);
    kfree(cbuf);
    if (rc != 0)
        return -1;

    u8 de[32];
    make_dirent(de, name11, FAT32_ATTR_DIR, fresh, 0);
    int wc = (frc == 1)
                 ? dir_entry_write_tail(fs, pnode->first_cluster, slot, de)
                 : dir_entry_write(fs, pnode->first_cluster, slot, de);
    if (wc != 0)
        return -1;

    struct fat32_node *node = kmalloc(sizeof(*node));
    if (!node)
        return -1;
    node->fs = fs;
    node->first_cluster = fresh;
    node->size = 0;
    node->dir_chain_head = pnode->first_cluster;
    node->entry_index = slot;
    struct vfs_inode *in = vfs_alloc_inode(parent, name,
                                           VFS_MODE_DIR | 0755,
                                           &fat32_dir_ops, node);
    if (!in) {
        kfree(node);
        return -1;
    }
    return 0;
}

/* --- mount ------------------------------------------------------------------ */

static int fat32_power_of_two(u32 v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

/* Parse + validate the BIOS parameter block into the fs struct. FAT16
 * is refused explicitly: FAT32 mandates RootEntCnt == 0, FATSz16 == 0
 * and at least 65525 data clusters (mkfs.vfat prints the same warning). */
static int fat32_parse_geometry(struct fat32_fs *fs)
{
    u8 sec[512];
    block_dev_t *dev = fs->dev;

    if (fs->dev->sector_size != sizeof(sec) ||
        rd_sectors(fs, 0, 1, sec) != 0) {
        printk("fat32: boot sector read failed (dev %s, %u-byte sectors)\n",
               dev && dev->name ? dev->name : "disk",
               fs->dev->sector_size);
        return -1;
    }
    if (le16(sec + 510) != 0xAA55) {
        printk("fat32: boot sector signature is %04x, not 0xAA55\n",
               le16(sec + 510));
        return -1;
    }

    u32 bps = le16(sec + 11);
    u32 spc = sec[13];
    u32 rsvd = le16(sec + 14);
    u32 nfats = sec[16];
    u32 rootent = le16(sec + 17);
    u32 totsec16 = le16(sec + 19);
    u32 fatsz16 = le16(sec + 22);
    u32 totsec32 = le32(sec + 32);
    u32 fatsz32 = le32(sec + 36);
    u32 rootclus = le32(sec + 44);

    if (bps != 512 || !fat32_power_of_two(spc) || spc > 64 ||
        rsvd == 0 || nfats < 1 || nfats > 4 ||
        rootent != 0 || fatsz16 != 0 || fatsz32 == 0) {
        printk("fat32: BPB reject: bps=%u spc=%u rsvd=%u nfats=%u "
               "rootent=%u fatsz16=%u fatsz32=%u\n",
               bps, spc, rsvd, nfats, rootent, fatsz16, fatsz32);
        return -1;
    }

    u32 totsec = totsec32 ? totsec32 : totsec16;
    if (totsec == 0) {
        printk("fat32: BPB reject: total sector count is 0\n");
        return -1;
    }

    fs->bytes_per_sector = bps;
    fs->sectors_per_cluster = spc;
    fs->cluster_bytes = bps * spc;
    fs->fat_lba = rsvd;
    fs->fat_sectors = fatsz32;
    fs->num_fats = nfats;
    fs->root_cluster = rootclus;
    fs->data_lba = rsvd + (u64)fatsz32 * nfats;

    if (totsec <= fs->data_lba) {
        printk("fat32: BPB reject: totsec%u <= data start %u\n",
               totsec, (u32)fs->data_lba);
        return -1;
    }
    fs->total_clusters = (u32)((totsec - fs->data_lba) / spc);
    if (fs->total_clusters < 65525) {
        printk("fat32: BPB reject: %u clusters < FAT32 minimum\n",
               fs->total_clusters);
        return -1;
    }
    if (rootclus < 2 || rootclus >= fs->total_clusters + 2) {
        printk("fat32: BPB reject: root cluster %u out of range\n",
               rootclus);
        return -1;
    }

    fs->alloc_cursor = 0;
    return 0;
}

int fat32_mount_tree(const char *path, block_dev_t *dev)
{
    if (!path || !dev || !dev->read_sectors)
        return -1;

    /* Path shape: NAME must contain at least 1 char, max form ALS 63. */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (*base == '\0' || strlen(base) >= 64)
        return -1;

    struct fat32_fs *fs = kmalloc(sizeof(*fs));
    if (!fs)
        return -1;
    memset(fs, 0, sizeof(*fs));
    fs->dev = dev;
    fs->bytes_per_sector = dev->sector_size;

    if (fat32_parse_geometry(fs) != 0) {
        printk("fat32: %s does not carry a FAT32 filesystem\n", dev->name);
        kfree(fs);
        return -1;
    }

    struct fat32_node *node = kmalloc(sizeof(*node));
    if (!node) {
        kfree(fs);
        return -1;
    }
    node->fs = fs;
    node->first_cluster = fs->root_cluster;
    node->size = 0;
    node->dir_chain_head = 0;
    node->entry_index = 0;

    if (vfs_lookup(path)) {
        printk("fat32: %s is already mounted\n", path);
        kfree(node);
        kfree(fs);
        return -1;
    }

    struct vfs_inode *in = vfs_alloc_inode(vfs_root(), base,
                                           VFS_MODE_DIR | 0755,
                                           &fat32_dir_ops, node);
    if (!in) {
        kfree(node);
        kfree(fs);
        return -1;
    }

    if (walk_directory(fs, fs->root_cluster, in, 0) != 0) {
        /* Keep the volume object alive (children reference it in their
         * private data); the tree may be partial. */
        printk("fat32: warning: %s volume read failed, partial mount\n",
               path);
        return -1;
    }

    printk("fat32: %s on %s: %u clusters x %u B, root cluster %u\n",
           path, dev->name, fs->total_clusters, fs->cluster_bytes,
           fs->root_cluster);
    return 0;
}

void fat32fs_init(void)
{
    block_dev_t *dev = ata_get_dev();
    if (!dev)
        return;                    /* no disk, no mount */
    if (fat32_mount_tree("/fat", dev) == 0)
        pr_info("fat32: /fat mounted from %s\n", dev->name);
}

/* --- inode_ops tables ------------------------------------------------------- */

static struct inode_ops fat32_file_ops = {
    .read     = fat32_file_read,
    .write    = fat32_file_write,
    .truncate = fat32_file_truncate,
    .destroy  = fat32_destroy,
};

static struct inode_ops fat32_dir_ops = {
    .create   = fat32_dir_create,
    .mkdir    = fat32_dir_mkdir,
    .destroy  = fat32_destroy,
};
