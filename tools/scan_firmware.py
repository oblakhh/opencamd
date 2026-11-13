#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
"""Read-only scan of a firmware image for compressed/filesystem signatures."""
import struct
import sys

SIGS = {
    b"hsqs": "squashfs (little-endian)",
    b"sqsh": "squashfs (big-endian)",
    b"\x1f\x8b\x08": "gzip",
    b"\xfd7zXZ\x00": "xz",
}


def lzma_candidate(d, i):
    # LZMA alone header: props byte 0x5d, dict size (u32 LE), uncompressed size (u64 LE)
    if d[i] != 0x5D or i + 13 > len(d):
        return False
    dict_size = struct.unpack_from("<I", d, i + 1)[0]
    usize = struct.unpack_from("<Q", d, i + 5)[0]
    return dict_size in (1 << n for n in range(16, 28)) and 0 < usize < 64 * 1024 * 1024


def main(path):
    d = open(path, "rb").read()
    print(f"{path}: {len(d)} bytes, header {d[:16]!r}")
    for i in range(len(d)):
        for sig, name in SIGS.items():
            if d.startswith(sig, i):
                print(f"0x{i:08x} ({i}): {name}")
        if lzma_candidate(d, i):
            print(f"0x{i:08x} ({i}): LZMA stream candidate")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} IMAGE")
    main(sys.argv[1])
