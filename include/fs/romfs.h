/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/fs/romfs.h
 * Description: romfs v1 read-only filesystem (kernel/fs/romfs/romfs.c), mounted over a
 * Added: 2026-10-22
 */
#ifndef FS_ROMFS_H
#define FS_ROMFS_H

/* romfs v1 read-only filesystem (kernel/fs/romfs/romfs.c), mounted over a
 * block_dev_t. Format per the classic romfs 1.0 definition ("-rom1fs-"
 * magic, 16-byte-aligned inode headers, next/type in the low nibble of
 * the first header word, additive checksum over the first 512 bytes that
 * must sum to zero). Directory chains materialize plain VFS children at
 * mount time; lookup/readdir stay in the VFS core. Read-only by design:
 * no create/mkdir hooks, so vfs_unlink refuses the tree. Scope: hard
 * links to files/dirs are materialized under the link name; symlinks and
 * device/socket/FIFO entries are skipped (NEWOS VFS has no symlinks). */
#include <drivers/block_dev.h>
int romfs_mount_tree(const char *path, block_dev_t *dev);
void romfsfs_init(void);

#endif
