/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/fs/iso9660.h
 * Description: ISO 9660 read-only filesystem (kernel/fs/iso9660/iso9660.c), mounted over
 * Added: 2026-10-22
 */
#ifndef FS_ISO9660_H
#define FS_ISO9660_H

/* ISO 9660 read-only filesystem (kernel/fs/iso9660/iso9660.c), mounted over
 * a block_dev_t. The primary volume descriptor (LBA 16) anchors the parser;
 * directory extents materialize plain VFS children at mount time, so
 * lookup/readdir stay in the VFS core. Read-only by construction: no
 * create/mkdir hooks, so vfs_unlink refuses its trees. Scope: 2048-byte
 * logical blocks, primary descriptor names (";1" stripped), continuation
 * (multi-extent) files up to 8 extents, max directory depth 8 (ISO level
 * cap). Rock Ridge, Joliet and El Torito are not parsed. */
#include <drivers/block_dev.h>
int iso9660_mount_tree(const char *path, block_dev_t *dev);
void iso9660fs_init(void);

#endif
