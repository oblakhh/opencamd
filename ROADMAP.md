# Roadmap

Ideas and planned work, roughly grouped. Nothing here is promised; items move when they're
investigated. Technical background for most networking items is in
[`docs/firmware.md`](docs/firmware.md) and [`docs/engineering-notes.md`](docs/engineering-notes.md).

## Kernel
Today the vendor kernel (Linux 2.6.30, Realtek SDK) runs unchanged. Owning it is the gate for
several items below; the first step is proving that a kernel built from the GPL source boots
in place of the stock one (TFTP recovery makes this safe to try).
- **Patch the Wi-Fi driver's EAPOL buffer overflow:** `rtl8192cd` copies received 802.1X frames
  into a fixed 1550-byte buffer without a length check (`8192cd_rx.c`); an oversized frame from the
  access point overflows kernel memory. Present in the stock firmware too; documented in
  [`docs/firmware.md`](docs/firmware.md#11-known-vendor-driver-issues).
- **Other driver/kernel fixes found along the way:** e.g. the in-kernel WPA supplicant's handshake
  parser (now bypassed by camd's own supplicant), reliable link/carrier reporting for eth0,
  `/proc` read handlers that return nothing for short reads, `CONFIG_MTD_CHAR`.
- **A later 2.6-line kernel:** evaluate moving to a newer 2.6 kernel (and what it would take to
  carry the Realtek SoC, switch and Wi-Fi support forward). Realistically constrained by the Lexra
  CPU core, which mainline compilers don't target without patches.

## WPA3
- **WPA3-Personal (SAE) and WPA3-only networks.** Needs, in the driver: SAE authentication frames
  (passed up to camd, which would run SAE — mbedTLS already provides the P-256 math) and **protected
  management frames** (802.11w: IGTK, BIP-CMAC in software since the hardware key engine lacks it,
  SA-Query). Requires kernel ownership.
- **Gate: CPU cost.** The SoC is a ~400 MHz core without FPU. Before committing, measure SAE's
  elliptic-curve operations and per-frame BIP-CMAC overhead on the device — join time and steady-state
  load must stay acceptable alongside video capture and streaming.
- Transition-mode (WPA2/WPA3) networks already work today.

## Userland modernisation
- **Toolchain:** move off gcc 3.4.6 / uClibc 0.9.30 to a newer compiler and libc (e.g. uClibc-ng),
  which needs Lexra support. Unlocks C99 and current libraries.
- **busybox 1.13 → current:** includes a maintained DHCP client (replacing standalone udhcp 0.9.8,
  which ignores SIGTERM and writes stale PID files) and the usual missing applets.
- **Remote access:** replace telnet with SSH (dropbear), with key-based login.
- **HTTPS for the web UI/API** using the bundled mbedTLS — subject to the same CPU-cost check as WPA3.
- **Supplicant as a separate process?** It currently lives inside camd. A split would isolate crashes
  from the video stream at the cost of an IPC/status interface and handing secrets to a second
  process; the handshake logic is already I/O-free, so a split is cheap if wanted.
- Keep the third-party submodules (civetweb, cJSON, mbedTLS) current.

## Camera & integrations
- Investigate the camera module's controls (exposure, white balance) via UVC.
- **RTSP / RTP-JPEG** and possibly **ONVIF Profile S**, for NVRs such as Synology Surveillance Station.
- **Home Assistant:** tighter integration (e.g. MQTT discovery, status sensors) beyond the MJPEG camera.

## Reliability
- **Group-key renewals occasionally fail:** about 1 in 100 renewals does not complete and the access
  point disconnects the camera, which rejoins within a second. Needs a targeted run with the key
  exchange logged at the failing moment.
- **Rare short stalls:** HTTP requests occasionally time out (about once an hour on Wi-Fi), at
  times followed by a failed upload. Cause not yet known (Wi-Fi link or camd).
- Long-running soak tests (Wi-Fi rekeys, AP restarts, roaming between mesh nodes, out-of-range
  periods) as part of each release.
- On-device health reporting (memory, restarts, link history) in the UI.
