#pragma once
#include <stdint.h>

/*
 * Pixel 2 XL (taimen) shim log channel on UFS: the shim writes its RAM log
 * into the `align_parti` partition (sda12, a 6 MiB all-zero GPT alignment
 * pad, backed up in ../backup/align_parti.img) instead of the tail of
 * userdata, which still carries the Android install. Read back from rooted
 * Android with tools/taimen/log-pull.sh. Nothing else on the device is
 * ever written.
 */
#define UFS_LOG_PARTITION	"align_parti"
#define UFS_SCRATCH_BLOCKS	64u	/* 256 KiB = LOG_SIZE, at block 0 of the partition */

int ufs_init(void);
int ufs_scratch_read(uint32_t idx, uint32_t count, void *buf);
int ufs_scratch_write(uint32_t idx, uint32_t count, const void *buf);
/* Write the RAM log (log.c) to the scratch area; returns 0 or the failing rc. */
int ufs_save_log(const char *why);
