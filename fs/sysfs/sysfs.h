/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/fs/sysfs/sysfs.h
 * Description: NEWOS filesystem interface
 * Added: 2026-10-17
 */
#ifndef SYSFS_H
#define SYSFS_H

#include <fs/vfs.h>

struct vfs_inode *sysfs_mount(struct vfs_inode *parent);
void sysfs_init(void);

#endif
