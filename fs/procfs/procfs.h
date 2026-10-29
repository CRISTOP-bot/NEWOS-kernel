/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/fs/procfs/procfs.h
 * Description: NEWOS filesystem interface
 * Added: 2026-10-17
 */
#ifndef PROCFS_H
#define PROCFS_H

#include <fs/vfs.h>

struct vfs_inode *procfs_mount(struct vfs_inode *parent);
void procfs_init(void);

#endif
