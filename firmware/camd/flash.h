/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * flash — a tiny region abstraction over an MTD char device (on the camera) or
 * a plain file (for host tests). Offsets passed to read/erase/write are relative
 * to the region base. A hard guard refuses any access whose ABSOLUTE offset is
 * below guard_min, so the config store can never touch the bootloader (0x0) or
 * the HW settings / MAC at 0x6000.
 */
#ifndef CAMD_FLASH_H
#define CAMD_FLASH_H

#include <stdint.h>

typedef struct {
	int fd;
	int is_mtd;          /* MTD char device (MEMERASE) */
	int is_block;        /* block device (e.g. mtdblock*): erases-on-write */
	uint32_t base;       /* absolute base offset within the device */
	uint32_t size;       /* region length */
	uint32_t erasesize;  /* erase block size (bytes) */
	uint32_t guard_min;  /* refuse any absolute offset < this */
} flash_region;

/* Open a region. dev may be an MTD char device (e.g. /dev/mtd0) or a regular
 * file. Returns 0 on success. */
int flash_open(flash_region *r, const char *dev, uint32_t base, uint32_t size,
               uint32_t erasesize, uint32_t guard_min);
void flash_close(flash_region *r);

/* All offsets are relative to base and bounded by size. Return 0 on success. */
int flash_read(flash_region *r, uint32_t off, void *buf, uint32_t len);
int flash_erase(flash_region *r, uint32_t off, uint32_t len);  /* len multiple of erasesize */
int flash_write(flash_region *r, uint32_t off, const void *buf, uint32_t len);

#endif
