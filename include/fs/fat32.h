/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/fs/fat32.h
 * Description: FAT32 read-write filesystem (kernel/fs/fat32/fat32fs.c), mounted over a
 * Added: 2026-10-22
 */
#ifndef FS_FAT32_H
#define FS_FAT32_H

/* FAT32 read-write filesystem (kernel/fs/fat32/fat32fs.c), mounted over a
 * block_dev_t. fat32fs_init() attaches the primary-master ATA disk under
 * /fat when one is present; fat32_mount_tree() is the generic entry point
 * (selftests and future devices). Directories materialize regular VFS
 * children when they are mounted, so lookup/readdir stay in the VFS core;
 * create/mkdir/unlink route through the inode_ops hooks and hit the disk.
 * Names are 8.3 on the device (long-name entries are foreign artifacts:
 * backslashes in the alias are ignored, short name is empty). */
#include <drivers/block_dev.h>
int fat32_mount_tree(const char *path, block_dev_t *dev);
void fat32fs_init(void);

#endif
