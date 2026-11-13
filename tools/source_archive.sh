#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
#
# Complete source archive of this repository at HEAD, including the submodules (GitHub's
# automatic "Source code" downloads leave them out). Run from the project root, outside or
# inside the container (needs git):
#   tools/source_archive.sh <label>   ->  dist/camd-<label>-source.tar.gz
set -e
label=${1:?usage: source_archive.sh <label>}
prefix=camd-$label
out=dist/$prefix-source.tar.gz
mkdir -p dist
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git archive --format=tar --prefix="$prefix/" HEAD > "$tmp/all.tar"
git submodule foreach --quiet 'echo "$sm_path"' | while read -r p; do
	git -C "$p" archive --format=tar --prefix="$prefix/$p/" HEAD > "$tmp/sub.tar"
	tar --concatenate -f "$tmp/all.tar" "$tmp/sub.tar"
done
gzip -9n < "$tmp/all.tar" > "$out"
ls -l "$out"
