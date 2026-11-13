#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
"""Minimal read-only squashfs 4.0 (LZMA/zlib) extractor, no external dependencies.

usage: python3 -I unsquash.py IMAGE OFFSET OUTDIR
Device nodes are not created; they are listed in OUTDIR/../<name>.special.txt.
"""
import lzma
import os
import struct
import sys
import zlib


def decompress(comp, data):
    if comp == 1:
        return zlib.decompress(data)
    if comp == 2:
        return lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(data)
    raise SystemExit(f"unsupported compression id {comp}")


class SquashFS:
    def __init__(self, img, off):
        self.d = img[off:]
        (magic, self.inodes, _mtime, self.bs, self.nfrag, self.comp, _log, _flags,
         _nids, major, _minor, self.root, self.used, _ids, _xattr, itab, dtab, ftab,
         _lookup) = struct.unpack_from("<IIIIIHHHHHHQQQQQQQQ", self.d, 0)
        if magic != 0x73717368 or major != 4:
            raise SystemExit("not a squashfs 4 image")
        self.itab, self.dtab, self.ftab = itab, dtab, ftab
        self.inode_buf, self.inode_map = self.read_meta(itab, dtab)
        self.dir_buf, self.dir_map = self.read_meta(dtab, ftab)
        self.frags = self.read_frags()

    def read_meta(self, start, end):
        buf, mp, p = bytearray(), {}, start
        while p < end:
            mp[p - start] = len(buf)
            (h,) = struct.unpack_from("<H", self.d, p)
            n = h & 0x7FFF
            blk = self.d[p + 2:p + 2 + n]
            buf += blk if h & 0x8000 else decompress(self.comp, blk)
            p += 2 + n
        return bytes(buf), mp

    def read_frags(self):
        if not self.nfrag:
            return []
        nblk = (self.nfrag * 16 + 8191) // 8192
        ptrs = struct.unpack_from(f"<{nblk}Q", self.d, self.ftab)
        out = []
        for ptr in ptrs:
            (h,) = struct.unpack_from("<H", self.d, ptr)
            n = h & 0x7FFF
            blk = self.d[ptr + 2:ptr + 2 + n]
            raw = blk if h & 0x8000 else decompress(self.comp, blk)
            for i in range(0, len(raw), 16):
                out.append(struct.unpack_from("<QII", raw, i))
        return out[:self.nfrag]

    def block(self, pos, size):
        raw = self.d[pos:pos + (size & 0xFFFFFF)]
        return raw if size & 0x1000000 else decompress(self.comp, raw)

    def inode(self, ref):
        p = self.inode_map[ref >> 16] + (ref & 0xFFFF)
        t, mode, _uid, _gid, _mt, _ino = struct.unpack_from("<HHHHII", self.inode_buf, p)
        b, q = self.inode_buf, p + 16
        ino = {"type": t, "mode": mode}
        if t == 1:
            ino["start"], _, ino["size"], ino["off"], _ = struct.unpack_from("<IIHHI", b, q)
        elif t == 8:
            _, ino["size"], ino["start"], _, _, ino["off"], _ = struct.unpack_from("<IIIIHHI", b, q)
        elif t in (2, 9):
            if t == 2:
                st, fr, fo, sz = struct.unpack_from("<IIII", b, q)
                q += 16
            else:
                st, sz, _sp, _nl, fr, fo, _x = struct.unpack_from("<QQQIIII", b, q)
                q += 40
            nb = sz // self.bs if fr != 0xFFFFFFFF else (sz + self.bs - 1) // self.bs
            ino.update(start=st, frag=fr, foff=fo, size=sz,
                       blocks=struct.unpack_from(f"<{nb}I", b, q))
        elif t in (3, 10):
            _, n = struct.unpack_from("<II", b, q)
            ino["target"] = b[q + 8:q + 8 + n].decode("utf-8", "replace")
        elif t in (4, 5, 11, 12):
            _, ino["rdev"] = struct.unpack_from("<II", b, q)
        return ino

    def listdir(self, ino):
        p = self.dir_map[ino["start"]] + ino["off"]
        end, b, out = p + ino["size"] - 3, self.dir_buf, []
        while p < end:
            count, _start, inum = struct.unpack_from("<III", b, p)
            p += 12
            for _ in range(count + 1):
                off, _delta, _t, sz = struct.unpack_from("<HhHH", b, p)
                name = b[p + 8:p + 8 + sz + 1].decode("utf-8", "replace")
                out.append((name, _start, off))
                p += 8 + sz + 1
        return out

    def file_data(self, ino):
        out, pos = bytearray(), ino["start"]
        for sz in ino["blocks"]:
            out += self.block(pos, sz) if sz & 0xFFFFFF else bytes(self.bs)
            pos += sz & 0xFFFFFF
        if ino["frag"] != 0xFFFFFFFF:
            fpos, fsz, _ = self.frags[ino["frag"]]
            frag = self.block(fpos, fsz)
            out += frag[ino["foff"]:ino["foff"] + ino["size"] - len(out)]
        return bytes(out[:ino["size"]])


def extract(fs, ref, dest, rel, special):
    ino = fs.inode(ref)
    t = ino["type"]
    if t in (1, 8):
        os.makedirs(dest, exist_ok=True)
        for name, start, off in fs.listdir(ino):
            if name in (".", "..") or "/" in name or "\0" in name:
                raise SystemExit(f"refusing suspicious name {name!r} in {rel}")
            extract(fs, (start << 16) | off, os.path.join(dest, name), f"{rel}/{name}", special)
    elif t in (2, 9):
        with open(dest, "wb") as f:
            f.write(fs.file_data(ino))
        os.chmod(dest, ino["mode"] & 0o777)
    elif t in (3, 10):
        special.append(f"{rel} -> {ino['target']} (symlink)")
    else:
        special.append(f"{rel} type={t} mode={ino['mode']:o} rdev={ino.get('rdev')}")


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    img = open(sys.argv[1], "rb").read()
    fs = SquashFS(img, int(sys.argv[2], 0))
    special = []
    out = os.path.abspath(sys.argv[3])
    extract(fs, fs.root, out, "", special)
    with open(out.rstrip("/") + ".special.txt", "w") as f:
        f.write("\n".join(special) + "\n")
    print(f"extracted {fs.inodes} inodes to {out}; {len(special)} symlinks/devices listed")


if __name__ == "__main__":
    main()
