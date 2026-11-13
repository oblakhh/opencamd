# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
#
# Sourced by the build scripts: locations of the vendor toolchain and inputs.
# In the camd build image these come from IC3115W_GPL / IC3115W_STOCK (see
# docker/Dockerfile); without them, the older local layout under build/ and
# resources/ is used.
GPL=${IC3115W_GPL:-/work/build/IC3115W_GPL}
SDK=$GPL/rtl819x-sdk-v2.3/rtl819x
RSDK=$SDK/toolchain/rsdk-1.3.6-4181-EB-2.6.30-0.9.30/bin
ST=$SDK/users/squashfs4.0/squashfs-tools
