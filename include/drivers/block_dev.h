/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/include/drivers/block_dev.h
 * Description: NEWOS headers interface
 * Added: 2026-10-17
 */
#ifndef BLOCK_DEV_H
#define BLOCK_DEV_H

#include <core/core_types.h>

typedef struct block_dev block_dev_t;

struct block_dev {
    const char *name;
    u64 sector_count;
    u32 sector_size;
    int (*read_sectors)(block_dev_t *dev, u64 sector_start, u32 count, u8 *buffer);
    int (*write_sectors)(block_dev_t *dev, u64 sector_start, u32 count, const u8 *buffer);
};

int block_dev_read(block_dev_t *dev, u64 sector, u32 count, u8 *buffer);
int block_dev_write(block_dev_t *dev, u64 sector, u32 count, const u8 *buffer);

#endif
