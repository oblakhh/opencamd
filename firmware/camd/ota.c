/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * ota — validate and write a firmware upgrade image to the rootfs
 * partition (see ota.h).
 */

#include "ota.h"
#include "log.h"

volatile int ota_in_progress = 0;

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * rootfs partition: /proc/mtd mtd1 "root fs", 0x110000..0x400000. This kernel
 * has CONFIG_MTD_CHAR off (no /dev/mtdN, no MEMERASE), so — like the vendor's
 * own flash tool — we write the block device /dev/mtdblock1, which erases each
 * block on write. The kernel boots root=/dev/mtdblock1 from exactly this.
 */
#define ROOTFS_DEV   "/dev/mtdblock1"
#define ROOTFS_SIZE  0x2F0000u          /* 0x400000 - 0x110000 = 3080192 */
#define SQFS_SB      640u               /* cvimg SIZE_OF_SQFS_SUPER_BLOCK */
#define R6CR_BURN    0x110000u
#define SQFS_MIN     0x40000u           /* a real stripped rootfs is far bigger */
#define SQFS_COMP_LZMA 2                /* squashfs_fs.h LZMA_COMPRESSION */

/* Big-endian 16-bit word sum (cvimg calculateChecksum); odd length padded 0. */
static unsigned sum16(const unsigned char *d, uint32_t n)
{
	unsigned s = 0;
	uint32_t i;
	for (i = 0; i + 1 < n; i += 2)
		s += ((unsigned)d[i] << 8) | d[i + 1];
	if (n & 1)
		s += (unsigned)d[n - 1] << 8;
	return s & 0xFFFF;
}

static unsigned be32(const unsigned char *p)
{
	return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
	       ((unsigned)p[2] << 8) | (unsigned)p[3];
}
static unsigned le16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }

/* bytes_used: squashfs 4.0 superblock, little-endian u64 at +40. */
static unsigned long long le64(const unsigned char *p)
{
	unsigned long long v = 0;
	int i;
	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

int ota_validate(const unsigned char *buf, size_t len,
                 const unsigned char **fs_out, uint32_t *total_out,
                 const char **err)
{
	const unsigned char *fs = buf;
	size_t fslen = len;
	uint32_t declared = 0, total, fs_len_field;
	unsigned long long used;
	int headed = 0;

	*err = NULL;
	if (len < 16) { *err = "image too short"; return -1; }

	/* Web upgrade image: verify the container, then validate its rootfs. The kernel
	 * slot is checked for a kernel signature but not flashed (camd runs the stock
	 * kernel and only ever replaces the rootfs). */
	if (len > sizeof(OTA_UPG_HEADER) - 1 &&
	    memcmp(buf, OTA_UPG_HEADER, sizeof(OTA_UPG_HEADER) - 1) == 0) {
		size_t i;
		unsigned char x = 0;
		if (len < OTA_UPG_HDR_LEN + OTA_KERNEL_SLOT + SQFS_SB + 2 + 1) {
			*err = "upgrade image too short"; return -1;
		}
		for (i = sizeof(OTA_UPG_HEADER) - 1; i < OTA_UPG_HDR_LEN; i++)
			if (buf[i]) { *err = "upgrade image header padding is not zero"; return -1; }
		for (i = OTA_UPG_HDR_LEN; i < len - 1; i++)
			x ^= buf[i];
		if (x != buf[len - 1]) { *err = "upgrade image checksum mismatch"; return -1; }
		if (memcmp(buf + OTA_UPG_HDR_LEN, "cr6c", 4) != 0 &&
		    memcmp(buf + OTA_UPG_HDR_LEN, "cs6c", 4) != 0) {
			*err = "upgrade image has no kernel block"; return -1;
		}
		fs = buf + OTA_UPG_HDR_LEN + OTA_KERNEL_SLOT;
		fslen = len - 1 - OTA_UPG_HDR_LEN - OTA_KERNEL_SLOT;
	}
	/* Optional 16-byte r6cr transport header (cvimg root.bin). */
	else if (memcmp(buf, "r6cr", 4) == 0) {
		uint32_t burn = be32(buf + 8);
		declared = be32(buf + 12);
		if (burn != R6CR_BURN) { *err = "r6cr burnAddr is not 0x110000"; return -1; }
		fs = buf + 16;
		fslen = len - 16;
		headed = 1;
	}

	if (fslen < SQFS_SB + 2) { *err = "payload shorter than a squashfs"; return -1; }
	/* squashfs 4.0 magic: 'hsqs' (LE host) or 'sqsh' (BE). */
	if (memcmp(fs, "hsqs", 4) != 0 && memcmp(fs, "sqsh", 4) != 0) {
		*err = "no squashfs magic at rootfs start";
		return -1;
	}

	fs_len_field = be32(fs + 8);            /* cvimg stores fs_len at +8 (BE) */
	if (fs_len_field > ROOTFS_SIZE) { *err = "rootfs length field absurd"; return -1; }
	if (fs_len_field < SQFS_MIN)    { *err = "rootfs implausibly small"; return -1; }
	total = fs_len_field + SQFS_SB + 2;
	if (total > ROOTFS_SIZE) { *err = "rootfs larger than the mtd1 partition"; return -1; }
	if (total > fslen) { *err = "rootfs length field runs past the upload"; return -1; }
	if (sum16(fs, total) != 0) { *err = "rootfs checksum mismatch"; return -1; }
	if (headed && declared != total) { *err = "r6cr length disagrees with payload"; return -1; }

	/* Structural identity: a Realtek-LZMA squashfs 4.0 (native fields are LE and
	 * untouched by cvimg). Rejects wrong-compressor / v3 images that would pass
	 * the word-sum yet fail to mount. */
	if (le16(fs + 20) != SQFS_COMP_LZMA) { *err = "rootfs is not LZMA-compressed"; return -1; }
	if (le16(fs + 28) != 4) { *err = "rootfs is not squashfs 4.0"; return -1; }
	used = le64(fs + 40);
	if (used == 0 || used > (unsigned long long)total - 2) { *err = "squashfs bytes_used implausible"; return -1; }

	*fs_out = fs;
	*total_out = total;
	return 0;
}

/* mtdblock is a BLOCK device, so its fallback node is S_IFBLK. */
static const char *blk_node(const char *canonical, const char *fallback,
                            unsigned maj, unsigned min)
{
	if (access(canonical, F_OK) == 0)
		return canonical;
	unlink(fallback);
	if (mknod(fallback, S_IFBLK | 0600, makedev(maj, min)) == 0 || errno == EEXIST)
		return fallback;
	LOGW("ota: mknod %s: %s", fallback, strerror(errno));
	return canonical;
}

static int write_all(int fd, const unsigned char *p, uint32_t n)
{
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w <= 0) return -1;
		p += w; n -= (uint32_t)w;
	}
	return 0;
}
static int read_all(int fd, unsigned char *p, uint32_t n)
{
	while (n) {
		ssize_t r = read(fd, p, n);
		if (r <= 0) return -1;
		p += r; n -= (uint32_t)r;
	}
	return 0;
}

/*
 * Core, testable on a regular file. Opens `dev`, (when strict) checks it is the
 * right-sized partition currently holding a squashfs, writes `total` bytes, and
 * reads them back to verify.
 *
 * NB: we do NOT mlockall here. On this 32 MB box locking the 6 MB frame pool +
 * 3 MB image creates heavy memory pressure that makes the write/verify crawl
 * (minutes). There is no swap, so camd's anonymous pages are never evicted; only
 * file-backed squashfs text could be, and with this much free RAM it isn't. We
 * then reboot immediately, keeping the live-rootfs-rewrite window short.
 */
int ota_write_verify(const char *dev, int strict, const unsigned char *fs,
                     uint32_t total, const char **err)
{
	unsigned char rb[4096];
	off_t devsz;
	uint32_t off;
	int fd;

	fd = open(dev, O_RDWR);
	if (fd < 0) { *err = "cannot open rootfs device"; return -1; }

	if (strict) {
		devsz = lseek(fd, 0, SEEK_END);
		if (devsz != (off_t)ROOTFS_SIZE) {
			close(fd);
			*err = "rootfs device is not the 0x2F0000 partition";
			return -1;
		}
		if (lseek(fd, 0, SEEK_SET) != 0 || read_all(fd, rb, 4) != 0 ||
		    (memcmp(rb, "hsqs", 4) != 0 && memcmp(rb, "sqsh", 4) != 0)) {
			close(fd);
			*err = "rootfs device does not currently hold a squashfs";
			return -1;
		}
	}

	if (lseek(fd, 0, SEEK_SET) != 0 || write_all(fd, fs, total) != 0) {
		close(fd);
		*err = "flash write failed — device needs TFTP recovery";
		return -1;
	}
	if (fsync(fd) != 0) {
		close(fd);
		*err = "flash fsync failed — device needs TFTP recovery";
		return -1;
	}

	/* Read-back verify. We deliberately do NOT drop the block cache (BLKFLSBUF)
	 * here: it would evict the mounted squashfs's pages and destabilise the live
	 * system before the reboot. This catches truncation / short writes; the
	 * mtdblock+NOR write path itself returns -EIO on a real program failure. */
	if (lseek(fd, 0, SEEK_SET) != 0) {
		close(fd);
		*err = "seek failed"; return -1;
	}
	for (off = 0; off < total; off += sizeof(rb)) {
		uint32_t chunk = total - off < sizeof(rb) ? total - off : (uint32_t)sizeof(rb);
		if (read_all(fd, rb, chunk) != 0) {
			close(fd);
			*err = "flash readback failed"; return -1;
		}
		if (memcmp(rb, fs + off, chunk) != 0) {
			close(fd);
			*err = "flash verify mismatch — device needs TFTP recovery";
			return -1;
		}
	}
	close(fd);
	LOGI("ota: rootfs flashed and verified (%u bytes)", total);
	return 0;
}

int ota_apply_rootfs(const unsigned char *buf, size_t len, const char **err)
{
	const unsigned char *fs;
	uint32_t total;
	const char *dev;

	if (ota_validate(buf, len, &fs, &total, err) != 0)
		return -1;                       /* nothing written — safe to retry */
	dev = blk_node(ROOTFS_DEV, "/var/ota-mtdblock1", 31, 1);
	return ota_write_verify(dev, 1, fs, total, err);
}

/* ---- SoC reset ------------------------------------------------------------
 *
 * Use the reboot(2) syscall (RB_AUTOBOOT), which calls the kernel's own
 * machine_restart — the stock kernel resets cleanly this way (the vendor's
 * /bin/reboot relies on it). We call the syscall DIRECTLY rather than going
 * through busybox `reboot`/init, whose shutdown (umount -a -r of the squashfs
 * root) is what used to hang. An earlier attempt poked the watchdog CDBR
 * register, but CDBR is the live kernel timer divider, so it froze the system
 * instead of resetting — never touch it from a running kernel.
 */
int ota_arm_reset(void)
{
	return 0;   /* nothing to pre-arm for reboot(2) */
}

void ota_fire_reset(void)
{
	sync();
	sync();
	reboot(RB_AUTOBOOT);           /* should not return */
	LOGE("ota: reboot(2) returned (%s) — power-cycle required", strerror(errno));
}

void ota_reboot(void)
{
	ota_fire_reset();
}
