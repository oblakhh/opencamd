#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
#
# Build the full custom firmware image: stock vendor kernel + a lean rootfs made
# of the stock base (busybox, uClibc, device nodes) with the vendor application
# stack removed and our stack added: camd (daemon), the Vue UI (/www), and our init.
#
# Prereqs: firmware/camd/camd built (tools/build_camd.sh) and firmware/ui/dist built.
# Run inside the camd-build container (root, for device nodes) from the project root:
#   docker compose run --rm build tools/build_image.sh
# Output: build/image/IC3115W_camd_upg.bin
set -e
cd /work
. tools/env.sh
# Stock inputs: from the build image (kernel + pruned rootfs tarball) or, outside
# it, the stock firmware file and its split-out kernel.
if [ -n "$IC3115W_STOCK" ]; then
	KERNEL=$IC3115W_STOCK/linux.bin
	BASE_TAR=$IC3115W_STOCK/rootfs-base.tar
else
	STOCK=$(pwd)/resources/IC-3115W_v2.03.bin
	KERNEL=$(pwd)/build/fw/recover/tftp/linux.bin   # stock kernel (cr6c), unchanged
fi
CAMD=$(pwd)/firmware/camd/camd
UIDIST=$(pwd)/firmware/ui/dist
OVERLAY=$(pwd)/firmware/rootfs
DEST=build/image
OUT=/tmp/image              # mknod-capable (the /work bind mount is not)
ROOT=$OUT/root

[ -f "$CAMD" ]   || { echo "missing $CAMD — run tools/build_camd.sh first"; exit 1; }
[ -d "$UIDIST" ] || { echo "missing $UIDIST — build the UI first (cd firmware/ui && npm run build)"; exit 1; }
rm -rf "$OUT" "$DEST"; mkdir -p "$OUT" "$DEST"

# 1. Stock rootfs as the base (gives us busybox, uClibc, /dev nodes, symlinks).
mkdir -p "$ROOT"
if [ -n "$BASE_TAR" ]; then
	tar -C "$ROOT" -xf "$BASE_TAR"
else
	dd if="$STOCK" of="$OUT/rootfs.sqfs" bs=1M iflag=skip_bytes skip=1048704 status=none
	rmdir "$ROOT"; "$ST/unsquashfs" -d "$ROOT" "$OUT/rootfs.sqfs" > "$OUT/unsq.log" 2>&1
fi

# 2. Allow list: keep only named GPL/LGPL stock components + device nodes; remove
#    everything else (vendor apps, assets, scripts, Edimax's cloud key in etc/ssl).
#    (The build image's base is already pruned; running it again is harmless.)
python3 -I tools/prune_rootfs.py "$ROOT"

# 3. Our overlay (init, passwd, hostname, udhcpc script) — overwrites stock /etc bits.
cp -a "$OVERLAY/." "$ROOT/"
# 4. camd + UI.
mkdir -p "$ROOT/bin" "$ROOT/www"
cp "$CAMD" "$ROOT/bin/camd"
cp "$(dirname "$CAMD")/getmac" "$ROOT/bin/getmac"
cp -a "$UIDIST/." "$ROOT/www/"
# MTD char nodes + /dev/mem for OTA (the squashfs /dev ships only mtdblock*);
# camd also falls back to /var at runtime, but bake them so the canonical path works.
for n in 0 1 2; do [ -e "$ROOT/dev/mtd$n" ] || mknod "$ROOT/dev/mtd$n" c 90 $((n*2)); done
[ -e "$ROOT/dev/mem" ] || mknod "$ROOT/dev/mem" c 1 1

# config dir / version
echo "camd custom firmware (built $(date -u '+%Y-%m-%dT%H:%M:%SZ'))" > "$ROOT/etc/version"

# 5. Sanity: ensure camd's shared libs are present.
MISS=""
for lib in $("$RSDK/rsdk-linux-readelf" -d "$CAMD" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'); do
	[ -e "$ROOT/lib/$lib" ] || MISS="$MISS $lib"
done
[ -z "$MISS" ] || { echo "ERROR: camd needs missing libs:$MISS"; exit 1; }

# 6. Repack squashfs (vendor LZMA) and pack the upgrade image.
rm -f "$OUT/new_rootfs.sqfs"
"$ST/mksquashfs" "$ROOT" "$OUT/new_rootfs.sqfs" -comp lzma -always-use-fragments -all-root -noappend > "$OUT/mksq.log" 2>&1
python3 -I tools/fwimage.py pack --kernel "$KERNEL" --rootfs "$OUT/new_rootfs.sqfs" -o "$OUT/IC3115W_camd_upg.bin"
cp "$OUT/IC3115W_camd_upg.bin" "$OUT/new_rootfs.sqfs" "$DEST/"
echo "--- rootfs $(stat -c%s "$OUT/new_rootfs.sqfs") bytes (limit 3080192); du $(du -sh "$ROOT" | cut -f1); dev nodes $(find "$ROOT" \( -type c -o -type b \) | wc -l) ---"
ls -l "$DEST/IC3115W_camd_upg.bin"
