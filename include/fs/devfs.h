/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/fs/devfs.h
 * Description: 1 when the open file is /dev/serial (the merged console stdin).
 * Added: 2026-10-17
 */
#ifndef FS_DEVFS_H
#define FS_DEVFS_H

void devfs_init(void);

/* 1 when the open file is /dev/serial (the merged console stdin). */
struct vfs_file;
int  devfs_is_serial(const struct vfs_file *f);

#endif