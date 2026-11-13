# Firmware internals

What the image contains, how it boots, how it is configured, the HTTP API, and how networking,
Wi-Fi, OTA and the rest work. The code is authoritative: `firmware/camd/http.c` for the API,
`net.c`/`netcfg.c`/`wifi.c` for networking, `firmware/rootfs/etc/` for init and the helper
scripts. Design rationale is in [`design.md`](design.md), hardware facts in
[`hardware.md`](hardware.md).

## 1. Image contents

The upgrade image is the **stock kernel, unchanged** (Linux 2.6.30.9 with the vendor drivers)
plus a rootfs of about 650 KB (limit 3,080,192 bytes), assembled by `tools/build_image.sh`:

| From the stock firmware (allow list in `tools/prune_rootfs.py`) | Ours |
|---|---|
| busybox (shell, `ifconfig`, `route`, `telnetd`, `syslogd`, `klogd`, …), standalone `udhcpc` 0.9.8, `iwpriv`, uClibc 0.9.30, `libgcc`, device nodes | `/bin/camd`, `/bin/getmac`, the web UI at `/www`, `/etc/inittab`, `/etc/init.d/rcS`, `/etc/netapply.sh`, `/etc/wifiapply.sh`, the `udhcpc` event scripts, `/etc/passwd` → `/var/passwd`, `/etc/version` |

No vendor application, web page, script, cloud client or key is kept.

## 2. Boot

`/etc/inittab`: `rcS` at sysinit, then `/bin/camd -r /www` under `::respawn` (started after rcS
completes, restarted if it exits), a root shell on `ttyS0`, `reboot` on Ctrl-Alt-Del.

`/etc/init.d/rcS`, in order: mount `/proc`, `/sys`, a ramfs on `/var` and `devpts`; power LED
blinking; `syslogd` (200 KB ring in `/var/log/messages`) and `klogd`; the switch bring-up the
vendor did (`echo 1 > /proc/sw_nat`, the `mib_vlan` entries, without which eth0 carries no
traffic); the unit's real MAC on eth0 from `getmac` (the driver's default is not unique); `lo`;
eth0 up with the `192.168.2.3` fallback address; the default `/var/passwd`; `udhcpc`
on eth0 in the background (`-b`, hostname `ic3115w`); power LED solid. camd then loads its
configuration, starts `telnetd` if telnet is enabled, and applies a static wired address if one is configured; otherwise the DHCP lease
from rcS stands.

## 3. Configuration

One JSON document, stored as described in [`design.md`](design.md#43-configuration-store).
Compiled-in defaults in `config.c`:

```jsonc
{
  "video":   { "width": 1280, "height": 960, "quality": 80, "target_fps": 10 },
  "system":  { "hostname": "ic3115w", "ntp": "pool.ntp.org", "syslog_remote": "",
               "syslog_level": 6, "telnet": false },          // + pw_hash (never returned)
  "upload":  { "enabled": false, "url": "", "interval": 10, "method": "POST",
               "field": "file", "headers": "" },
  "network": { "wired": { "mode": "dhcp", "ip": "", "netmask": "", "gateway": "", "dns": "" },
               "wifi":  { "enabled": false, "ssid": "", "passphrase_set": false } }  // + passphrase
}
```

- `video`: one of the module's modes (160×120 … 1280×1024), `quality` 1–100, `target_fps` 1–30
  (the module delivers ~10 regardless). A resolution change restarts capture.
- `upload`: `url` must start with `http://` or `https://`; `interval` 1–3600 s; `method` `POST`
  (multipart, form field `field`) or `PUT` (raw JPEG body); `headers` one `Key: Value` per line.
  A `User-Agent: Camd/<version>` is added unless you set one.
- `network`: only ever changed through `POST /api/network` (validated, safe-applied); the
  `network` section of a `PUT /api/config` body is ignored. `wired.dns` holds up to two servers
  separated by a space or comma.
- Secrets: `system.pw_hash` (md5-crypt) and `network.wifi.passphrase` are stored in flash and
  never serialized for the API; the API shows `passphrase_set` instead.

## 4. HTTP API

Auth: **open** = no credentials; **admin** = session cookie from `/api/login` *or* HTTP Basic
(`admin:<password>`). Every admin route that changes state also applies the CSRF check (a
browser's `Sec-Fetch-Site` must be `same-origin` or `none`; 403 otherwise). Unauthenticated admin
calls get 401 with `WWW-Authenticate: Basic`. Responses are JSON unless noted; errors are
`{"ok":false,"error":"…"}`.

| Method | Path | Auth | Request / response |
|---|---|---|---|
| GET | `/`, `/assets/…` | open | the web UI (static files from `/www`) |
| GET | `/stream.mjpg` (alias `/mjpg/video.mjpg`) | open | `multipart/x-mixed-replace; boundary=camdframe`. With `?raw=1`: the same framing as `application/octet-stream` plus `X-Stream-Boundary: camdframe` (Safari cannot stream multipart through `fetch()`). At most **2** concurrent streams; a third gets `503` with `Retry-After: 5`. Ends after ~15 s without frames. |
| GET | `/snapshot.jpg` (alias `/snapshot.cgi`) | open | the latest JPEG (`image/jpeg`); `503 no frame yet` if none arrives within 3 s |
| GET | `/api/status` | open | `{name, model, version, uptime, time, video{width,height,fps,quality,alive}, net{iface,ip,mac,link,mode,uplink,wired_link,psr[5],revert_in,wifi{state,ssid,bssid,ip,rssi}}, upload{enabled,ok,last_code,count}, admin}` — `link` is the interface kind that carries the address, `uplink` the state machine's choice (`wired`/`wifi`/`none`), `mode` the *committed* wired mode, `revert_in` seconds left in a safe-apply window (0 = none), `psr` the raw switch port status registers, `admin` whether the caller is authenticated |
| POST | `/api/login` | open | `{"password":"…"}` → `200 {ok:true}` + cookie `camd_session` (HttpOnly, SameSite=Strict); `401` otherwise |
| POST | `/api/logout` | open | clears the session |
| GET | `/api/config` | admin | the configuration without secrets (see §3) |
| PUT/POST | `/api/config` | admin | partial JSON merged into `video`/`system`/`upload`, validated as a whole (`400 bad json` / `invalid video settings` / `invalid upload settings`), applied and saved; returns the new config |
| POST | `/api/password` | admin | `{"password":"…"}` (1–63 chars) → new hash saved, telnet `passwd` rewritten |
| POST | `/api/network` | admin | `{"wifi":{enabled,ssid,passphrase?}}`: validated (SSID 1–32 chars, passphrase 8–63 printable ASCII; an omitted/empty passphrase keeps the stored one), saved and applied immediately → `{ok:true,"wifi":"applying"}`. `{"wired":{mode,ip,netmask,gateway,dns}}`: `409` unless the cable is the uplink; `400` with a message on validation failure; otherwise applied 1 s later for a 90 s trial → `{ok:true,mode,ip,revert_in:90}` (`ip` is `""` for DHCP). Re-applying during a trial keeps the original fallback. |
| POST | `/api/network/confirm` | admin | commits the tried wired config → `{ok:true,confirmed:true}`; `409 nothing to confirm` |
| POST | `/api/wifi/scan` | admin | → `[{ssid,bssid,channel}, …]` after ~5 s; no signal or security info (the driver's scan result lacks them); SSIDs may be empty (hidden) or repeat (several BSSIDs) |
| POST | `/api/wifi/test` | admin | `{"ssid":"…","passphrase":"…"?}` (omitted = stored). Synchronous, about 30 s at most → `200 {ok, result, ssid, ip?, join_ms?, retry_in?, error?}` with `result` one of `ok`, `wrong_passphrase`, `not_found`, `no_dhcp`, `busy`, `error` (§5.3). `409` unless the cable is the uplink; `400` on validation or if no passphrase is known. |
| POST/PUT | `/api/ota` | admin | body = the upgrade image `*_upg.bin`, or the rootfs block (§7). `400 image too small` (< 642 bytes) or `400 upload incomplete: received X of Y bytes`, `413` (larger than a full upgrade image), `409` if an update/reboot is in progress, `500` with the validation or flash error; `200 {ok:true,bytes,rebooting:true}` and the camera reboots |
| POST | `/api/reboot` | admin | `{ok:true,msg:"rebooting"}`; reboots 1 s later |
| POST | `/api/factory-reset` | admin | erases the settings and reboots to defaults |
| GET | `/api/debug` | admin | plain text: stream count and `/proc/mtd`, `/proc/meminfo`, `/proc/mounts`, `/proc/uptime`, `/proc/loadavg`, `/etc/version` |

## 5. Networking

### 5.1 Wired

Address on eth0 directly (no bridge; wlan0 is never bridged). DHCP is started by rcS; a static
configuration is applied by camd through `/etc/netapply.sh static <ip> <mask> <gw|-> <dns1|-> <dns2|->`.
While no DHCP lease is held, and as an alias `eth0:0` in static mode, the camera also answers at
**`192.168.2.3`** (rescue address for a direct cable). DHCP sends the configured hostname.

**Safe apply.** `POST /api/network {"wired":…}` validates (canonical dotted quads, contiguous
netmask /1–/30, IP not network/broadcast, gateway in the subnet and not the camera, up to two DNS
servers), then applies the change **1 s later** (so the HTTP response gets out) and starts a
**90 s** timer. `POST /api/network/confirm` within that window saves the config; otherwise the
previous configuration is re-applied. Nothing unconfirmed is ever written to flash, so a reboot
also reverts. `revert_in` in `/api/status` drives the UI's countdown banner.

**409 rules.** Wired settings can only be changed while the cable is the uplink; a Wi-Fi test can
only run while the cable is the uplink *and* has link; a second OTA/reboot/factory reset while one
is in progress is refused.

### 5.2 Uplink manager (`net.c`)

Wired is preferred; Wi-Fi is a **cold standby** (wlan0 down while the cable is the uplink, so the
LAN sees one identity and the shared MAC is never on both links). The loop polls the switch port
status every 500 ms:

| Event | Action |
|---|---|
| Cable gone for **5 s** (10 ticks) and Wi-Fi enabled | `netapply.sh down` (eth0 keeps only `192.168.2.3`), `/var/run/uplink` = `wlan0`, Wi-Fi up **with eth0's MAC**, supplicant, wait for the port (≤ 15 s, abortable), DHCP on wlan0 (default route and DNS only because wlan0 is the uplink) |
| Cable back and stable for **10 s** (20 ticks) | Wi-Fi down first (break before make), `/var/run/uplink` = `eth0`, the committed wired config re-applied |
| Wi-Fi settings changed while on Wi-Fi | re-join with the new settings, or, if Wi-Fi was disabled, `uplink: none` |
| No uplink, cable absent, Wi-Fi (re)enabled | bring Wi-Fi up |

Measured on the test network: switch-over about 5 s after the pull plus a few seconds to join and
get a lease (about 7–13 s outage), same IP thanks to the shared MAC; hand-back after 10 s without
a visible gap. `/api/status.net.iface/ip/mac` follow the uplink.

### 5.3 Wi-Fi test (`POST /api/wifi/test`)

Checks credentials while the cable stays up. wlan0 is prepared with a **locally administered MAC
(`02:00:00:00:00:01`)**, never the eth0 MAC, joins the given SSID with camd's supplicant, and on
success runs a one-shot DHCP probe (`udhcpc -n -q`, no hostname, configures nothing); wlan0 is
taken down again afterwards. Thresholds: no association after 9 s → one re-join; never associated
and no handshake after 18 s → `not_found`; join phase capped at 22 s; DHCP probe ≤ 10 s.

| `result` | Meaning | Typical time |
|---|---|---|
| `ok` | handshake completed and a lease was obtained (`ip`) | 3–10 s |
| `wrong_passphrase` | the AP refused our handshake (§6.2) | 3–8 s |
| `not_found` | the SSID never associated | ~21 s |
| `no_dhcp` | joined, but no DHCP answer | join time + ~10 s |
| `busy` | wlan0 is in use (failover, scan, another test) or the AP's cool-down after a rejected handshake is running: `retry_in` seconds | at once |
| `error` | anything else; `error` says `aborted: …` when the cable went away during the test | |

After a rejected handshake the tested AVM access points accept no new handshake from *any*
station for about 30 s, so camd answers `busy` with `retry_in` for 30 s after a
`wrong_passphrase`.

### 5.4 Wi-Fi status

`/api/status.net.wifi.state`: `off` (disabled), `standby` (enabled, cable is the uplink: wlan0
down, or only up for a test/scan), `connecting` (wlan0 up), `associated`, `connected` (the
802.1X port is open), `wrong_passphrase` (the supplicant keeps being refused). `rssi` is the
driver's 0–100 value, `-1` when unknown.

## 6. Wi-Fi station internals

### 6.1 Driver recipe (`/etc/wifiapply.sh`)

Set with wlan0 **down** (MIBs only take effect at the next `ifconfig up`), then up:

```
iwpriv wlan0 set_mib MIMO_TR_mode=4            # chip is 1T1R
iwpriv wlan0 set_mib <RF calibration from getmac -rf>   # pwrlevelCCK_A, pwrlevelHT40_1S_A, pwrdiffHT20, pwrdiffOFDM
iwpriv wlan0 set_mib opmode=8 802_1x=1 wsc_enable=0 authtype=2 encmode=2 psk_enable=0 \
                     wpa2_cipher=8 band=3 oprates=4095 basicrates=15 ssid=<ssid>
ifconfig wlan0 [hw ether <mac>] up
iwpriv wlan0 write_reg w,63c,0606              # CCK response SIFS, reset by every 'up'
```

`opmode=8` is station (16 is AP). `802_1x=1 psk_enable=0` routes EAPOL to the driver's event
queue instead of the in-kernel supplicant. `encmode=2` is required although the cipher is CCMP:
the driver ignores `SET_RSNIE` for none/WEP. With `802_1x=1` and no RSN IE yet, `ifconfig up`
does **not** start a join, so the SIFS fix is always in place before the first authentication
frame. The join starts when camd issues `SET_RSNIE`. Register `0x63C` defaults to 10 µs, which
makes the camera's CCK ACKs land late; the tested AVM access points then retry every frame and
never start the handshake (phone hotspots tolerate it). `0x0606` joined 5/5 against 0/5.

A scan (`iwpriv wlan0 at_ss`, result in `/proc/wlan0/SS_Result`) with wlan0 down uses a
non-existent SSID and the test MAC, so it can never join anything or collide with the cloned MAC.

### 6.2 Supplicant (`wpa.c`, `wpa_drv.c`)

`wpa.c` is pure logic (no I/O, clock or globals): PMK = PBKDF2-HMAC-SHA1(passphrase, SSID, 4096)
via Mbed TLS, PTK = PRF-384 (KCK | KEK | TK), HMAC-SHA1-128 MIC, RFC 3394 AES unwrap (own
implementation on `mbedtls_aes`). Only Key Descriptor Type 2 / Version 2, AKM PSK, CCMP pairwise
and group. The caller feeds EAPOL frames and association changes in and executes the returned
actions in order: send reply, install PTK, install GTK, open port. The MIC is verified before Key
Data is decrypted; the replay counter becomes the floor only after a verified MIC; Key Data is
parsed as an element loop (RSNE/RSNXE/unknown skipped, GTK KDE taken with its key index, IGTK
ignored); a retransmitted msg-3 or group message never reinstalls a key.

`wpa_drv.c` runs it in one thread with a 20 ms tick and no signals: drains the driver's event
queue, polls the association every 60 ms, sends EAPOL on a send-only `PF_PACKET` socket, installs
keys, opens the port, caches one PMK (PBKDF2 takes 300–500 ms on this CPU) keyed by
SHA-256(SSID‖0‖passphrase), draws SNonces from `mbedtls_ctr_drbg`. Watchdogs: associated but no
msg-1 for 4 s → re-join with backoff 4, 8, … 60 s; not connected for 60 s → re-join. Re-issuing
`SET_RSNIE` is used instead of `DISCONNECT_REQ`, which in client mode only drops the station
without restarting the join. On disassociation the group key is deleted and all key state cleared.
All key material is zeroed with `mbedtls_platform_zeroize`; logs carry element IDs only.

**Wrong passphrase** is declared when, within one association and before any PTK, the AP re-sent
msg-1 **twice** with rising replay counters instead of answering msg-2 with msg-3, or two
driver-observed disassociations followed our msg-2, and no msg-3 MIC ever verified or failed in
this run. Forged msg-1 frames can therefore force the verdict during a join (a detection DoS of
the same class as a forged deauth) but can never make a working link report it: one verified
msg-3 clears it for good. Measured: with a wrong passphrase the AP re-sends msg-1 every 1.0 s;
with the right one, 0 re-sends in 12/12 joins.

### 6.3 Driver interface (`8192cd_security.c/.h`, MIPS32 big-endian layout)

All requests: `ioctl(sock, 0x89ff /* SIOCDEVPRIVATE+0xf */, struct iwreq)` with
`u.data.pointer` → a struct whose first byte is the event ID. The driver reads the request in
place and **always returns 0**; success is verified in `/proc/wlan0/sta_info` (`ieee8021x_ctrlport`),
`sta_keyinfo` and `mib_gkeytbl` (`keyInCam`).

| Request | ID | Layout (byte offsets) | Effect |
|---|---|---|---|
| `DOT11_REQUEST` | 2 | buffer ≥ 1560 | dequeue one event; `EventId = 1` when empty |
| `DOT11_EAP_PACKET` (event) | 41 | 0 id, 1 more, 2 u16 len, 4 frame[1550] = Ethernet header + EAPOL | every EAPOL frame from the AP |
| `DOT11_INIT_QUEUE` | 18 | 2 bytes | flush (the queue holds only 4 items) |
| `DOT11_SET_RSNIE` | 14 | 0 id, 1 more, 2 u16 Flag=1, 4 u16 len, 6 IE[128], 134 MAC[6]; 140 bytes | stores our RSN IE and starts the join if wlan0 is up |
| `DOT11_STA_QUERY_BSSID` | 36 | 0 id, 4 u32 IsValid, 8 BSSID[6]; 16 bytes | association state (no association/deauth events are queued in client mode) |
| `DOT11_SET_KEY` | 11 | 0 id, 4 u32 KeyIndex, 8 u32 KeyLen, 12 KeyType (0 group / 1 pairwise), 13 EncType (4 = CCMP), 14 MAC[6], 20 RSC[8], 28 Key[64]; 92 bytes | pairwise: MAC = BSSID, index 0; group: MAC = ff:…:ff, index = GTK key ID (old and new GTK coexist during a rekey). **RSC is ignored.** |
| `DOT11_SET_PORT` | 12 | 0 id, 1 PortStatus, 2 PortType, 3 MAC[6]; 9 bytes | opens `ieee8021x_ctrlport` |
| `DOT11_DELETE_KEY` | — | | group key removal on disassociation |

Our RSN IE: `30 14 01 00 00 0F AC 04 01 00 00 0F AC 04 01 00 00 0F AC 02 00 00` (WPA2-PSK, CCMP
pairwise and group, no PMF). EAPOL TX: `socket(PF_PACKET, SOCK_RAW, 0)`, `sll_protocol 0x888E`,
our own Ethernet header. The driver sends EAPOL unencrypted until a pairwise key is installed, so
msg-4 goes out before `SET_KEY`; the group msg-2 is CCMP-protected as required. Struct sizes and
offsets are compile-time asserted in `wpa_drv.c`.

## 7. OTA

`POST /api/ota` takes the image as the raw request body (this is what the web UI sends):
the **upgrade image** `camd-<version>-upg.bin`, the cvimg `root.bin` with its 16-byte `r6cr`
header (`camd-<version>-tftp-root.bin`), or the bare checksummed squashfs. For an upgrade image
camd first checks the container (header and zero padding, trailing XOR-8 over the body, `cr6c`/
`cs6c` kernel block) and then takes the rootfs from it. The kernel never changes between opencamd
releases and is not flashed, so only the rootfs partition (mtd1, `0x110000`) is rewritten; the settings block and
the MAC/calibration area are physically untouched and settings survive.

Validation before any write (`ota_validate`, host-tested): the upgrade-image container if present, optional `r6cr` header with burn
address `0x110000`, squashfs magic, plausible length (≥ 256 KB, ≤ partition), zero 16-bit word
sum over the checksummed range, squashfs 4.0 with LZMA, plausible `bytes_used`. Then: the target
device must be the `0x2F0000` partition and currently hold a squashfs; write, `fsync`, read back
and compare; an upload that ends before `Content-Length` bytes arrived (stalled connection) is
rejected without writing anything; the uploader is paused meanwhile; the HTTP response is sent and the camera resets
with `reboot(2)` 0.4 s later. A clean update takes about 40 s including the reboot. A write that
fails midway leaves the rootfs incomplete and the error says so: recover over TFTP
([`recovery.md`](recovery.md)).

## 8. Factory reset

Two paths, same effect: `POST /api/factory-reset` (admin), or holding the reset button for
**10 s while the camera is running**. camd polls GPIOA3 every 500 ms, honours a hold only after it
has seen the button released once (a stuck or misread input can never loop-wipe) and logs the
startup reading. Both overwrite the headers of both config slots and reboot; the camera comes back
on DHCP with the `192.168.2.3` fallback and `admin`/`admin`. TFTP recovery does **not** reset the
settings.

## 9. Logging, time

camd logs through `syslog(3)` (facility daemon) to busybox `syslogd`, which keeps a 200 KB
ring in `/var/log/messages` (ramfs, lost on reboot). `system.syslog_remote` (`host[:port]`) sends
a copy to a remote collector; `system.syslog_level` is the level. The kernel log ring is 4 KB.
The clock is set by camd's SNTP client from `system.ntp` (retry every 60 s until the first
success, then hourly); `/api/status.time` is UTC. Internal intervals use `CLOCK_MONOTONIC`, so the
jump from 1970 to the current time at the first sync does not affect timeouts.

## 10. Telnet

A busybox `telnetd` root shell, user `admin` with the admin password (camd rewrites `/var/passwd`
from the stored hash at startup and on every password change). It is unencrypted. The default is
**off**; enable it under Settings → System (`system.telnet`) only when you need it. camd starts or
stops `telnetd` at startup and whenever the setting is saved (in a child that closes camd's file
descriptors first), so the change takes effect immediately. The device
shell is the stock busybox: `ash` without `$(( ))` arithmetic, no `tail`, `sed`, `hexdump` or
`devmem`, `head` without `-N`; `wget`, `tftp`, `ftpget`, `iwpriv`, `ifconfig`, `route`, `udhcpc`,
`ps`, `dd`, `md5sum` are there.

## 11. Known vendor-driver issues

The kernel and the `rtl8192cd` Wi-Fi driver are used unchanged; these are their known problems and
how camd deals with them. Fixing them properly needs a kernel built from the GPL source
([`../ROADMAP.md`](../ROADMAP.md)).

- **EAPOL buffer overflow (unfixed, present in the stock firmware too).** `8192cd_rx.c` (around
  lines 4507–4517 and 4667–4677) copies a received 802.1X frame of `pskb->len` bytes into the
  fixed `Eap_packet->packet[1550]` buffer without a length check. An oversized `0x888E` frame that
  the driver accepts as coming from the BSSID overflows kernel heap, before camd sees anything and
  also with the in-kernel supplicant. The fix is a clamp to `sizeof(packet) - 14`.
- **In-kernel WPA-PSK supplicant** (`8192cd_psk.c`, the same code as the vendor `auth` daemon),
  bypassed by camd's own supplicant: msg-3 Key Data parser accepts only
  `[RSN IE][WPA IE?][GTK KDE]`, so an RSNXE (sent by WPA3-transition APs with SAE-H2E) before the
  GTK means no group key, hence no broadcast/multicast, no DHCP; group-key replies with Descriptor
  Type 254 in WPA2; no Key Descriptor Version 3 / 802.11w; msg-1 Key Data must be empty or one
  PMKID KDE; unchecked 16-bit Key Data length into fixed buffers (post-MIC crash/DoS); vacuous
  ANonce check; key reinstallation on a retransmitted msg-3 (KRACK); SNonce from `time()` and
  uninitialised stack; `PasswordHash` errors ignored (silent zero PMK); sends from a `SIGALRM`
  handler (`auth`).
- **No SAE, no PMF (802.11w), no IGTK/BIP, no management-frame injection from userland**: WPA3-only
  networks cannot be joined.
- **`/proc/wlan0/*` read handlers return 0 bytes for reads shorter than 1 KiB**; camd reads with
  buffers ≥ 2 KB.
- **eth0's carrier never changes**; link state is read from the switch's port status registers.
- **Driver requests always return 0**; results must be verified in `/proc`. The event queue holds
  4 items. `KeyRSC` is ignored (group replay protection is whatever the hardware does). The driver
  prints installed keys in `/proc/wlan0/mib_gkeytbl` and `sta_keyinfo` (root only).
- **CCK response SIFS** (`0x63C`) defaults to a value that makes ACKs late for strict access
  points; rewritten after every `ifconfig wlan0 up` (§6.1).
- **`udhcpc` is standalone udhcp 0.9.8** (busybox's applet is not built): no `-t/-T/-A`
  options, `-b` writes its PID file before backgrounding (the file holds a dead PID), and it ignores
  `SIGTERM`; the scripts stop it by command-line match with `SIGKILL`.
- **Reboot**: `reboot(2)` called directly works; going through busybox `reboot`/init (which
  unmounts the squashfs root) hangs, and poking the watchdog `CDBR` register freezes the kernel
  timer instead of resetting.
