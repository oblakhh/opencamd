#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
"""Reduce the unpacked stock rootfs to an explicit allow list.

The image is built on top of the vendor's rootfs (busybox, uClibc, device
nodes). Everything we do not name here is removed — vendor applications,
assets, scripts and Edimax's cloud client key in /etc/ssl — so nothing
proprietary or secret can end up in a redistributed image by accident.
Our own files (camd, UI, init scripts) are added afterwards by build_image.sh.

Usage: prune_rootfs.py <rootfs-dir>
"""
import os
import re
import sys

# Regular files we keep from the stock rootfs (GPL/LGPL components only).
KEEP_FILES = re.compile(r"""^(
      bin/(busybox|udhcpc|iwpriv)          # busybox (GPL), udhcp (GPL), wireless-tools (GPL)
    | lib/[^/]+-0\.9\.30\.so               # uClibc 0.9.30 (LGPL)
    | lib/libgcc_s_4181\.so\.1             # libgcc (GPL + runtime exception)
    | etc/(hosts|host\.conf)               # trivial resolver config
)$""", re.X)

# Absolute symlinks into the ramfs we keep; the rest pointed at vendor state.
KEEP_ABS_LINKS = {"tmp", "dev/log", "etc/TZ"}


def main(root):
    removed = kept = 0
    # 1. Regular files and absolute symlinks.
    for dirpath, dirnames, filenames in os.walk(root):
        for name in filenames + dirnames:
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, root)
            if os.path.islink(path):
                if os.readlink(path).startswith("/") and rel not in KEEP_ABS_LINKS:
                    os.unlink(path)
                    removed += 1
                continue
            if name in dirnames:
                continue
            if rel.startswith("dev/") or not os.path.isfile(path):
                continue                      # device nodes, fifos
            if KEEP_FILES.match(rel):
                kept += 1
            else:
                os.unlink(path)
                removed += 1
    # 2. Relative symlinks whose target is gone (links to removed tools/libs).
    for dirpath, dirnames, filenames in os.walk(root):
        for name in filenames + dirnames:
            path = os.path.join(dirpath, name)
            if os.path.islink(path):
                target = os.readlink(path)
                if not target.startswith("/") and \
                        not os.path.lexists(os.path.join(dirpath, target)):
                    os.unlink(path)
                    removed += 1
    # 3. Empty nested directories (top-level ones stay: mount points, /var, ...).
    for dirpath, dirnames, filenames in os.walk(root, topdown=False):
        rel = os.path.relpath(dirpath, root)
        if rel != "." and os.sep in rel and not rel.startswith("dev") \
                and not os.path.islink(dirpath) and not os.listdir(dirpath):
            os.rmdir(dirpath)
    print("prune_rootfs: kept %d stock files, removed %d entries" % (kept, removed))


if __name__ == "__main__":
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        sys.exit("usage: prune_rootfs.py <rootfs-dir>")
    main(sys.argv[1])
