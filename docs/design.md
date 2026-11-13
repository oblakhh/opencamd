# Design

What opencamd is for, what it deliberately is not, the constraints the hardware imposes, the
decisions that follow from them, and how the daemon is put together. Implementation detail
(API, configuration, networking internals) is in [`firmware.md`](firmware.md); hardware facts
are in [`hardware.md`](hardware.md).

## 1. Goals and non-goals

**Goals**
- A reliable, local-only "printer cam": live MJPEG in a browser, snapshots, Home Assistant,
  PrusaLink/Prusa Connect-style snapshot uploads.
- A web UI a non-expert can use without documentation, including Wi-Fi setup.
- Wired and Wi-Fi networking that never strands the camera: wired preferred, Wi-Fi as automatic
  fallback, every risky change reversible by itself.
- Small attack surface: one daemon, one admin account, no cloud, no phone-home, nothing listening
  that is not needed.
- Field-updatable over the network, with the bootloader's TFTP mode as the recovery path.

**Non-goals**
- RTSP/ONVIF, H.264 (the hardware cannot encode it), recording, multi-camera.
- Wi-Fi AP or onboarding mode, WPS, enterprise 802.1X, 5 GHz, TKIP/WEP.
- WPA3-only networks (needs driver work; see [`../ROADMAP.md`](../ROADMAP.md)).
- TLS for the camera's own web UI/API (a reverse proxy can add it; HTTPS *uploads* are supported).
- Higher frame rates: the camera module delivers about 10 fps whatever the request.

## 2. Binding constraints

| Constraint | Consequence |
|---|---|
| **CPU: Lexra RLX4181** in the RTL8196C, a MIPS clone without the (then patented) unaligned load/store instructions. Mainline Linux, modern GCC/Clang and OpenWrt do not target it. | We are locked to the vendor's Linux 2.6.30 kernel and the rsdk GCC 3.4.6 / uClibc 0.9.30 toolchain with linuxthreads. C89 only. |
| ~400 MHz single core, no FPU, 32 MB RAM | No image processing; the daemon forwards the camera's JPEG frames. Thread count and stacks are kept small. |
| 4 MB SPI flash: 1 MiB kernel slot, 3 MiB rootfs slot, 16 KB settings region | No A/B firmware images; a read-only squashfs rootfs; configuration as small JSON. |
| Vendor kernel and Wi-Fi driver used unchanged | Driver bugs are worked around in userland (see [`firmware.md`](firmware.md#11-known-vendor-driver-issues)); anything that needs a kernel change is roadmap. |
| Camera module: UVC MJPEG, ~10 fps | Presets are honest about the rate; the vendor firmware did not do better. |

## 3. Key decisions

| Decision | Reason |
|---|---|
| **Vendor kernel, new userland.** | The kernel cannot be replaced (Lexra); the 2014 userland (cloud agent, UPnP, ASP web server, closed `ipcam`/`videoServer`) is where the bloat and the unreliability lived. |
| **MJPEG over HTTP.** | The module encodes JPEG in hardware, so the CPU only copies frames; every consumer (browsers, Home Assistant, VLC) speaks it. |
| **C, one multi-threaded daemon (`camd`), civetweb embedded.** | The only language the toolchain supports well; one address space makes frame sharing trivial and RAM use lowest. civetweb builds cleanly with GCC 3.4.6. Fault isolation is traded away; init respawns camd. |
| **camd-native WPA2 supplicant ("option U")** instead of the kernel's in-driver supplicant or the vendor's `auth` daemon. | The in-kernel supplicant cannot parse msg-3 Key Data from WPA2/WPA3 transition-mode APs (an RSNXE precedes the GTK), so it never installs the group key on such networks; it has further bugs that can only be fixed by rebuilding the kernel. The driver exposes a userland-supplicant interface, so a small supplicant in camd fixes all of them with a rootfs update and is unit-testable on the host. |
| **Single uplink with MAC clone, no bridge.** | Wired is preferred; Wi-Fi comes up only when the cable is gone, with eth0's MAC, so the LAN sees one device and DHCP usually hands out the same address. Never both at once (break before make), so no MAC flapping. The vendor's one-port bridge was dropped. |
| **Safe-apply for wired changes.** | A wired address change is tried for 90 s and reverted unless confirmed, so a typo cannot lock the camera out. Wi-Fi settings apply immediately because the cable stays up. |
| **A/B JSON settings with CRC in the vendor's settings region.** | Power-fail safe, human-readable, and physically separate from the rootfs so updates keep settings. |
| **OTA rewrites only the rootfs.** | The kernel never changes between opencamd releases; the config block and MAC/calibration area are never touched. The bootloader's TFTP mode remains the backstop. |
| **Stream and snapshot are open; everything else needs the admin password.** | Integrations need the video without credentials on a trusted LAN; settings, OTA and reboot are protected. |
| **Vue 3 SPA, no UI libraries.** | Modern browsers only, a small static bundle (about 70 KB gzipped) served from the rootfs. |
| **Image tools and the build live in `tools/` and `docker/`.** | Anyone can rebuild the exact image; nothing depends on a particular workstation. |

## 4. Architecture

### 4.1 camd threads

```
main thread       load config (flash) -> start capture, net, HTTP, NTP, uploader -> pause()
capture thread    owns /dev/video0: V4L2 MJPEG mmap loop, DHT insertion, publish into the frame pool;
                  reopens the device on errors or a 3 s stall
civetweb workers  6 threads: GET /stream.mjpg (one worker per stream, max 2 concurrent),
                  /snapshot.jpg, static UI, /api/* control plane
uploader thread   every N s: take the latest frame, POST/PUT it (HTTP or TLS); pauses during OTA
netcfg thread     applies a tried wired config 1 s after the request, reverts it at the 90 s deadline
net thread        uplink state machine (wired <-> Wi-Fi), polls the switch link state every 500 ms
wpa_drv thread    the WPA2 supplicant's I/O loop (20 ms tick), only while wlan0 is up
ntp thread        SNTP: retry every 60 s until the first sync, then hourly
reset-watch       polls the reset button every 500 ms; 10 s hold erases settings and reboots
```

A control-plane mutex serializes config reads/writes and capture reconfiguration; a second mutex
serializes OTA, reboot and factory reset. net.c owns wlan0; scans and connection tests borrow it
under a lock, and a running test is aborted if the uplink has to change.

### 4.2 Frame fan-out

Latest-frame semantics, not a queue: a small pool of slots (6 by default), each tagged with a
sequence number. Capture writes into a free slot and publishes it under a mutex; each stream
worker waits for a newer sequence and sends it. Readers pin the slot they are sending (refcount)
and the producer only reuses unpinned slots, so a slow client costs at most a dropped frame, never
a stalled capture. Snapshots are the latest frame; nothing is re-encoded.

### 4.3 Configuration store

JSON, serialized with cJSON, in the 16 KB "current settings" region at flash `0xC000`: two 8 KB
slots, each `magic "CAMD" | version | seq | length | CRC32 | json`. A save writes the slot that
does not hold the newest valid copy with `seq + 1`; a load takes the valid slot with the highest
`seq`. Writes go through `/dev/mtdblock0` (this kernel has no MTD character device) and are
refused below `0x8000`, so the bootloader and the MAC/calibration block can never be hit. Factory
reset overwrites both slot headers. Secrets (password hash, Wi-Fi passphrase) are stored but
never serialized for the API.

### 4.4 OTA

`POST /api/ota` receives the upgrade image (or just its rootfs block), validates it completely in memory (container, cvimg header,
squashfs 4.0/LZMA identity, length, 16-bit checksum), checks that the target partition is the
expected size and currently holds a squashfs, writes `/dev/mtdblock1`, reads it back to verify, and
resets via the `reboot(2)` syscall. The live rootfs is rewritten in place, so the window between
the first write and the reboot is kept short and the uploader is paused meanwhile. A failed write
leaves the camera in TFTP-recoverable state.

### 4.5 Security model

- One account, `admin`. The password is stored as an md5-crypt hash (`$1$…`, random salt) in the
  settings; the same hash is written to the ramfs `/etc/passwd`, so the optional telnet shell uses
  the admin password too. The default is `admin`.
- Browser sessions: `POST /api/login` sets `camd_session` (random 128-bit token, `HttpOnly`,
  `SameSite=Strict`); at most 8 sessions are kept, oldest evicted. Scripts and integrations can use
  HTTP Basic instead.
- CSRF: every state-changing route also checks the `Sec-Fetch-Site` header; browsers send it and
  cannot forge it, so cross-site requests are rejected with 403, while non-browser clients (which
  omit it) pass. This covers the Basic-auth vector that `SameSite=Strict` does not.
- Secrets never leave the device: `GET /api/config` returns `passphrase_set` instead of the
  passphrase and omits the password hash; the passphrase is handed to the Wi-Fi driver only as
  derived keys (never via `iwpriv` arguments, environment or logs).
- The video stream and snapshot are deliberately unauthenticated.
- Everything is plain HTTP; the camera is meant for a trusted LAN. Telnet is off by default.
