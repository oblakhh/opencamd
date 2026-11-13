#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
#
# Non-interactive replacement for the vendor flow in resources/IC-3115W_GPL_20140901/README.txt
# (usetenv, prepare.sh with its menuconfig screens, IC3015_BUILD).
# Run inside the camd-build container on a COPY of IC3115W_GPL (the build rewrites it in place):
#   tar xzf resources/IC-3115W_GPL_20140901/IC3115W_EDIMAX_20140901.tar.gz -C build   # NOT the macOS-extracted copy (case collisions)
#   docker run --rm -v "$PWD":/work camd-build tools/build_gpl.sh build/IC3115W_GPL [version]
set -e

TREE=$(cd "${1:?usage: build_gpl.sh <IC3115W_GPL copy> [version]}" && pwd)
VERSION=${2:-2.03}
cd "$TREE"

# usetenv: it only exports variables, so source it in this shell.
cd project
. ./usetenv
cd "$TREE"

# prepare.sh, minus the menuconfig screens and the toolchain build (the toolchain is prebuilt).
rm -f rtl819x-sdk rtl819x-sdk-patch bootcode
ln -sf rtl819x-sdk-v2.3 rtl819x-sdk
ln -sf patch_v2.3 rtl819x-sdk-patch
ln -sf bootcode_rtl8196c_98 bootcode
export RTL819X_SDK_DIR=rtl819x-sdk

restore() { (cd rtl819x-sdk-patch/oldconfig && ./restoreConfig); }

SDK=$TREE/rtl819x-sdk/rtl819x
# The sub-Makefiles expect what the top-level Makefile normally exports.
export DIR_ROOT=$SDK DIR_LINUX=$SDK/linux-2.6.30 DIR_BZBOX=$SDK/users/busybox-1.13 DIR_USERS=$SDK/users
export PATH=$SDK/toolchain/rsdk-1.3.6-4181-EB-2.6.30-0.9.30/bin:$PATH
cd "$SDK"
# GNU make >= 3.82 rejects rules that mix implicit and normal targets (old kbuild Makefiles).
# Drop the plain targets "config" and "/"; only the pattern rules are used by this build.
for mk in users/busybox-1.13/Makefile linux-2.6.30/Makefile; do
	sed -i -e 's|^config %config: |%config: |' -e 's|^/ %/: |%/: |' "$mk"
done
# Host squashfs tools: glibc >= 2.28 no longer pulls major()/minor()/makedev() in via sys/types.h.
for c in users/squashfs4.0/squashfs-tools/mksquashfs.c users/squashfs4.0/squashfs-tools/unsquashfs.c; do
	grep -q 'sys/sysmacros.h' "$c" || sed -i '0,/^#include/s||#include <sys/sysmacros.h>\n#include|' "$c"
done
# Perl >= 5.22 rejects defined(@array) (upstream kernel fix, 2.6.36).
sed -i 's/if (!defined(@val))/if (!@val)/' linux-2.6.30/kernel/timeconst.pl
config/genconfig > Kconfig
# First pass: setconfig records .oldconfig and creates the target/romfs/image symlinks.
# Without an .oldconfig it reloads the board defaults, so restore the vendor configs afterwards.
cd "$TREE"; restore; cd "$SDK"
config/setconfig defaults
cd "$TREE"; restore; cd "$SDK"
# Second pass, equivalent to saving every menuconfig screen without changes.
config/setconfig defaults
yes "" | make -C linux-2.6.30 oldconfig > /dev/null
(cd users && yes "" | "$SDK/config/conf" -o Kconfig > /dev/null)
yes "" | make -C users/busybox-1.13 oldconfig > /dev/null
config/setconfig final
config/hdrconfig "$SDK"

# Bootcode menuconfig, non-interactive: Configure -d keeps the restored .config values.
cd "$TREE/bootcode"
yes "" | AUTOCONF_FILE=autoconf.h bash config/scripts/Configure -d config.in > /dev/null
cp autoconf.h boot/include/linux/autoconf2.h

# IC3015_BUILD asks for the firmware version and a comment.
cd "$TREE"
mkdir -p VendorVersions
printf '%s\n\n' "$VERSION" | ./IC3015_BUILD
ls -la image/IC3115/EDIMAX/ 2>/dev/null || ls -la image/
