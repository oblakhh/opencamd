/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * flash — guarded region access to an MTD device or a plain file (see flash.h).
 */

#include "flash.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Minimal MTD user ABI (matches linux 2.6.30 include/mtd/mtd-abi.h), declared
 * inline so we need no mtd-user.h on either the host or the cross toolchain. */
struct mtd_info_user {
	unsigned char type;
	unsigned int flags;
	unsigned int size;
	unsigned int erasesize;
	unsigned int writesize;
	unsigned int oobsize;
	unsigned long long padding;
};
struct erase_info_user { unsigned int start; unsigned int length; };
#define MEMGETINFO _IOR('M', 1, struct mtd_info_user)
#define MEMERASE   _IOW('M', 2, struct erase_info_user)

int flash_open(flash_region *r, const char *dev, uint32_t base, uint32_t size,
               uint32_t erasesize, uint32_t guard_min)
{
	struct mtd_info_user info;
	memset(r, 0, sizeof(*r));
	r->fd = open(dev, O_RDWR);
	if (r->fd < 0) {
		LOGE("flash: open %s: %s", dev, strerror(errno));
		return -1;
	}
	r->base = base;
	r->size = size;
	r->guard_min = guard_min;
	if (ioctl(r->fd, MEMGETINFO, &info) == 0) {
		r->is_mtd = 1;
		r->erasesize = info.erasesize;
		if (base + size > info.size) {
			LOGE("flash: region [%x,%x) exceeds mtd size %x", base, base + size, info.size);
			close(r->fd); r->fd = -1; return -1;
		}
		if (base % r->erasesize || size % r->erasesize)
			LOGW("flash: region base 0x%x/size 0x%x not aligned to erasesize 0x%x — "
			     "saves may fail", base, size, r->erasesize);
	} else {
		struct stat stt;
		r->is_mtd = 0;
		r->erasesize = erasesize ? erasesize : 4096;
		/* A block device (mtdblock*) has no MEMERASE; it erases on write. */
		if (fstat(r->fd, &stt) == 0 && S_ISBLK(stt.st_mode))
			r->is_block = 1;
	}
	return 0;
}

void flash_close(flash_region *r)
{
	if (r->fd >= 0)
		close(r->fd);
	r->fd = -1;
}

static int bounds_ok(flash_region *r, uint32_t off, uint32_t len)
{
	if (off > r->size || len > r->size - off) {
		LOGE("flash: access [%u,%u) out of region (size %u)", off, off + len, r->size);
		return 0;
	}
	if (r->base + off < r->guard_min) {
		LOGE("flash: REFUSING write below guard 0x%x (abs 0x%x)", r->guard_min, r->base + off);
		return 0;
	}
	return 1;
}

int flash_read(flash_region *r, uint32_t off, void *buf, uint32_t len)
{
	if (off > r->size || len > r->size - off)
		return -1;
	if (lseek(r->fd, r->base + off, SEEK_SET) < 0)
		return -1;
	{
		char *p = buf;
		uint32_t left = len;
		while (left) {
			ssize_t n = read(r->fd, p, left);
			if (n <= 0) return -1;
			p += n; left -= (uint32_t)n;
		}
	}
	return 0;
}

int flash_erase(flash_region *r, uint32_t off, uint32_t len)
{
	if (!bounds_ok(r, off, len))
		return -1;
	if (r->is_block)
		return 0;   /* mtdblock erases the affected sectors implicitly on write */
	if (r->is_mtd) {
		struct erase_info_user e;
		if ((r->base + off) % r->erasesize || len % r->erasesize) {
			LOGE("flash: erase [%x,+%x) not aligned to erasesize 0x%x",
			     r->base + off, len, r->erasesize);
			return -1;
		}
		e.start = r->base + off;
		e.length = len;
		if (ioctl(r->fd, MEMERASE, &e) != 0) {
			LOGE("flash: MEMERASE [%x,%x): %s", e.start, e.start + len, strerror(errno));
			return -1;
		}
	} else {
		/* file backend: erased flash reads as 0xFF */
		static unsigned char ff[4096];
		uint32_t left = len;
		memset(ff, 0xFF, sizeof(ff));
		if (lseek(r->fd, r->base + off, SEEK_SET) < 0) return -1;
		while (left) {
			uint32_t chunk = left < sizeof(ff) ? left : sizeof(ff);
			if (write(r->fd, ff, chunk) != (ssize_t)chunk) return -1;
			left -= chunk;
		}
	}
	return 0;
}

int flash_write(flash_region *r, uint32_t off, const void *buf, uint32_t len)
{
	if (!bounds_ok(r, off, len))
		return -1;
	if (lseek(r->fd, r->base + off, SEEK_SET) < 0)
		return -1;
	{
		const char *p = buf;
		uint32_t left = len;
		while (left) {
			ssize_t n = write(r->fd, p, left);
			if (n <= 0) return -1;
			p += n; left -= (uint32_t)n;
		}
	}
	if (r->is_block && fsync(r->fd) != 0)   /* commit mtdblock's cached sector */
		return -1;
	return 0;
}
