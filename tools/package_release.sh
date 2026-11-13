#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
#
# Release artifacts, after tools/build_all.sh, inside the camd build image:
#   docker compose run --rm build tools/package_release.sh <label>
# <label> is the version as it appears in file names, e.g. 0.5.0 or 0.5.0-dev.20261111183000.gabc1234.
# Writes dist/:
#   camd-<label>-upg.bin          web UI / OTA upgrade image
#   camd-<label>-tftp-linux.bin   TFTP recovery: kernel block   (docs/recovery.md)
#   camd-<label>-tftp-root.bin    TFTP recovery: rootfs block
#   ic3115w-gpl-sources.tar.xz    source of the vendor GPL/LGPL components in the image (kernel,
#                                 busybox, udhcp, wireless tools, uClibc, squashfs/lzma) — the
#                                 pruned Edimax GPL release we build against, without binaries
# The source of camd itself is packaged separately by tools/source_archive.sh (needs git).
set -e
cd /work
label=${1:?usage: package_release.sh <label>}
[ -f build/image/IC3115W_camd_upg.bin ] || { echo "no firmware image - run tools/build_all.sh first"; exit 1; }
[ -n "$IC3115W_GPL" ] || { echo "run inside the camd build image (IC3115W_GPL not set)"; exit 1; }
owner=$(stat -c %u:%g /work)
trap 'chown -R "$owner" dist 2>/dev/null || true' EXIT

rm -rf dist && mkdir -p dist
cp build/image/IC3115W_camd_upg.bin "dist/camd-$label-upg.bin"
python3 -I tools/fwimage.py split build/image/IC3115W_camd_upg.bin build/image/tftp > /dev/null
cp build/image/tftp/linux.bin "dist/camd-$label-tftp-linux.bin"
cp build/image/tftp/root.bin  "dist/camd-$label-tftp-root.bin"

# GPL sources: no toolchain binaries (we don't redistribute the toolchain; its uClibc source
# under config/uclibc stays), no build outputs.
tc=rtl819x-sdk-v2.3/rtl819x/toolchain/rsdk-1.3.6-4181-EB-2.6.30-0.9.30
cat > /tmp/GPL-SOURCES.txt <<TXT
Source code of the third-party GPL/LGPL components contained in opencamd firmware images for the
Edimax IC-3115W: the Linux kernel (unchanged stock kernel), busybox, udhcp, wireless tools
(iwpriv) and uClibc (under $tc/config/uclibc), plus the squashfs/LZMA tools.

This is the Edimax GPL source release IC3115W_EDIMAX_20140901.tar.gz
(SHA-256 25b209c7023ab1d8813c50e4ca0ac1f8195bdc26f464771545130c6c03e68f8f, from
https://www.edimax.com, IC-3115W downloads), reduced by docker/prepare-sdk.sh of the opencamd
repository to the components above, with the build fixes for modern hosts applied there.
Prebuilt toolchain binaries and build outputs are not included.
TXT
tar -C "$(dirname "$IC3115W_GPL")" \
    --exclude="$(basename "$IC3115W_GPL")/$tc/bin" --exclude="$(basename "$IC3115W_GPL")/$tc/libexec" \
    --exclude="$(basename "$IC3115W_GPL")/$tc/lib" --exclude="$(basename "$IC3115W_GPL")/$tc/mips-linux" \
    --exclude="$(basename "$IC3115W_GPL")/$tc/include" \
    --exclude='*.o' --exclude='squashfs-tools/mksquashfs' --exclude='squashfs-tools/unsquashfs' \
    --owner=0 --group=0 --numeric-owner --sort=name --mtime='2014-09-01 00:00Z' \
    --transform 's|^|ic3115w-gpl-sources/|' \
    -cf /tmp/gpl.tar "$(basename "$IC3115W_GPL")"
tar -C /tmp --owner=0 --group=0 --numeric-owner --mtime='2014-09-01 00:00Z' \
    --transform 's|^|ic3115w-gpl-sources/|' -rf /tmp/gpl.tar GPL-SOURCES.txt
xz -T0 -6 < /tmp/gpl.tar > dist/ic3115w-gpl-sources.tar.xz
rm -f /tmp/gpl.tar /tmp/GPL-SOURCES.txt
ls -l dist
