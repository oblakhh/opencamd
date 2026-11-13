#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
#
# fetch-vendor.sh <dest-dir> <file-name> <sha256> [url]
# Take <file-name> from /vendor (the "vendor" build context, searched recursively)
# or download it from <url>, then verify its SHA-256. A .zip download is searched for
# <file-name>, which is extracted (the checksum always applies to <file-name> itself).
# Fails with instructions if neither source is available.
set -e
dest=$1 name=$2 sum=$3 url=$4
mkdir -p "$dest"
src=$(find /vendor -name "$name" -type f 2>/dev/null | head -n 1)
if [ -n "$src" ]; then
	echo "vendor: using $name from the vendor build context"
	cp "$src" "$dest/$name"
elif [ -n "$url" ]; then
	echo "vendor: downloading $name from $url"
	echo "vendor: (vendor file, used only locally for this build and verified by SHA-256 below)"
	curl -fL --retry 3 -o "$dest/download" "$url"
	case "$url" in
	*.zip)
		python3 -I - "$dest/download" "$name" "$dest/$name" <<'PY'
import shutil, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
members = [m for m in z.namelist() if m.rsplit("/", 1)[-1] == sys.argv[2]]
if not members:
    sys.exit("vendor: %s not found in the downloaded zip" % sys.argv[2])
with z.open(members[0]) as src, open(sys.argv[3], "wb") as dst:
    shutil.copyfileobj(src, dst, 1 << 20)
PY
		rm -f "$dest/download" ;;
	*)	mv "$dest/download" "$dest/$name" ;;
	esac
else
	echo "ERROR: $name not found." >&2
	echo "  Put it into the vendor/ directory (see docker/README.md), or pass a download URL" >&2
	echo "  with --build-arg (GPL_URL for the GPL tarball, STOCK_URL for the stock firmware)." >&2
	exit 1
fi
echo "$sum  $dest/$name" | sha256sum -c -
