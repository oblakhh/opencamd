/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * Host unit tests for the OTA validator and the write/verify core (file-backed).
 */
#include "ota.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ROOTFS_SIZE 0x2F0000u

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static unsigned sum16(const unsigned char *d, unsigned n)
{
	unsigned s = 0, i;
	for (i = 0; i + 1 < n; i += 2)
		s += ((unsigned)d[i] << 8) | d[i + 1];
	if (n & 1)
		s += (unsigned)d[n - 1] << 8;
	return s & 0xFFFF;
}

static void put_be32(unsigned char *p, unsigned v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (unsigned char)v;
}
static void put_le16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put_le64(unsigned char *p, unsigned long long v) { int i; for (i = 0; i < 8; i++) { p[i] = (unsigned char)v; v >>= 8; } }

/* Build a raw checksummed squashfs-4.0/LZMA image of the given fs_len. */
static unsigned char *make_rootfs(unsigned fs_len, unsigned *total_out)
{
	unsigned total = fs_len + 640 + 2, i, partial, last;
	unsigned char *b = calloc(1, total);
	memcpy(b, "hsqs", 4);
	put_be32(b + 8, fs_len);     /* cvimg fs_len (BE) */
	put_le16(b + 20, 2);         /* compression = LZMA */
	put_le16(b + 28, 4);         /* s_major = 4 */
	put_le64(b + 40, fs_len);    /* bytes_used (<= total-2) */
	for (i = 48; i < fs_len; i++)
		b[i] = (unsigned char)(i * 7 + 1);
	partial = sum16(b, total - 2);
	last = (0x10000u - partial) & 0xFFFF;
	b[total - 2] = (unsigned char)(last >> 8);
	b[total - 1] = (unsigned char)last;
	*total_out = total;
	return b;
}

/* Recompute the trailing 2-byte checksum so sum16(b, total) == 0. */
static void fix_cksum(unsigned char *b, unsigned total)
{
	unsigned partial, last;
	b[total - 2] = b[total - 1] = 0;
	partial = sum16(b, total - 2);
	last = (0x10000u - partial) & 0xFFFF;
	b[total - 2] = (unsigned char)(last >> 8);
	b[total - 1] = (unsigned char)last;
}

static unsigned char *wrap_r6cr(const unsigned char *fs, unsigned total,
                                unsigned burn, unsigned declared, unsigned *out)
{
	unsigned char *b = malloc(16 + total);
	memcpy(b, "r6cr", 4);
	put_be32(b + 4, 0xF0000);
	put_be32(b + 8, burn);
	put_be32(b + 12, declared);
	memcpy(b + 16, fs, total);
	*out = 16 + total;
	return b;
}

/* Web upgrade image around a rootfs, as tools/fwimage.py pack builds it. */
static unsigned char *wrap_upg(const unsigned char *fs, unsigned total, unsigned *out)
{
	unsigned n = OTA_UPG_HDR_LEN + OTA_KERNEL_SLOT + total + 1, i;
	unsigned char *b = calloc(1, n), x = 0;
	memcpy(b, OTA_UPG_HEADER, sizeof(OTA_UPG_HEADER) - 1);
	memcpy(b + OTA_UPG_HDR_LEN, "cr6c", 4);
	memcpy(b + OTA_UPG_HDR_LEN + OTA_KERNEL_SLOT, fs, total);
	for (i = OTA_UPG_HDR_LEN; i < n - 1; i++)
		x ^= b[i];
	b[n - 1] = x;
	*out = n;
	return b;
}

int main(void)
{
	const unsigned char *fs;
	uint32_t total;
	const char *err;
	unsigned t, n;
	unsigned char *raw, *wrapped;

	/* 1. valid raw squashfs */
	raw = make_rootfs(0x40000, &t);
	err = (const char *)1;
	CHECK(ota_validate(raw, t, &fs, &total, &err) == 0);
	CHECK(total == t && fs == raw && err == NULL);

	/* 2. valid r6cr-wrapped */
	wrapped = wrap_r6cr(raw, t, 0x110000, t, &n);
	CHECK(ota_validate(wrapped, n, &fs, &total, &err) == 0);
	CHECK(fs == wrapped + 16 && total == t);
	free(wrapped);

	/* 2b. valid web upgrade image; then corrupted trailer, kernel block, padding */
	wrapped = wrap_upg(raw, t, &n);
	CHECK(ota_validate(wrapped, n, &fs, &total, &err) == 0);
	CHECK(fs == wrapped + OTA_UPG_HDR_LEN + OTA_KERNEL_SLOT && total == t);
	CHECK(n <= OTA_MAX);
	wrapped[n - 1] ^= 1;
	CHECK(ota_validate(wrapped, n, &fs, &total, &err) == -1);
	wrapped[n - 1] ^= 1;
	wrapped[OTA_UPG_HDR_LEN] = 'X';
	wrapped[n - 1] ^= 'X' ^ 'c';   /* keep the XOR valid: only the kernel check fails */
	CHECK(ota_validate(wrapped, n, &fs, &total, &err) == -1);
	free(wrapped);
	wrapped = wrap_upg(raw, t, &n);
	wrapped[20] = 1;
	CHECK(ota_validate(wrapped, n, &fs, &total, &err) == -1);
	wrapped[20] = 0;
	CHECK(ota_validate(wrapped, n - 5000, &fs, &total, &err) == -1);   /* truncated */
	free(wrapped);

	/* 3. r6cr wrong burn address */
	wrapped = wrap_r6cr(raw, t, 0x100000, t, &n);
	CHECK(ota_validate(wrapped, n, &fs, &total, &err) == -1);
	free(wrapped);

	/* 4. r6cr declared length disagrees */
	wrapped = wrap_r6cr(raw, t, 0x110000, t + 2, &n);
	CHECK(ota_validate(wrapped, n, &fs, &total, &err) == -1);
	free(wrapped);

	/* 5. bad magic */
	raw[0] = 'X';
	CHECK(ota_validate(raw, t, &fs, &total, &err) == -1);
	raw[0] = 'h';

	/* 6. corrupted body -> checksum mismatch, then restored */
	raw[100] ^= 0xFF;
	CHECK(ota_validate(raw, t, &fs, &total, &err) == -1);
	raw[100] ^= 0xFF;
	CHECK(ota_validate(raw, t, &fs, &total, &err) == 0);

	/* 7. truncated upload */
	CHECK(ota_validate(raw, t - 10, &fs, &total, &err) == -1);

	/* 8. trailing junk after total is accepted (pad the allocation) */
	{
		unsigned char *pad = malloc(t + 16);
		memcpy(pad, raw, t);
		memset(pad + t, 0xAB, 16);
		CHECK(ota_validate(pad, t + 16, &fs, &total, &err) == 0);
		CHECK(total == t);
		free(pad);
	}

	/* 9. 'sqsh' (big-endian) magic also accepted (re-checksum after the change) */
	memcpy(raw, "sqsh", 4); fix_cksum(raw, t);
	CHECK(ota_validate(raw, t, &fs, &total, &err) == 0);
	memcpy(raw, "hsqs", 4); fix_cksum(raw, t);

	/* 10. wrong compressor rejected (checksum fixed so the compressor check is reached) */
	put_le16(raw + 20, 1); fix_cksum(raw, t);   /* gzip */
	CHECK(ota_validate(raw, t, &fs, &total, &err) == -1);
	put_le16(raw + 20, 2); fix_cksum(raw, t);

	/* 11. wrong squashfs major rejected */
	put_le16(raw + 28, 3); fix_cksum(raw, t);
	CHECK(ota_validate(raw, t, &fs, &total, &err) == -1);
	put_le16(raw + 28, 4); fix_cksum(raw, t);
	CHECK(ota_validate(raw, t, &fs, &total, &err) == 0);  /* restored, still valid */
	free(raw);

	/* 12. implausibly small (fs_len < 0x40000) rejected */
	raw = make_rootfs(0x40000 - 2, &t);
	CHECK(ota_validate(raw, t, &fs, &total, &err) == -1);
	free(raw);

	/* 13. partition-size boundary: total == ROOTFS_SIZE ok, +2 rejected */
	raw = make_rootfs(ROOTFS_SIZE - 642, &t);           /* total == ROOTFS_SIZE */
	CHECK(t == ROOTFS_SIZE);
	CHECK(ota_validate(raw, t, &fs, &total, &err) == 0);
	free(raw);
	raw = make_rootfs(ROOTFS_SIZE - 640, &t);           /* total == ROOTFS_SIZE + 2 */
	CHECK(ota_validate(raw, t, &fs, &total, &err) == -1);
	free(raw);

	/* 14. absurd length field (u32 wrap guard) */
	{
		unsigned char small[700];
		memset(small, 0, sizeof(small));
		memcpy(small, "hsqs", 4);
		put_be32(small + 8, 0xFFFFFFF0u);
		CHECK(ota_validate(small, sizeof(small), &fs, &total, &err) == -1);
	}

	/* 15. too short to be anything */
	{
		unsigned char tiny[8] = { 'h', 's', 'q', 's', 0, 0, 0, 0 };
		CHECK(ota_validate(tiny, sizeof(tiny), &fs, &total, &err) == -1);
	}

	/* 16. bytes_used larger than the image is rejected */
	raw = make_rootfs(0x40000, &t);
	put_le64(raw + 40, (unsigned long long)t);   /* > total-2 */
	/* checksum now stale, but bytes_used check follows the checksum; recompute cksum */
	{
		unsigned partial, last;
		raw[t - 2] = raw[t - 1] = 0;
		partial = sum16(raw, t - 2);
		last = (0x10000u - partial) & 0xFFFF;
		raw[t - 2] = (unsigned char)(last >> 8);
		raw[t - 1] = (unsigned char)last;
	}
	CHECK(ota_validate(raw, t, &fs, &total, &err) == -1);
	free(raw);

	/* 17. write/verify core against a file (strict=0) round-trips */
	raw = make_rootfs(0x40000, &t);
	CHECK(ota_validate(raw, t, &fs, &total, &err) == 0);
	{
		char path[] = "/tmp/camd_ota_testXXXXXX";
		int fd = mkstemp(path);
		CHECK(fd >= 0);
		CHECK(ota_write_verify(path, 0, fs, total, &err) == 0);
		/* confirm the file actually holds the image */
		{
			unsigned char *back = malloc(total);
			lseek(fd, 0, SEEK_SET);
			CHECK((uint32_t)read(fd, back, total) == total);
			CHECK(memcmp(back, fs, total) == 0);
			free(back);
		}
		close(fd);
		unlink(path);
	}

	/* 18. strict identity: wrong-sized device rejected */
	{
		char path[] = "/tmp/camd_ota_badXXXXXX";
		int fd = mkstemp(path);
		CHECK(fd >= 0);
		CHECK(write(fd, "hsqs", 4) == 4);   /* tiny file, not ROOTFS_SIZE */
		close(fd);
		CHECK(ota_write_verify(path, 1, fs, total, &err) == -1);
		unlink(path);
	}
	free(raw);

	if (fails == 0)
		printf("test_ota: all OK\n");
	return fails ? 1 : 0;
}
