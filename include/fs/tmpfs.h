/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/fs/tmpfs.h
 * Description: tmpfs public API.
 * Added: 2026-10-17
 */
#ifndef FS_TMPFS_H
#define FS_TMPFS_H

#include <core/core_types.h>
#include <fs/vfs.h>

/* tmpfs public API. */
void tmpfs_init(void);
struct vfs_inode *tmpfs_create_node(struct vfs_inode *parent,
                                    const char *name, u32 mode);

/* True for inodes backed by tmpfs. The VFS uses this to keep synthetic
 * trees (devfs/procfs/sysfs) read-only with respect to unlink/rename. */
int tmpfs_is_node(const struct vfs_inode *inode);

#endif
