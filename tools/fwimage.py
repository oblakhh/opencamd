#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
"""IC-3115W upgrade image (*_upg.bin) verifier and packer. No dependencies; read-only unless packing.

  python3 -I tools/fwimage.py verify IMAGE
  python3 -I tools/fwimage.py pack --kernel linux.bin --rootfs root.bin|rootfs.squashfs -o OUT
  python3 -I tools/fwimage.py split IMAGE OUTDIR   # linux.bin + root.bin for bootloader TFTP recovery

Format (image/mkImage.sh, AppendChecksum.c, cvimg.c, bootcode boot/init/utility.c):
  0x000000  128-byte header: b"IC3115W  EDIMAX", zero padded
  0x000080  kernel block (linux.bin), zero padded to 1 MiB. 16-byte Realtek header
            (signature "cr6c" = kernel with separate rootfs, startAddr, burnAddr, len; big-endian),
            then `len` bytes whose big-endian 16-bit word sum is 0.
  0x100080  rootfs: squashfs whose superblock word at +8 (normally mkfs_time) holds fs_len, followed
            by a 2-byte checksum; the first fs_len + 640 + 2 bytes have a 16-bit BE word sum of 0.
            (root.bin from cvimg has a 16-byte "r6cr" header that mkImage.sh strips.)
  last byte XOR of every byte after the 128-byte header.
Everything from 0x80 on is written to flash at 0x10000 (kernel 0x10000, rootfs 0x110000).
"""
import argparse
import struct
import sys

HEADER = b"IC3115W  EDIMAX"
HEADER_LEN = 128
KERNEL_SLOT = 0x100000          # mkImage.sh: kernel max 1 MiB
ROOTFS_MAX = 3080192            # mkImage.sh: 0x2F0000, 0x110000..0x400000
KERNEL_SIGS = (b"cr6c", b"cs6c")
SQFS_SB = 640                   # SIZE_OF_SQFS_SUPER_BLOCK in cvimg.c / utility.c


def sum16(data):
    """Big-endian 16-bit word sum, odd length padded with a zero byte (cvimg calculateChecksum)."""
    if len(data) % 2:
        data += b"\0"
    return sum(struct.unpack(">%dH" % (len(data) // 2), data)) & 0xFFFF


def xor8(data):
    x = 0
    for b in data:
        x ^= b
    return x


def check_kernel(blk):
    """Return (errors, info) for a kernel block starting with the 16-byte Realtek header."""
    errs = []
    sig, start, burn, length = struct.unpack(">4sIII", blk[:16])
    info = "sig=%s start=0x%08x burn=0x%x len=%d" % (sig.decode("latin1"), start, burn, length)
    if sig not in KERNEL_SIGS:
        errs.append("kernel signature %r, expected one of %r" % (sig, KERNEL_SIGS))
    if burn != 0x10000:
        errs.append("kernel burnAddr 0x%x, expected 0x10000" % burn)
    if 16 + length > KERNEL_SLOT:
        errs.append("kernel %d bytes does not fit the 1 MiB slot" % (16 + length))
    elif sum16(blk[16:16 + length]):
        errs.append("kernel checksum: word sum 0x%04x, expected 0" % sum16(blk[16:16 + length]))
    return errs, info


def check_rootfs(fs):
    """Return (errors, info, total_len) for a rootfs as it sits in flash (no r6cr header)."""
    errs = []
    if fs[:4] not in (b"hsqs", b"sqsh"):
        return ["no squashfs magic at rootfs start"], "", 0
    fs_len = struct.unpack(">I", fs[8:12])[0]
    total = fs_len + SQFS_SB + 2
    used = struct.unpack("<Q", fs[40:48])[0]  # squashfs 4 bytes_used (little-endian)
    info = "squashfs bytes_used=%d fs_len=%d total=%d" % (used, fs_len, total)
    if total > ROOTFS_MAX:
        errs.append("rootfs %d bytes exceeds the %d-byte slot" % (total, ROOTFS_MAX))
    if total > len(fs):
        errs.append("rootfs length field (%d) runs past the image end" % total)
    elif sum16(fs[:total]):
        errs.append("rootfs checksum: word sum 0x%04x, expected 0" % sum16(fs[:total]))
    if used > total - 2:
        errs.append("squashfs bytes_used %d larger than the checksummed area" % used)
    return errs, info, total


def verify(path):
    img = open(path, "rb").read()
    errs = []
    print("%s: %d bytes" % (path, len(img)))
    if not img.startswith(HEADER) or any(img[len(HEADER):HEADER_LEN]):
        errs.append("header is not %r + zero padding" % HEADER)
    if len(img) < HEADER_LEN + KERNEL_SLOT + SQFS_SB:
        errs.append("image too short")
        return report(errs)
    want = xor8(img[HEADER_LEN:-1])
    print("  trailer xor: 0x%02x (computed 0x%02x)" % (img[-1], want))
    if img[-1] != want:
        errs.append("trailing XOR checksum mismatch")
    e, info = check_kernel(img[HEADER_LEN:HEADER_LEN + KERNEL_SLOT])
    print("  kernel: " + info)
    errs += e
    e, info, total = check_rootfs(img[HEADER_LEN + KERNEL_SLOT:-1])
    print("  rootfs: " + info)
    errs += e
    extra = len(img) - 1 - (HEADER_LEN + KERNEL_SLOT + total)
    if extra:
        errs.append("%d unexpected bytes between rootfs and trailer" % extra)
    return report(errs)


def report(errs):
    for e in errs:
        print("  ERROR: " + e)
    print("  " + ("OK" if not errs else "FAILED"))
    return 1 if errs else 0


def wrap_squashfs(fs):
    """cvimg 'root': store fs_len at +8, append the 16-bit checksum (pad to even length first)."""
    if len(fs) % 2:
        fs += b"\0"
    fs = bytearray(fs)
    fs[8:12] = struct.pack(">I", len(fs) - SQFS_SB)
    fs += struct.pack(">H", (-sum16(bytes(fs))) & 0xFFFF)
    return bytes(fs)


def pack(kernel_path, rootfs_path, out):
    kernel = open(kernel_path, "rb").read()
    rootfs = open(rootfs_path, "rb").read()
    e, _ = check_kernel(kernel + b"\0" * max(0, KERNEL_SLOT - len(kernel)))
    if e:
        sys.exit("kernel: " + "; ".join(e))
    if rootfs[:4] == b"r6cr":
        rootfs = rootfs[16:]
    elif rootfs[:4] in (b"hsqs", b"sqsh"):
        rootfs = wrap_squashfs(rootfs)
    else:
        sys.exit("rootfs: neither an r6cr root.bin nor a squashfs image")
    e, _, total = check_rootfs(rootfs)
    if e:
        sys.exit("rootfs: " + "; ".join(e))
    body = kernel.ljust(KERNEL_SLOT, b"\0") + rootfs[:total]
    img = HEADER.ljust(HEADER_LEN, b"\0") + body + bytes([xor8(body)])
    open(out, "wb").write(img)
    return verify(out)


def split(path, outdir):
    """Recreate the cvimg blocks the bootloader's TFTP download mode accepts."""
    import os
    if verify(path):
        sys.exit("refusing to split an image that fails verification")
    img = open(path, "rb").read()
    kblk = img[HEADER_LEN:HEADER_LEN + KERNEL_SLOT]
    klen = struct.unpack(">I", kblk[12:16])[0]
    fs = img[HEADER_LEN + KERNEL_SLOT:-1]
    _, _, total = check_rootfs(fs)
    os.makedirs(outdir, exist_ok=True)
    open(os.path.join(outdir, "linux.bin"), "wb").write(kblk[:16 + klen])
    # cvimg root: startAddr 0xF0000 (as boards/rtl8196c/tools/mkimg passes it), burnAddr 0x110000.
    root = struct.pack(">4sIII", b"r6cr", 0xF0000, 0x110000, total) + fs[:total]
    open(os.path.join(outdir, "root.bin"), "wb").write(root)
    print("wrote %s/linux.bin (%d bytes) and root.bin (%d bytes)" % (outdir, 16 + klen, len(root)))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("verify", help="check an upgrade image")
    v.add_argument("image")
    p = sub.add_parser("pack", help="build an upgrade image")
    p.add_argument("--kernel", required=True, help="linux.bin (with cr6c header)")
    p.add_argument("--rootfs", required=True, help="root.bin (r6cr) or a raw squashfs image")
    p.add_argument("-o", "--out", required=True)
    s = sub.add_parser("split", help="extract linux.bin and root.bin (bootloader TFTP format)")
    s.add_argument("image")
    s.add_argument("outdir")
    a = ap.parse_args()
    if a.cmd == "verify":
        sys.exit(verify(a.image))
    if a.cmd == "pack":
        sys.exit(pack(a.kernel, a.rootfs, a.out))
    sys.exit(split(a.image, a.outdir))


if __name__ == "__main__":
    main()
