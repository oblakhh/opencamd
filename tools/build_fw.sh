#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
#
# Build the test firmware image: vendor kernel (from the GPL build) + our userland from firmware/.
# Needs a completed tools/build_gpl.sh run. Run inside the camd-build container from the project root:
#   docker run --rm -v "$PWD":/work camd-build tools/build_fw.sh [build/IC3115W_GPL]
# Output: build/fw/IC3115W_test_upg.bin (verified with tools/fwimage.py).
set -e

PROJ=$(pwd)
GPL=$(cd "${1:-build/IC3115W_GPL}" && pwd)
SDK=$GPL/rtl819x-sdk-v2.3/rtl819x
RSDK=$SDK/toolchain/rsdk-1.3.6-4181-EB-2.6.30-0.9.30
OUT=$PROJ/build/fw
ROOT=$OUT/rootfs
export PATH=$RSDK/bin:$PATH
CC=rsdk-linux-gcc
JOBS=$(nproc)

[ -f "$GPL/image/linux.bin" ] || { echo "no $GPL/image/linux.bin; run tools/build_gpl.sh first"; exit 1; }
rm -rf "$ROOT" && mkdir -p "$ROOT"

# busybox: the vendor config plus firmware/busybox.fragment, built in a private copy of the SDK source.
BB=$OUT/busybox
if [ ! -d "$BB" ]; then
	cp -a "$SDK/users/busybox-1.13" "$BB"
	make -C "$BB" clean > /dev/null
fi
frag=$PROJ/firmware/busybox.fragment
syms=$(grep -o '^CONFIG_[A-Z0-9_]*' "$frag" | sed 's/$/[= ]/; s/^/^(# )?/' | paste -sd'|')
grep -v -E "$syms" "$GPL/patch_v2.3/oldconfig/busybox_config" | grep -v '^CONFIG_PREFIX=' > "$BB/.config"
grep '^CONFIG_' "$frag" >> "$BB/.config"
echo 'CONFIG_PREFIX="'"$ROOT"'"' >> "$BB/.config"
yes "" | make -C "$BB" oldconfig > /dev/null
make -C "$BB" -j"$JOBS" > "$OUT/busybox.log" 2>&1 || { tail -30 "$OUT/busybox.log"; exit 1; }
make -C "$BB" install > /dev/null

# Our programs.
mkdir -p "$ROOT/bin"
$CC -O2 -Wall -o "$ROOT/bin/camprobe" "$PROJ/firmware/camprobe/camprobe.c"

# Overlay, directories, version.
cp -a "$PROJ/firmware/rootfs/." "$ROOT/"
mkdir -p "$ROOT"/{dev,proc,sys,var,mnt,lib}
ln -sfn var/tmp "$ROOT/tmp"
echo "IC-3115W test image -- $(date -u '+%Y-%m-%d %H:%M UTC')" > "$ROOT/etc/version"

# Shared libraries the binaries need, from the toolchain's uClibc as installed by the SDK build.
LIBSRC=$SDK/target/romfs/lib
need() { rsdk-linux-readelf -d "$@" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | sort -u; }
libs=$(need "$ROOT/bin/busybox" "$ROOT/bin/camprobe")
libs="$libs ld-uClibc.so.0"
for l in $libs; do
	for n in $(need "$LIBSRC/$l"); do libs="$libs $n"; done
done
for l in $(echo $libs | tr ' ' '\n' | sort -u); do
	cp -a "$LIBSRC/$l" "$ROOT/lib/"
	t=$(readlink "$LIBSRC/$l" || true)
	[ -z "$t" ] || cp -a "$LIBSRC/$t" "$ROOT/lib/"
done
rsdk-linux-strip "$ROOT/bin/camprobe" "$ROOT/bin/busybox"

# Squashfs (the SDK's patched mksquashfs, LZMA, as in boards/rtl8196c/tools/mkimg) and the upgrade image.
rm -f "$OUT/rootfs.sqfs"
"$SDK/users/squashfs4.0/squashfs-tools/mksquashfs" "$ROOT" "$OUT/rootfs.sqfs" -comp lzma \
	-always-use-fragments -all-root -noappend -pf "$PROJ/firmware/devices.txt" > /dev/null
python3 -I "$PROJ/tools/fwimage.py" pack --kernel "$GPL/image/linux.bin" --rootfs "$OUT/rootfs.sqfs" \
	-o "$OUT/IC3115W_test_upg.bin"
du -sh "$ROOT"; ls -l "$OUT/rootfs.sqfs" "$OUT/IC3115W_test_upg.bin"
python3 -I "$PROJ/tools/fwimage.py" split "$OUT/IC3115W_test_upg.bin" "$OUT/tftp" | tail -1
