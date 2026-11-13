# Building, testing and releasing

## 1. Build environment

One Docker image holds everything: the vendor MIPS toolchain (GCC 3.4.6 / uClibc 0.9.30), the
subset of the GPL sources we build against, the host squashfs tools, Python and Node for the web
UI. You need Docker with BuildKit (any current version) and a recursive clone of this repository:

```sh
git clone --recursive <repo-url>        # or, in an existing clone: git submodule update --init
docker compose run --rm build           # tests + camd + UI + firmware image -> build/image/
docker compose run --rm build bash      # interactive shell in the build environment
```

The first run builds the image (`docker compose build`, or
`docker build -f docker/Dockerfile --build-context vendor=vendor -t camd-build .`). The toolchain
is 32-bit x86, so the image is `linux/amd64`; on Apple Silicon it runs under emulation, slower but
fine.

Outputs: `build/image/IC3115W_camd_upg.bin` (upgrade image) and `build/image/tftp/linux.bin`,
`build/image/tftp/root.bin` (TFTP recovery files).

### Vendor inputs

Two files from Edimax are needed. They are **not** in the repository and are never redistributed;
the image is built locally (and in CI) and never published. Both are verified against SHA-256
checksums pinned in `docker/Dockerfile`, and the build fails on a mismatch.

| File | What | Source |
|---|---|---|
| `IC3115W_EDIMAX_20140901.tar.gz` | Edimax GPL source release for the IC-3115W | downloaded from Edimax by default (`GPL_URL`, a zip around the tarball) |
| `IC-3115W_v2.03.bin` | stock firmware v2.03 | downloaded from Edimax by default (`STOCK_URL`, a zip around the `.bin`) |

A copy placed in `vendor/` takes precedence over the download, for offline builds or if the
download goes away (use copies or hard links; symlinks do not work in a build context). Other URLs
can be passed with `--build-arg GPL_URL=… STOCK_URL=…`.

### What the image contains

`docker/fetch-vendor.sh` obtains and verifies the two files; `docker/prepare-sdk.sh` reduces them
to open-source material:

- `/opt/ic3115w/gpl/IC3115W_GPL/`: the GPL tree, pruned to the `rsdk-1.3.6-4181` toolchain, the
  Linux 2.6.30 source and the sources of the GPL tools that end up in the image (busybox, udhcp,
  wireless tools, squashfs + LZMA, with the host squashfs tools built). Removed: Edimax's
  application (`ipcam/`), the other SDK versions, the second toolchain, bootloaders, unrelated
  userland packages, and Realtek's unlicensed utilities and manual inside the toolchain. The
  modern-host build fixes (make ≥ 3.82 rule syntax, `sys/sysmacros.h`, a Perl `defined(@array)`)
  are applied.
- `/opt/ic3115w/stock/linux.bin`: the stock kernel, unchanged.
- `/opt/ic3115w/stock/rootfs-base.tar`: the stock root filesystem reduced by
  `tools/prune_rootfs.py` to an allow list of GPL/LGPL components (busybox, udhcpc, iwpriv, uClibc,
  libgcc) and device nodes. No vendor applications, assets or keys.

The build scripts find these through `IC3115W_GPL` and `IC3115W_STOCK` (`tools/env.sh`).

### Build steps

`tools/build_all.sh` (what `docker compose run --rm build` runs) does, in order:

1. `make -C firmware/camd test`: host unit tests.
2. `tools/build_camd.sh`: builds Mbed TLS for the device (out of tree in `build/mbedtls`), then
   camd and `getmac` with the rsdk toolchain, stripped.
3. `cd firmware/ui && npm ci && npm test && npm run build`: UI tests and the static bundle.
4. `tools/build_image.sh`: unpacks the pruned stock rootfs, prunes it again (harmless), overlays
   `firmware/rootfs/`, adds camd, `getmac` and the UI at `/www`, creates the MTD and `/dev/mem`
   nodes, repacks the squashfs (vendor LZMA) and packs the upgrade image with
   `tools/fwimage.py pack`. The rootfs is assembled under the container's `/tmp`, because device
   nodes cannot be created on the bind-mounted work tree.
5. `tools/fwimage.py split`: the TFTP files.

## 2. Tests on the host

No device is needed for either:

```sh
make -C firmware/camd test        # C: frame pool, config store, OTA validator, DHT insertion,
                                  #    wired validation, Wi-Fi test logic, WPA2 supplicant
cd firmware/ui && npm test        # vitest: config merge, upload/network validation and copy, MJPEG parser
```

The C tests compile the pure-logic modules with the host compiler; `test_wpa` is built with
`-std=gnu89 -Wdeclaration-after-statement` and ASan/UBSan and drives the supplicant through a
synthetic access point and a stubbed driver (`-DWPA_DRV_TEST`), including fuzzed Key Data. The
Mbed TLS crypto library is compiled for the host once into `tests/libmbedcrypto-host.a`.

## 3. Development loop

- **Deploy to a camera:** Settings → System → Firmware update in the UI, or from a shell
  `curl -u admin:<password> --data-binary @build/image/IC3115W_camd_upg.bin http://<camera>/api/ota`
  (`build/image/tftp/root.bin` works too and is smaller).
  Only the rootfs is rewritten; the camera verifies and reboots by itself (about 40 s). Keep a
  telnet session open, or the TFTP recovery files at hand, when touching anything that affects
  booting or the network.
- **UI without a camera:** `cd firmware/ui && npm run dev` serves the UI with a mock backend that
  models the API, the stream and every network state; see
  [`../firmware/ui/README.md`](../firmware/ui/README.md).
- **UI against a real camera:** add a Vite proxy for `/api`, `/stream.mjpg` and `/snapshot.jpg`
  (same README).
- **Device introspection:** `GET /api/debug` (admin) dumps `/proc/mtd`, `/proc/meminfo`,
  `/proc/mounts`, uptime and load; `system.syslog_remote` sends the camera's syslog to a host
  (`tools/syslog_recv.py` is a minimal collector); the telnet shell can be enabled in
  Settings → System.
- **Versions:** bump `CAMD_VER` in `firmware/camd/version.h` for every build you deploy; the
  version shows in `/api/status` and the About tab.

## 4. Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request (except changes limited to
Markdown, `docs/` and `LICENSE`): it builds the Docker image with its layers in the GitHub Actions
cache (so Edimax's files are downloaded only when the cache is cold), runs `tools/build_all.sh`
inside it, and uploads the upgrade image and the TFTP files as a workflow artifact.

## 5. Releases

`.github/workflows/release.yml` builds the same way, then runs `tools/package_release.sh` and
`tools/source_archive.sh` and publishes a GitHub release with:

| Asset | Content |
|---|---|
| `camd-<label>-upg.bin` | upgrade image |
| `camd-<label>-tftp-linux.bin`, `camd-<label>-tftp-root.bin` | TFTP recovery files (kernel and rootfs blocks) |
| `camd-<label>-source.tar.gz` | the complete source at that commit, submodules included (GitHub's automatic archives leave them out) |
| `ic3115w-gpl-sources.tar.xz` | source of the vendor GPL/LGPL components in the image (kernel, busybox, udhcp, wireless tools, uClibc, squashfs/LZMA): the pruned GPL release, without toolchain binaries |
| `SHA256SUMS` | checksums of all of the above |

`<label>` is `<CAMD_VER>` for a tagged release and `<CAMD_VER>-dev.<commit time>.g<hash>` for a
development build.

- **Every push to `main`** replaces the pre-release named `snapshot` with a build of that commit.
- **A tag `vX.Y.Z`** creates the release `vX.Y.Z`; the workflow checks that the tag matches
  `CAMD_VER` in `firmware/camd/version.h` and refuses otherwise. The release notes are the
  matching `## X.Y.Z` section of `CHANGELOG.md`, followed by installation instructions.

To release:

1. Bump `CAMD_VER` in `firmware/camd/version.h`.
2. Add or complete the version's section in `CHANGELOG.md`.
3. Commit, then `git tag vX.Y.Z && git push origin vX.Y.Z`.

## 6. Legacy local-layout scripts

`tools/build_gpl.sh` (non-interactive full vendor build), `tools/build_fw.sh`, `tools/test_fw.sh`
and `tools/build_debloat.sh` predate the build image. They expect the older local layout (the GPL
tarball extracted into `build/IC3115W_GPL`, the stock firmware under `resources/`) and are kept
for reference and for experiments with the vendor kernel; the release build does not use them.
