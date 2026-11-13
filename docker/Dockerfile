# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
#
# Build environment for opencamd (camd + the IC-3115W firmware image): vendor MIPS toolchain, the
# GPL source subset we build against, host tools and Node for the web UI.
#
# The vendor inputs are not part of this repository. They are taken from the
# "vendor" build context (a directory holding the two files below) or downloaded
# from GPL_URL (default: Edimax's server) / STOCK_URL, and are always verified
# against the pinned SHA-256:
#
#   IC3115W_EDIMAX_20140901.tar.gz   Edimax GPL source release for the IC-3115W
#   IC-3115W_v2.03.bin               stock firmware v2.03 (only its kernel and its
#                                    GPL/LGPL userland are kept; see prepare-sdk.sh)
#
#   docker build -f docker/Dockerfile --build-context vendor=vendor -t camd-build .
#   (or simply: docker compose build)
#
# The image is built locally (and in CI) and never published: the vendor files are
# used only on the machine that builds it. Even so, only open-source material is
# kept: Edimax's application sources, the unrelated SDK versions, Realtek's
# unlicensed tools and the proprietary parts of the stock firmware are removed.
# The vendor toolchain is 32-bit x86, hence amd64.

ARG DEBIAN_IMAGE=debian:12
ARG NODE_IMAGE=node:22.12.0-bookworm-slim

# Empty by default; replaced by --build-context vendor=<dir>.
FROM scratch AS vendor

FROM ${NODE_IMAGE} AS node

FROM --platform=linux/amd64 ${DEBIAN_IMAGE} AS base
RUN dpkg --add-architecture i386 && apt-get update && apt-get install -y --no-install-recommends \
      build-essential gcc-multilib g++-multilib libc6-dev-i386 \
      zlib1g-dev zlib1g-dev:i386 libncurses-dev libncurses-dev:i386 bison flex \
      squashfs-tools xxd file python3 xz-utils bzip2 curl ca-certificates \
      git patch wget cpio perl bc qemu-user-static \
    && rm -rf /var/lib/apt/lists/*
# The vendor scripts are bash scripts with #!/bin/sh (built on Fedora); Debian's sh is dash.
RUN ln -sf bash /bin/sh
# Host tools in the SDK predate GCC 5/10 defaults (gnu89 inline semantics, -fcommon). This only
# affects the host gcc/cc; the MIPS cross compiler (rsdk-linux-gcc) is separate.
RUN printf '#!/bin/sh\nexec /usr/bin/gcc -fgnu89-inline -fcommon "$@"\n' > /usr/local/bin/gcc \
    && chmod +x /usr/local/bin/gcc && ln -s gcc /usr/local/bin/cc

# Downloads in their own layer: they are cached (locally and in CI) independently of the
# preparation scripts below, so editing those does not download 800 MB again.
FROM base AS downloads
# The GPL release (a zip around the tarball): Edimax's download, then the Internet
# Archive's copy (Edimax's server refuses some networks, e.g. GitHub's runners).
ARG GPL_URL="https://download.edimax.com/mw/cufiles/files/download/OpenSourceCode/transfer/Wireless/IPCamera/IC-3115W/IC-3115W_v2.01_GPL_20140901.zip https://archive.org/download/edimax-gpl-source/For%20Home/Network%20Cameras/IC-3115W/IC-3115W_v2.01_GPL_20140901.zip"
ARG GPL_SHA256=25b209c7023ab1d8813c50e4ca0ac1f8195bdc26f464771545130c6c03e68f8f
# Edimax's download of the stock firmware v2.03 (a zip around the .bin).
ARG STOCK_URL="https://download.edimax.com/mw/cufiles/files/download/Firmware/transfer/Wireless/IPCamera/IC-3115W/IC-3115W_v2.03.zip"
ARG STOCK_SHA256=ccc91f8dfabd33ee0a4d342e24189ba983ccf0c9841f7d844e96880a0d0f0e19
# Fallback when the stock firmware can't be downloaded: only the parts of it we use
# (stock kernel + rootfs reduced to its GPL/LGPL components, as prepare-sdk.sh makes
# them), published once as a release asset. Every firmware release redistributes
# these binaries anyway, with their source.
ARG STOCK_BASE_URL="https://github.com/oblakhh/opencamd/releases/download/stock-base/ic3115w-stock-base.tar"
ARG STOCK_BASE_SHA256=925c0f2e0ab90ee24202d919a6cd7e04e46896caddfee8e9a7e2602bc5ddfe16
COPY docker/fetch-vendor.sh /opt/build-scripts/
RUN --mount=from=vendor,target=/vendor \
    sh /opt/build-scripts/fetch-vendor.sh /dl IC3115W_EDIMAX_20140901.tar.gz "$GPL_SHA256" "$GPL_URL" \
 && { sh /opt/build-scripts/fetch-vendor.sh /dl IC-3115W_v2.03.bin "$STOCK_SHA256" "$STOCK_URL" \
      || sh /opt/build-scripts/fetch-vendor.sh /dl ic3115w-stock-base.tar "$STOCK_BASE_SHA256" "$STOCK_BASE_URL"; }

FROM base AS sdk
COPY docker/prepare-sdk.sh tools/fwimage.py tools/prune_rootfs.py /opt/build-scripts/
RUN --mount=from=downloads,source=/dl,target=/dl \
    sh /opt/build-scripts/prepare-sdk.sh /dl /opt/ic3115w

FROM base
COPY --from=node /usr/local/bin/node /usr/local/bin/node
COPY --from=node /usr/local/lib/node_modules /usr/local/lib/node_modules
RUN ln -s ../lib/node_modules/npm/bin/npm-cli.js /usr/local/bin/npm \
 && ln -s ../lib/node_modules/npm/bin/npx-cli.js /usr/local/bin/npx
COPY --from=sdk /opt/ic3115w /opt/ic3115w
ENV IC3115W_GPL=/opt/ic3115w/gpl/IC3115W_GPL \
    IC3115W_STOCK=/opt/ic3115w/stock
LABEL org.opencontainers.image.title="opencamd build environment" \
      org.opencontainers.image.description="Toolchain and GPL sources for building opencamd for the Edimax IC-3115W" \
      org.opencontainers.image.licenses="GPL-2.0-or-later"
WORKDIR /work
