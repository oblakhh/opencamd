#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
#
# fetch-vendor.sh <dest-dir> <file-name> <sha256> [urls]
# Take <file-name> from /vendor (the "vendor" build context, searched recursively)
# or download it, trying each of the space-separated <urls> in order (the vendor's
# server first, then mirrors) until one yields a file with the pinned SHA-256.
# A .zip download is searched for <file-name>, which is extracted (the checksum
# always applies to <file-name> itself). Fails with instructions if no source works.
set -e
dest=$1 name=$2 sum=$3 urls=$4
mkdir -p "$dest"
src=$(find /vendor -name "$name" -type f 2>/dev/null | head -n 1)
if [ -n "$src" ]; then
	echo "vendor: using $name from the vendor build context"
	cp "$src" "$dest/$name"
	echo "$sum  $dest/$name" | sha256sum -c -
	exit 0
fi
for u in $urls; do
	echo "vendor: downloading $name from $u"
	echo "vendor: (vendor file, used only locally for this build and verified by SHA-256 below)"
	rm -f "$dest/download" "$dest/$name"
	if ! curl -fL --retry 3 -o "$dest/download" "$u"; then
		echo "vendor: download failed, trying the next source"
		continue
	fi
	case "$u" in
	*.zip)
		if ! python3 -I - "$dest/download" "$name" "$dest/$name" <<'PY'
import shutil, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
members = [m for m in z.namelist() if m.rsplit("/", 1)[-1] == sys.argv[2]]
if not members:
    sys.exit("vendor: %s not found in the downloaded zip" % sys.argv[2])
with z.open(members[0]) as src, open(sys.argv[3], "wb") as dst:
    shutil.copyfileobj(src, dst, 1 << 20)
PY
		then
			echo "vendor: not a usable zip, trying the next source"
			continue
		fi ;;
	*)	mv "$dest/download" "$dest/$name" ;;
	esac
	rm -f "$dest/download"
	if echo "$sum  $dest/$name" | sha256sum -c -; then
		exit 0
	fi
	echo "vendor: checksum mismatch, trying the next source"
done
echo "vendor: no source delivered $name with the pinned checksum" >&2
echo "vendor: (put the file into vendor/, see docs/building.md, or pass other download URLs" >&2
echo "vendor:  with --build-arg; the stock firmware falls back to the stock-base asset)" >&2
exit 1
