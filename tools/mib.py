#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
"""Decode the Realtek/Edimax flash settings blocks (HW / default / current).

  python3 -I tools/mib.py FLASH.bin [--all]

Reads the three settings regions of a 4 MB dump (see docs/hardware.md):
  0x6000 hw setting ("COMPHS"), 0x8000 default ("COMPDS"), 0xC000 current ("COMPCS").
Each is either a raw param block ("H6"/"6A"/"6a" tag + u16 len + payload) or an
LZSS-compressed wrapper: COMPRESS_MIB_HEADER_T { char sig[6]; u16 compRate; u32 compLen; }
followed by LZSS data that expands to the raw param block. LZSS is Okumura's
(N=4096, F=18, THRESHOLD=2), from users/goahead-2.1.1/LINUX/utility.c Decode().

The expanded hw block is: signature[4] + u16 len, then HW_SETTING_T:
  boardVer u8, nic0Addr[6], nic1Addr[6], then per-WLAN wlan[] (wlan[0].macAddr[6] first).
Read-only.
"""
import struct
import sys

N, F, THRESHOLD = 4096, 18, 2


def lzss_decode(data):
    buf = bytearray(b" " * (N - F)) + bytearray(F)
    out = bytearray()
    r = N - F
    pos = 0
    flags = 0
    while pos < len(data):
        flags >>= 1
        if (flags & 256) == 0:
            if pos >= len(data):
                break
            flags = data[pos] | 0xFF00
            pos += 1
        if flags & 1:
            if pos >= len(data):
                break
            c = data[pos]; pos += 1
            out.append(c); buf[r] = c; r = (r + 1) & (N - 1)
        else:
            if pos + 1 >= len(data):
                break
            i = data[pos]; j = data[pos + 1]; pos += 2
            i |= (j & 0xF0) << 4
            j = (j & 0x0F) + THRESHOLD
            for k in range(j + 1):
                c = buf[(i + k) & (N - 1)]
                out.append(c); buf[r] = c; r = (r + 1) & (N - 1)
    return bytes(out)


def unwrap(blk):
    """Return the raw param block, decompressing the COMP wrapper if present."""
    if blk[:4] == b"COMP":
        comp_len = struct.unpack("<I", blk[6:10])[0]
        return lzss_decode(blk[12:12 + comp_len])
    return blk


def mac(b):
    return ":".join("%02x" % x for x in b)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    f = open(sys.argv[1], "rb").read()
    regions = [("hw", 0x6000), ("default", 0x8000), ("current", 0xC000)]
    for name, off in regions:
        blk = f[off:off + 0x2000]
        raw = unwrap(blk)
        tag = raw[:4]
        rlen = struct.unpack("<H", raw[4:6])[0] if len(raw) >= 6 else 0
        comp = " (COMP)" if blk[:4] == b"COMP" else ""
        print("%-8s @0x%05x%s tag=%r len=%d expanded=%d" % (name, off, comp, tag, rlen, len(raw)))
        if name == "hw":
            # The expanded block is a record stream; MAC records are id(1) + len 0x0006 (u16 BE) +
            # 6 bytes. id 0xC9 = eth0 (Edimax OUI), 0xCA/0xCB = WLAN (Realtek default). Scan for them.
            labels = {0xC9: "eth0", 0xCA: "wlan0", 0xCB: "wlan1"}
            for mid, lbl in labels.items():
                rec = bytes([mid]) + b"\x00\x06"
                k = raw.find(rec)
                if k >= 0:
                    print("    %-5s (id 0x%02x) = %s" % (lbl, mid, mac(raw[k + 3:k + 9])))
        if "--all" in sys.argv:
            import binascii
            print("    first 48 bytes:", binascii.hexlify(raw[:48]).decode())


if __name__ == "__main__":
    main()
