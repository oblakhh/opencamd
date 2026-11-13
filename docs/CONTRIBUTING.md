# Contributing

Guidelines for changes to opencamd: the camd daemon, the web UI, the scripts and the documentation. Build, test and
release mechanics are in [`building.md`](building.md).

## 1. Target constraints

Everything that runs on the camera is compiled with the vendor toolchain: **GCC 3.4.6, uClibc
0.9.30, linuxthreads** (not NPTL), big-endian MIPS without unaligned access, no FPU, 32 MB RAM,
about 3 MB of rootfs. Mainline compilers cannot target the Lexra core, so there is no way around
this toolchain; see [`design.md`](design.md#2-binding-constraints).

Practical consequences:
- No C99 (`-std=gnu89`); no VLAs, no `//` comments, no mixed declarations and code, no
  designated initializers, no `inline` outside what GCC 3.4 accepts.
- No unaligned loads: do not cast byte buffers to wider integer pointers; use `memcpy` or shifts.
- Few threads with modest stacks; no signal-handler I/O; `pthread` only.
- Memory is tight: fixed-size buffers with explicit bounds, no large allocations per request.
- Everything that ends up in the image must be GPL-compatible and free of vendor binaries.

## 2. C guidelines (as practised in `firmware/camd/`)

- Each file starts with the SPDX header (§7), a one-line purpose and a pointer to its header.
- Tabs for indentation; K&R braces (function braces on their own line); `snake_case` for
  functions and variables, `UPPER_CASE` for macros and enum constants; file-local helpers are
  `static`; module-global state is `static` and prefixed `g_`.
- Declarations at the start of a block. When a declaration is needed mid-function, open a new
  `{ … }` block, as the existing code does.
- Fixed-size `char` buffers filled with `snprintf`; copies with `snprintf("%s")` or `memcpy` with
  explicit lengths; every length that comes from the network or the driver is checked against the
  frame and the buffer.
- Pure logic goes in modules without I/O (`wpa.c`, `netcfg_validate`, `ota_validate`,
  `wifi_test_decide`, `framebuf.c`, `mjpeg.c`) so it can be unit-tested on the host; I/O lives in
  thin wrappers (`wpa_drv.c`, `flash.c`, the scripts).
- Shared state is protected by a named mutex and the comment says which one; locks are held
  briefly and never across script execution or network I/O.
- Logging through `LOGI`/`LOGW`/`LOGE` (`log.h`). Never log a passphrase, a key, a PMK or a
  password hash. Secrets are zeroed after use (`mbedtls_platform_zeroize`, `memset`).
- Scripts under `firmware/rootfs/etc/` are run by camd with `execv`/`execve` and validated
  arguments only; they are busybox `ash` scripts (see §4) and must stay idempotent.
- Driver facts and magic numbers (register addresses, MIB names, ioctl layouts) carry a comment
  naming the SDK source they come from.
- Build with `-Wall -Wextra` clean on both the host and the cross compiler.

## 3. Tests

- `make -C firmware/camd test` must pass before a change is deployed or pushed. Add a test to the
  matching `tests/test_*.c` for every bug fix in a pure module; the supplicant tests
  (`tests/test_wpa.c`) are built with `-std=gnu89 -Wdeclaration-after-statement` and
  ASan/UBSan, which also catches C89 violations early.
- `cd firmware/ui && npm test` for the UI; `npm run typecheck` (part of `npm run build`).
- Changes that touch booting, the network bring-up, authentication or flash writes are tested on
  a camera with a telnet session already open and the TFTP recovery files at hand. Never deploy a
  network-affecting change without the safe-apply path in place.
- CI runs both test suites and the full build on every push and pull request.

## 4. Device shell limits

Scripts run on the stock busybox (2013 build): `ash` without `$(( ))` arithmetic; no `tail`,
`sed`, `hexdump`, `devmem`, `nslookup`; `head` lacks `-N`; `ps` output is the busybox format;
`udhcpc` is standalone udhcp 0.9.8 (only `-c -f -b -H -h -i -n -p -q -r -s -v`, ignores
`SIGTERM`). Reads from `/proc/wlan0/*` must be at least 1 KiB (shorter reads return nothing).

## 5. Web UI

Vue 3 + TypeScript + Vite, no UI, icon or animation libraries; design tokens and primitives in
`src/style.css`; pure helpers in `src/utils/` with vitest tests; the mock backend in
`mock/server.ts` must model every state a component can show. Copy is plain English that tells the
user what happens next (see the Network tab's rules in [`../firmware/ui/README.md`](../firmware/ui/README.md)).
Keep the bundle small (currently ~70 KB gzipped, target well under 150 KB). The API contract is
[`firmware.md`](firmware.md#4-http-api); change the backend and `src/types.ts` together.

## 6. Third-party code

`firmware/third_party/` holds civetweb, cJSON and Mbed TLS as shallow git submodules pinned to
release tags. Do not patch them in place; configure through compile flags (`Makefile`,
`tools/build_camd.sh`). Update by moving the submodule to a newer tag, rebuilding, and running the
tests; mention the new version in the changelog. Anything from the Edimax GPL release stays in the
build container and in the `ic3115w-gpl-sources` release asset, never in this repository.

## 7. Licensing

The project's own code is **GPL-2.0-or-later**; contributions are accepted under the same terms.
Every source file (C, headers, shell, Python, TypeScript, Vue, CSS, HTML, Makefiles, Dockerfiles,
workflows) carries the header in the file's comment syntax, followed by a short purpose line:

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * <module> — <what this file does>.
 */
```

```sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
```

Vue single-file components and HTML use an `<!-- … -->` block with the same two lines. Third-party
code keeps its own license and headers.

## 8. Workflow

- Branch from `main`; open a pull request. CI must be green.
- Commit messages: `area: summary` in the imperative, e.g. `wifi: re-join when no msg-1 arrives`,
  `ui: network tab countdown`, `docs: …`, `ci: …`; the body explains why and what was verified on
  the device.
- A change that alters behaviour on the camera gets a line in `CHANGELOG.md` under the upcoming
  version, and `CAMD_VER` is bumped for every build that is deployed.
- Never commit vendor files, flash dumps, packet captures, passphrases or anything from your own
  network (addresses, MACs, SSIDs). `build/`, `resources/`, `backup/`, `vendor/` and `dist/` are
  ignored for that reason.
