#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
#
# Build a "de-bloated stock" firmware: the stock v2.03 rootfs with cloud phone-home disabled,
# camera functionality untouched. Run inside the camd-build container (as root) from /work:
#   docker run --rm -v "$PWD":/work camd-build tools/build_debloat.sh
# Output: build/debloat/IC3115W_debloat_upg.bin  (flash via web upgrade or TFTP).
set -e
cd /work
STOCK=$(pwd)/resources/IC-3115W_v2.03.bin
ST=$(pwd)/build/IC3115W_GPL/rtl819x-sdk-v2.3/rtl819x/users/squashfs4.0/squashfs-tools
KERNEL=$(pwd)/build/fw/recover/tftp/linux.bin   # stock kernel (cr6c block), unchanged
DEST=build/debloat                              # on the /work bind mount (final artifacts only)
# The /work bind mount cannot hold device nodes (mknod fails), so extract+repack in a
# container-local dir (tmpfs/overlay supports mknod), then copy the image back to /work.
OUT=/tmp/debloat
ROOT=$OUT/root
rm -rf "$OUT" "$DEST"; mkdir -p "$OUT" "$DEST"

# 1. Faithfully extract the stock rootfs (vendor unsquashfs handles the old LZMA; root => dev nodes).
dd if="$STOCK" of="$OUT/stock_rootfs.sqfs" bs=1M iflag=skip_bytes skip=1048704 status=none  # 0x100080
"$ST/unsquashfs" -d "$ROOT" "$OUT/stock_rootfs.sqfs" > "$OUT/unsq.log" 2>&1

# 2. De-bloat edits (camera stack left fully intact).
#  a) Disable stunc (EdiLife STUN phone-home launched from rcS).
sed -i 's|^if \[ -f /bin/runStun.sh \]; then|if false; then  # debloat: stunc disabled|' "$ROOT/etc/init.d/rcS"
#  b) Neuter UPnP IGD (spawned by ipcam; make the helper a no-op).
printf '#!/bin/sh\nexit 0  # debloat: UPnP IGD disabled\n' > "$ROOT/etc/script/run_igd.sh"
chmod 755 "$ROOT/etc/script/run_igd.sh"
#  c) Marker so we can confirm our rootfs is the one running.
echo "IC-3115W de-bloat v1 built $(date -u '+%Y-%m-%dT%H:%M:%SZ')" > "$ROOT/etc/custom-firmware.txt"

# 3. Repack (match vendor options; root context captures symlinks + device nodes directly).
rm -f "$OUT/rootfs.sqfs"
"$ST/mksquashfs" "$ROOT" "$OUT/rootfs.sqfs" -comp lzma -always-use-fragments -all-root -noappend > "$OUT/mksq.log" 2>&1

# 4. Pack the upgrade image (stock kernel + new rootfs) and verify, then copy artifacts to /work.
python3 -I tools/fwimage.py pack --kernel "$KERNEL" --rootfs "$OUT/rootfs.sqfs" -o "$OUT/IC3115W_debloat_upg.bin"
cp "$OUT/IC3115W_debloat_upg.bin" "$OUT/rootfs.sqfs" "$DEST/"
echo "--- rootfs size $(stat -c%s "$OUT/rootfs.sqfs") (limit 3080192); device nodes: $(find "$ROOT" \( -type c -o -type b \) | wc -l) ---"
ls -l "$DEST/IC3115W_debloat_upg.bin"
