/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * ota — over-the-air firmware update for the rootfs partition (mtd1, 0x110000).
 *
 * The kernel and bootloader never change between our builds, so OTA only ever
 * rewrites the rootfs MTD partition. The config block (0xC000) and the HW MAC
 * (0x6000) live in mtd0 and are physically untouched, so updates preserve them
 * for free. Worst case (a write that fails midway) still falls back to the
 * bootloader TFTP recovery path — see docs/recovery.md.
 */
#ifndef CAMD_OTA_H
#define CAMD_OTA_H

#include <stddef.h>
#include <stdint.h>

/* Web upgrade image (*_upg.bin, tools/fwimage.py): 128-byte header "IC3115W  EDIMAX"
 * + zero padding, a 1 MiB kernel slot, the rootfs, and a trailing XOR-8 checksum
 * over everything between header and trailer. */
#define OTA_UPG_HEADER  "IC3115W  EDIMAX"
#define OTA_UPG_HDR_LEN 128u
#define OTA_KERNEL_SLOT 0x100000u

/* Largest image we will buffer: a full web upgrade image (header + kernel slot +
 * rootfs partition + trailer); a root.bin is smaller. */
#define OTA_MAX (OTA_UPG_HDR_LEN + OTA_KERNEL_SLOT + 0x2F0000u + 1u)

/*
 * Pure validator (no I/O; unit-tested on the host). Accepts a web upgrade image
 * (*_upg.bin; only its rootfs is flashed, the kernel is not updated), a cvimg
 * r6cr-headed root.bin, or a raw checksummed squashfs, as produced by
 * tools/fwimage.py. On success returns 0 and points *fs_out / *total_out at the
 * squashfs payload (r6cr header stripped) and its checksummed length — exactly
 * the bytes to burn at flash 0x110000. On failure returns -1 and sets *err to a
 * static message; the caller must NOT have erased anything yet.
 */
int ota_validate(const unsigned char *buf, size_t len,
                 const unsigned char **fs_out, uint32_t *total_out,
                 const char **err);

/*
 * Validate then burn the rootfs to /dev/mtdblock1 (this kernel has no MTD char
 * device): the block driver erases each block on write; we read back to verify.
 * Returns 0 on success, -1 on failure with *err set. A failure after the write
 * begins leaves the rootfs incomplete (recover via TFTP); *err says so.
 */
int ota_apply_rootfs(const unsigned char *buf, size_t len, const char **err);

/*
 * Core of ota_apply_rootfs, split out so it can be unit-tested against a regular
 * file. strict=1 enforces the on-device identity checks (right-sized partition
 * already holding a squashfs); strict=0 skips them for host tests.
 */
int ota_write_verify(const char *dev, int strict, const unsigned char *fs,
                     uint32_t total, const char **err);

/*
 * Reset the SoC using the bootloader's watchdog sequence (CDBR=0x2,
 * WDTCNR=0x100). ota_arm_reset() pre-maps /dev/mem (call it BEFORE flashing so
 * the reset path touches no filesystem); it returns 0 on success. ota_fire_reset
 * does not return. ota_reboot() arms then fires. Used instead of reboot(2)
 * because this kernel's machine_restart is a no-op (it hangs).
 */
int ota_arm_reset(void);
void ota_fire_reset(void);
void ota_reboot(void);

/* Raised by the OTA handler while an image is being validated/flashed, so
 * background workers (the uploader) can pause and leave CPU/flash to it. */
extern volatile int ota_in_progress;

#endif
