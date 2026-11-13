#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
#
# Cross-compile camd for the device (MIPS, rsdk GCC 3.4.6). Run inside the
# camd-build container from the project root:
#   docker compose run --rm build tools/build_camd.sh
set -e
cd /work
. tools/env.sh
# mbedTLS static libs (TLS 1.2 for the HTTPS uploader), built out of tree in build/mbedtls
# so the submodule stays clean; rebuilt when the mbedTLS version changes.
MB_SRC=firmware/third_party/mbedtls
MB=build/mbedtls
[ -f "$MB_SRC/include/mbedtls/version.h" ] || { echo "mbedTLS source missing - run: git submodule update --init"; exit 1; }
stamp=$(sha256sum "$MB_SRC/include/mbedtls/version.h" | cut -c1-16)
if [ ! -f "$MB/library/libmbedtls.a" ] || [ "$(cat "$MB/.stamp" 2>/dev/null)" != "$stamp" ]; then
  echo "=== building mbedTLS (rsdk) ==="
  rm -rf "$MB"; mkdir -p "$MB"
  cp -a "$MB_SRC/include" "$MB_SRC/library" "$MB_SRC/3rdparty" "$MB/"
  make -C "$MB/library" \
    CC="$RSDK/rsdk-linux-gcc" AR="$RSDK/rsdk-linux-ar" \
    CFLAGS="-O2 -std=gnu99 -D_FILE_OFFSET_BITS=64" WARNING_CFLAGS=" " > "$MB/build.log" 2>&1 || true
  test -f "$MB/library/libmbedtls.a" || { tail -20 "$MB/build.log"; echo "mbedTLS build FAILED"; exit 1; }
  echo "$stamp" > "$MB/.stamp"
fi
make -C firmware/camd clean
make -C firmware/camd all CC="$RSDK/rsdk-linux-gcc"
"$RSDK/rsdk-linux-strip" firmware/camd/camd
echo "---"
file firmware/camd/camd
ls -l firmware/camd/camd
