# opencamd — open replacement firmware for the Edimax IC-3115W

**opencamd** is a small, hardened replacement firmware for the **Edimax IC-3115W** IP camera,
whose vendor firmware was abandoned in 2014. It turns the camera into a simple, reliable network camera:
a clean MJPEG live stream and snapshots, a modern web UI, wired and Wi-Fi networking with
automatic failover, and an uploader that pushes snapshots to services such as Prusa Connect.
At its core is `camd`, the camera daemon; binaries and release files carry that name.

> **Status: working, single-unit tested.** It runs on real hardware and is used daily, but it
> has only been verified on one camera. Flashing custom firmware can brick your device; read
> [Install & recovery](#install--recovery) and [`docs/recovery.md`](docs/recovery.md) first.

## Features

**Video**
- MJPEG live stream (`/stream.mjpg`) and full-quality snapshots (`/snapshot.jpg`) taken from the
  live stream; the legacy paths `/mjpg/video.mjpg` and `/snapshot.cgi` still work.
- Works with anything that consumes MJPEG/JPEG over HTTP (Home Assistant, VLC, browsers).
- Resolutions from 160×120 to 1280×1024. The camera module tops out at about 10 fps, a hardware
  limit the stock firmware shares.

**Web UI**
- Single-page app: live view plus a settings dialog (Video, Network, Uploads, System, About).
- Current desktop and mobile browsers, including Safari.

**Networking**
- Wired Ethernet with DHCP or a static address. Address changes use a **90-second safe-apply**:
  unless you confirm the new settings, they revert by themselves.
- **Wi-Fi client** (2.4 GHz) for **WPA2-PSK and WPA2/WPA3 transition mode** networks (AES/CCMP),
  mesh systems included. The WPA2 handshake is done by camd's own supplicant.
- **Automatic wired-over-Wi-Fi failover:** the cable is preferred; when it is unplugged the camera
  switches to Wi-Fi within seconds using the *same* MAC address, so it usually keeps its IP, and
  hands back to the cable when it returns.
- "Test connection" checks Wi-Fi credentials without disturbing the wired connection and reports
  a wrong password, an unknown network or a missing DHCP answer.

**Snapshot uploads**
- Push a snapshot every N seconds to any HTTP(S) endpoint as a multipart POST or a raw PUT, with
  custom headers. HTTPS is built in (mbedTLS, TLS 1.2; redirects are followed). Tested with
  **Prusa Connect**; also suits PrusaLink/OctoPrint-style receivers.

**System**
- Over-the-air updates; the image is validated before anything is written, and settings survive.
- Power-fail-safe settings storage (two alternating copies with checksums).
- Admin password (salted hash, shared with the optional telnet shell), session cookie with
  `SameSite=Strict`, CSRF protection, secrets never returned by the API.
- Factory reset from the UI, or **hold the reset button for 10 s** while the camera is running.
- NTP time sync, local and remote syslog; telnet is off by default and can be enabled in
  Settings → System.

## Hardware

| | |
|---|---|
| Camera | Edimax IC-3115W |
| SoC | Realtek RTL8196C (Lexra RLX4181, big-endian, no FPU), 32 MB RAM, 4 MB SPI flash |
| Wi-Fi | Realtek RTL8188CN, 2.4 GHz 802.11b/g/n, 1T1R |
| Ethernet | 10/100 via the SoC's switch |
| Camera module | USB UVC (MJPEG), `064e:c221` |
| Kernel | Vendor Linux 2.6.30 (Realtek SDK), kept unchanged |

Details: [`docs/hardware.md`](docs/hardware.md).

## Install & recovery

**Back up your camera's full flash before anything else** and keep the copy somewhere safe.

Firmware files are attached to the [releases](../../releases) on GitHub:
`camd-<version>-upg.bin` (upgrade image) and `camd-<version>-tftp-linux.bin` /
`camd-<version>-tftp-root.bin` (TFTP recovery files).

- **First install:** use the bootloader's **TFTP recovery mode**: hold the reset button while
  powering on, connect a PC by cable, and upload the two TFTP files. Step by step:
  [`docs/recovery.md`](docs/recovery.md).
- **Updates afterwards:** Settings → System → Firmware update in the web UI with
  `camd-<version>-upg.bin`, or `POST /api/ota` (see [`docs/firmware.md`](docs/firmware.md)).
- **If something goes wrong:** TFTP recovery works as long as the bootloader is intact; the
  firmware never writes the bootloader or the factory calibration/MAC area.
- **Forgot the password:** hold the reset button for about 10 s while the camera is running.
  Settings are erased and the camera restarts with the defaults. (Holding it at power-on enters
  TFTP recovery instead.)

Defaults after flashing or a factory reset: DHCP, with `192.168.2.3` as the fallback address;
user `admin`, password `admin`. **Change the password right away.**

## Integrations

- **Home Assistant:** *MJPEG IP Camera* with stream `http://<camera>/stream.mjpg` and still
  image `http://<camera>/snapshot.jpg` (the *Generic Camera* integration works the same way).
  No authentication is needed for the stream and snapshot.
- **Prusa Connect / PrusaLink:** Settings → Uploads. Enter the snapshot URL and the
  `Fingerprint` / `Token` headers from Prusa's "add camera" dialog, one `Key: Value` per line;
  method `PUT`. The camera then uploads a fresh snapshot every N seconds.
- **VLC and other players:** open `http://<camera>/stream.mjpg` as a network stream.

## Building

```sh
git clone --recursive <repo-url>   # or, in an existing clone: git submodule update --init
docker compose run --rm build      # unit tests, camd, web UI, firmware image -> build/image/
```

The first run builds the container, which downloads the Edimax GPL release and stock firmware
(checksum-verified, used only locally). Everything else, including tests, the development loop,
CI and releases: [`docs/building.md`](docs/building.md).

## Documentation

| Document | Contents |
|---|---|
| [`docs/design.md`](docs/design.md) | Goals, constraints, key decisions, architecture and security model |
| [`docs/hardware.md`](docs/hardware.md) | SoC, flash map, settings blocks, GPIO, UART, bootloader, Wi-Fi chip and camera module |
| [`docs/firmware.md`](docs/firmware.md) | What is in the image, boot, configuration, HTTP API, networking and Wi-Fi internals, OTA, logging |
| [`docs/building.md`](docs/building.md) | Build environment, tests, development loop, CI and the release procedure |
| [`docs/recovery.md`](docs/recovery.md) | Bootloader TFTP recovery and the hardware factory reset |
| [`docs/engineering-notes.md`](docs/engineering-notes.md) | Hard-won findings about this hardware and its vendor software |
| [`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md) | Coding guidelines, tests, workflow and licensing of contributions |
| [`firmware/ui/README.md`](firmware/ui/README.md) | The web UI: design system, development server with mock backend |
| [`ROADMAP.md`](ROADMAP.md) | Planned and possible future work |

## Repository layout

| Path | Contents |
|---|---|
| `firmware/camd/` | The camera daemon (C89): capture, HTTP API, uploader, networking, WPA2 supplicant |
| `firmware/ui/` | Web UI (Vue 3 + TypeScript + Vite), with a mock server for development |
| `firmware/rootfs/` | Init scripts and network helpers overlaid onto the root filesystem |
| `firmware/camprobe/` | V4L2 probe used during bring-up to enumerate the camera module |
| `firmware/third_party/` | civetweb, cJSON, Mbed TLS (git submodules, pinned to release tags) |
| `docker/` | The build container (Dockerfile and the scripts that prepare the vendor inputs) |
| `tools/` | Build scripts, firmware image tools and bench helpers |
| `docs/` | Documentation (see above) |
| `.github/workflows/` | CI and release automation |

## Known limitations

- 2.4 GHz only; no WPA3-only networks (they need SAE and protected management frames, which the
  vendor Wi-Fi driver lacks; see the roadmap). WPA2/WPA3 transition mode works.
- About 10 fps maximum (camera module limit).
- The web UI and API are plain HTTP on the LAN (no TLS on the device yet); the optional telnet
  shell is unencrypted, so leave it off unless you need it.
- The vendor kernel and Wi-Fi driver are used unchanged and carry their own known issues, see
  [`docs/firmware.md`](docs/firmware.md#11-known-vendor-driver-issues).
- Tested with one camera and one family of access points (AVM FRITZ! mesh, WPA2/WPA3 transition
  mode) plus a phone hotspot; other hardware may behave differently.

## Roadmap

See [`ROADMAP.md`](ROADMAP.md).

## Credits & licenses

Built on the Realtek SDK and Edimax GPL release (GPL-2.0), [civetweb](https://github.com/civetweb/civetweb)
(MIT), [cJSON](https://github.com/DaveGamble/cJSON) (MIT), [Mbed TLS](https://github.com/Mbed-TLS/mbedtls)
(Apache-2.0 OR GPL-2.0-or-later; used here under GPL-2.0-or-later) and [Vue](https://vuejs.org) (MIT).

opencamd's own code — the camd daemon, the web UI, scripts and tools — is licensed under the
**GNU General Public License v2.0 or later** (`SPDX-License-Identifier: GPL-2.0-or-later`); see
[`LICENSE`](LICENSE). Third-party code (the git submodules) keeps its own license (see each directory under
`firmware/third_party/`). Firmware images also contain unmodified GPL components from the vendor
release (Linux kernel, busybox and others); their source is the Edimax GPL release, and each
release on GitHub ships it as `ic3115w-gpl-sources.tar.xz`.

This project is not affiliated with or endorsed by Edimax or Realtek.
