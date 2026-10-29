/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/fs/ext4/ext4_config.h
 * Description: NEWOS filesystem interface
 * Added: 2026-10-17
 */
#ifndef EXT4_CONFIG_H
#define EXT4_CONFIG_H

#define CONFIG_USE_DEFAULT_CFG 1
#define CONFIG_HAVE_OWN_ERRNO 1
#define CONFIG_DEBUG_PRINTF 0
#define CONFIG_DEBUG_ASSERT 0
#define CONFIG_BLOCK_DEV_CACHE_SIZE 16
#define CONFIG_BLOCK_DEV_ENABLE_STATS 0
#define CONFIG_EXT4_BLOCKDEVS_COUNT 16
#define CONFIG_EXT4_MOUNTPOINTS_COUNT 16
#define CONFIG_UNALIGNED_ACCESS 1
#define CONFIG_USE_USER_MALLOC 1

#endif
