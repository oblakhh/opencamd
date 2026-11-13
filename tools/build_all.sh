#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
#
# Full build inside the camd build image, from the project root:
#   docker compose run --rm build            (or: tools/build_all.sh inside the container)
# Runs the host unit tests, builds camd (+ mbedTLS), the web UI and the firmware image.
# Output: build/image/IC3115W_camd_upg.bin (web/OTA upgrade) and build/image/tftp/ (TFTP recovery).
set -e
cd /work
for m in civetweb/include/civetweb.h cjson/cJSON.h mbedtls/include/mbedtls/version.h; do
	[ -f "firmware/third_party/$m" ] || { echo "Missing third-party sources - run: git submodule update --init"; exit 1; }
done
# Files created here are owned by root inside the container; hand them back to the
# owner of the checkout on exit (no-op on Docker Desktop, which maps ownership).
owner=$(stat -c %u:%g /work)
trap 'chown -R "$owner" build firmware/camd firmware/ui/dist firmware/ui/node_modules \
      2>/dev/null || true' EXIT

echo "=== unit tests (host) ==="
make -C firmware/camd test
echo "=== camd (MIPS) ==="
tools/build_camd.sh
echo "=== web UI ==="
( cd firmware/ui && npm ci --no-audit --no-fund && npm test -- --run && npm run build )
echo "=== firmware image ==="
tools/build_image.sh
python3 -I tools/fwimage.py split build/image/IC3115W_camd_upg.bin build/image/tftp
sha256sum build/image/IC3115W_camd_upg.bin build/image/tftp/*.bin
