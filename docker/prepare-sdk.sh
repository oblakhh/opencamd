#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
#
# prepare-sdk.sh <download-dir> <prefix>
# Turns the verified vendor downloads into the build environment under <prefix>:
#   gpl/IC3115W_GPL/   pruned GPL source tree: toolchain, kernel, and the sources of
#                      the GPL tools that end up in the image (busybox, udhcp,
#                      wireless tools, squashfs + lzma); host squashfs tools built
#   stock/linux.bin    the stock kernel (GPL), unchanged
#   stock/rootfs-base.tar   the stock rootfs reduced to its GPL/LGPL components
#                      (tools/prune_rootfs.py) plus device nodes
# Everything else from the downloads (Edimax's application sources, other SDK
# versions, the second toolchain, proprietary binaries) is discarded.
set -e
dl=$1 prefix=$2
scripts=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$prefix/gpl" "$prefix/stock"

# --- GPL tree --------------------------------------------------------------
tar xzf "$dl/IC3115W_EDIMAX_20140901.tar.gz" -C "$prefix/gpl"
top=$prefix/gpl/IC3115W_GPL
cd "$top"
# Edimax's own application, other SDK releases, other boards' bootloaders and the
# stock-image build scaffolding: not needed, and partly not open source.
rm -rf ipcam v2.5 v3.2 bootcode_rtl8196c_98 bootcode_rtl8196d image toolchain \
       project prepare.sh IC3015_BUILD CLEAN_IC3015 VendorVersions rtl819x-sdk rtl819x-sdk-patch
sdk=$top/rtl819x-sdk-v2.3/rtl819x
cd "$sdk"
rm -rf toolchain/rsdk-1.3.6-5281-EB-2.6.30-0.9.30 boards/rtl8198 image romfs target tmpfs
# Realtek's own utilities and manual in the toolchain: no license, no source, not used by
# our build (which needs only gcc/binutils). The GNU tools and uClibc stay.
tc=toolchain/rsdk-1.3.6-4181-EB-2.6.30-0.9.30
rm -f "$tc"/bin/catimg "$tc"/bin/img2bin "$tc"/bin/imgpad "$tc"/bin/lxobjHex* \
      "$tc"/bin/*genopc "$tc"/bin/*opcutil "$tc"/bin/rlxisa*.bin "$tc"/userguide.pdf
cd users
for d in * .[!.]*; do
	[ -e "$d" ] || [ -L "$d" ] || continue
	case "$d" in
	busybox|busybox-1.13|udhcp-0.9.9-pre|wireless_tools.25|wireless_tools.29|squashfs4.0|lzma465|Makefile|Kconfig|Kconfig.mk) ;;
	*) rm -rf "$d" ;;
	esac
done

# Modern-host fixes (same as tools/build_gpl.sh), so kernel/busybox builds work later.
cd "$sdk"
for mk in users/busybox-1.13/Makefile linux-2.6.30/Makefile; do
	sed -i -e 's|^config %config: |%config: |' -e 's|^/ %/: |%/: |' "$mk"
done
for c in users/squashfs4.0/squashfs-tools/mksquashfs.c users/squashfs4.0/squashfs-tools/unsquashfs.c; do
	grep -q 'sys/sysmacros.h' "$c" || sed -i '0,/^#include/s||#include <sys/sysmacros.h>\n#include|' "$c"
done
sed -i 's/if (!defined(@val))/if (!@val)/' linux-2.6.30/kernel/timeconst.pl

# Host squashfs tools with the vendor LZMA variant (the stock rootfs format).
make -C users/squashfs4.0/squashfs-tools -j"$(nproc)" mksquashfs unsquashfs > /tmp/squashfs.log 2>&1 \
	|| { tail -30 /tmp/squashfs.log; exit 1; }
st=$sdk/users/squashfs4.0/squashfs-tools

# --- Stock firmware: keep only the kernel and the pruned GPL/LGPL userland ----
work=/tmp/stock
mkdir -p "$work"
python3 -I "$scripts/fwimage.py" split "$dl/IC-3115W_v2.03.bin" "$work/split" > /dev/null
cp "$work/split/linux.bin" "$prefix/stock/linux.bin"
dd if="$dl/IC-3115W_v2.03.bin" of="$work/rootfs.sqfs" bs=1M iflag=skip_bytes skip=1048704 status=none
"$st/unsquashfs" -d "$work/root" "$work/rootfs.sqfs" > /dev/null
python3 -I "$scripts/prune_rootfs.py" "$work/root"
tar -C "$work/root" --numeric-owner -cf "$prefix/stock/rootfs-base.tar" .
rm -rf "$work"

du -sh "$prefix/gpl" "$prefix/stock"
