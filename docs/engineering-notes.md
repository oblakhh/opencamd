# Engineering notes

Hard-won findings about this hardware and its vendor software, one paragraph each, dated when
they were established. Most are also reflected in the code comments and in
[`firmware.md`](firmware.md); this file is the place to look when something behaves strangely.

## Wi-Fi

**CCK response SIFS (2026-11-11).** The chip's built-in MAC table sets `RESP_SIFS_CCK`
(register `0x63C`) to `0x100a` (10 µs). Because the PHY signals end-of-CCK-frame late, the
camera's CCK ACKs land late on air; AVM FRITZ! access points (a repeater, before and after a
firmware update, and a second radio) reject them, retry every frame to the camera four times and
never send EAPOL msg-1, so no handshake ever starts. A phone hotspot tolerated it. Joins with the
port open: `0x100a` 0/5, `0x0808` 5/5, `0x0606` 5/5, `0x0404` 5/5; `0x0606` was chosen. The
register is reset by every `ifconfig wlan0 up`, so it is written right after each one
(`iwpriv wlan0 write_reg w,63c,0606`). Earlier "random" successes were runs where a previous
test's value was still in place.

**Board RF calibration (2026-11-11).** The HW settings block at `0x6000` holds the Wi-Fi
calibration in nested record `0x80C9`: `0x0385` `pwrlevelCCK_A`, `0x0387` `pwrlevelHT40_1S_A`,
`0x038A` `pwrdiffHT20`, `0x038B` `pwrdiffOFDM`, 14 bytes per channel each. The stripped vendor
application used to apply them at boot; the driver defaults are all-zero power and 2T2R. The chip
is an RTL8188CN, **1T1R**, so `MIMO_TR_mode=4` must be set too. `getmac -rf` prints them.

**opmode 8 is station, 16 is AP (2026-11-10).** `8192cd.h`: `WIFI_STATION_STATE = 0x08`,
`WIFI_AP_STATE = 0x10`. Early attempts set `opmode=16` from the vendor application's strings and
ran wlan0 as an access point, which "associated" but could never complete a client handshake.
`/proc/wlan0/mib_operation` shows `0x409` in station mode.

**MIBs only take effect at `ifconfig up`.** The driver initialises its PSK state when the
interface comes up (`rtl8192cd_init_sw` → `psk_init`), so every `set_mib` must happen while wlan0
is down. With `802_1x=1` and no RSN IE set, `ifconfig up` does not start a join; the join starts
at `SET_RSNIE`.

**`encmode=2` is required for CCMP.** `encmode` writes `dot11PrivacyAlgrthm` directly (2 = TKIP
marker, 4 = CCMP); `SET_RSNIE` is ignored for none/WEP, and the vendor's own configuration uses 2
with `wpa2_cipher=8` carrying the real cipher. Both values pass the EAPOL gate.

**`iwpriv wlan0 read_reg` prints decimal.** The returned bytes are printed as decimal numbers in
big-endian order, e.g. `0x100a` shows up as `16 10`.

**`/proc/wlan0/*` returns nothing for reads shorter than 1 KiB (2026-11-11).** Measured with
`dd`: `bs=1023` gives 0 bytes, `bs=1024` gives all 27 lines of `sta_info`. A 1023-byte read in
the port-open wait made every failover wait the full 15 s before DHCP; all readers now use
buffers of 2 KB or more.

**Transition-mode APs place an RSNXE before the GTK KDE (2026-11-11).** On a WPA2/WPA3 mesh
(SAE-H2E) the 4-way handshake completes and the PTK is installed, but the in-kernel supplicant
installs no GTK (`/proc/wlan0/mib_gkeytbl`: `keyInCam no`): its msg-3 Key Data parser accepts only
`[RSN IE][WPA IE?][GTK KDE]` and stops at the RSNXE. Without the group key no broadcast or
multicast can be decrypted, so no DHCP offer and no ARP; a static address does not help. The same
AP in standalone mode with its "improve compatibility" option on omits the RSNXE and works. This
is why camd has its own supplicant, which parses Key Data as an element loop (observed live:
`30 f4 KDE:1` = RSNE, RSNXE, GTK KDE).

**In-kernel / vendor `auth` supplicant bugs (review, 2026-11-10).** Same code in `8192cd_psk.c`
and `users/auth`: A1 Key Data parser (above); A2 group-key reply with Descriptor Type 254 in WPA2;
A3 no Key Descriptor Version 3 / 802.11w; A4 msg-1 Key Data must be empty or exactly one PMKID
KDE; A5/B1/B6 unchecked 16-bit Key Data length into fixed `decrypted_data[128]`/`GTK[32]`
(post-MIC crash); B2 vacuous ANonce check; B3 key reinstallation on a retransmitted msg-3 (KRACK);
B4 SNonce from `time()` plus uninitialised stack; B5 `PasswordHash` return ignored (silent zero
PMK for long passphrases); B7 sends from a `SIGALRM` handler. The crypto primitives themselves
(PBKDF2, PRF-512, RFC 3394 unwrap, HMAC) are correct and big-endian safe.

**Driver requests always return 0.** The `0x89ff` daemon-request ioctl reads the request in place
and reports success regardless; key installs and the port state must be confirmed in
`/proc/wlan0/sta_keyinfo`, `mib_gkeytbl` and `sta_info`. The event queue holds 4 items. `KeyRSC`
in `SET_KEY` is ignored. `DISCONNECT_REQ` in client mode only sends a disassociation and frees the
station entry without restarting the join; re-issuing `SET_RSNIE` restarts it.

**Wrong passphrase looks like repeated msg-1 (2026-11-11).** With a wrong passphrase the AP
re-sends msg-1 every 1.0 s instead of answering msg-2 with msg-3 (3/3 runs); with the right one, 0
re-sends in 12/12 joins (msg-3 follows within ~30 ms). One re-send can be a lost msg-2; two in a
row next to the AP cannot. Some APs deauthenticate after the first refusal instead; both
patterns count.

**AP cool-down after a rejected handshake (2026-11-11).** Right after rejecting a handshake the
tested AVM access points associate but send no msg-1 to *any* station, also with a fresh random
MAC: 2/2 immediate retries refused, 2/2 accepted after 30 s. The Wi-Fi test therefore answers
`busy` with `retry_in` for 30 s after a `wrong_passphrase`. Separately, a scan occasionally
misses the AP and the first join after an OTA sometimes associated without msg-1, so a test
re-joins once at 9 s.

**PBKDF2 on this CPU.** Deriving the PMK (4096 iterations) takes 310–480 ms; it is cached in RAM
per (SSID, passphrase). Join to association takes 0.6 s, or 5.6 s when the driver scans first;
msg-1 to port open takes about 30 ms.

**The vendor's `at_ss` / `at_join` are factory-test commands.** They associate without engaging
the driver's WPA state machine, so the AP never starts a handshake. `at_ss` is still fine for a
scan (`/proc/wlan0/SS_Result`), which gives SSID, BSSID and channel but no RSSI or security.

## Networking and boot

**`udhcpc` is standalone udhcp 0.9.8 (2026-11-10).** busybox has no `udhcpc` applet in the
stock build. Its option string is `c:fbH:h:i:np:qr:s:v`: no `-t`, `-T` or `-A`. The first camd
init passed `-t 8 -T 3 -A 5`, so getopt printed the usage and exited 0 before any DISCOVER, and
the camera never got a lease. With `-b` it writes its PID file before backgrounding (the file
holds the dead parent's PID) and it ignores `SIGTERM`, so the scripts stop it by command-line
match per interface with `SIGKILL`. Its `deconfig` event wiped the interface to 0.0.0.0; the event
script re-applies `192.168.2.3` there.

**The Ethernet driver's default MAC (2026-11-10).** The RTL819x NIC driver does not read the
MAC from flash; it defaults to `00:12:34:56:78:90`, and the vendor's closed application set the
real one. The real address is LZSS-compressed in the `COMPHS` block at `0x6000` (record `0xC9`);
reading raw bytes there finds nothing. `getmac` decodes it at boot.

**The switch needs the vendor bring-up (2026-11-10).** Without `echo 1 > /proc/sw_nat` and the
`mib_vlan` entries from the stock `rcS`, eth0 is up but carries no traffic. The vendor's one-port
bridge `br0` was dropped afterwards; the address lives on eth0.

**eth0's carrier never changes.** `/sys/class/net/eth0/carrier` and `SIOCGIFFLAGS` stay the
same with the cable in or out. The switch's port status registers (`PSRP0..4` at physical
`0x1B804128`, bit 4) reflect the link and are read through `/dev/mem`.

**camd must run under `::respawn` (2026-11-10).** Started from `rcS`, it occasionally did not
come up during the early-boot storm; `inittab` now starts it after `rcS` and restarts it if it
exits.

**Stock web upgrade rejects scripted uploads (2026-11-09).** The vendor's
`/form/formUpgradeFirmware.cgi` accepts a browser upload (checks the `IC3115W` header, the XOR
checksum and the size; no version gate) but the closed CGI dispatcher answers "Page Not Found" to
a scripted POST. Use a browser for the stock upgrade page, TFTP for everything else.

## Flash, reboot, kernel

**No MTD character device (2026-11-10).** The kernel has `CONFIG_MTD_CHAR` off and only
`CONFIG_MTD_BLOCK`: no `/dev/mtdN`, no `MEMERASE`. Writes go to `/dev/mtdblockN`, which erases a
sector on write (the vendor's `flash` tool does the same), followed by `fsync`. `flash_erase()` is
therefore a no-op on the block device; a factory reset wipes by overwriting the slot headers. The
4 KB erase size keeps the config at `0xC000` clear of the bootloader.

**Reboot (2026-11-10).** `reboot(2)` called directly resets the stock kernel cleanly. Going
through busybox `reboot`/init hangs in the shutdown (`umount -a -r` of the squashfs root). Poking
the bootloader's watchdog sequence (`CDBR = 0x2`, `WDTCNR = 0x100`) through `/dev/mem` froze the
system instead: `CDBR` is the live kernel timer divider. Never touch it from a running kernel.

**Device nodes need a mknod-capable directory.** The rootfs is assembled under the container's
`/tmp`, because `mknod` on the bind-mounted work tree silently drops device nodes and the image
would boot without `/dev/video0`.

**The GPL tarball must be extracted on a case-sensitive filesystem.** A copy unpacked on macOS
lost 416 entries to case collisions (`xt_MARK.h` vs `xt_mark.h`, empty directories). Always
extract fresh from the tarball; the build container does.

**OTA timing.** A clean rootfs update takes about 40 s: SPI-NOR write plus readback of ~650 KB,
then the reboot. The rootfs is rewritten while mounted, so the uploader pauses and the reboot
follows immediately.

**Kernel log ring is 4 KB** (`LOG_BUF_SHIFT=12`); anything older than a few driver messages is
gone. Remote syslog is the reliable way to watch a boot.

## Camera and streaming

**About 10 fps is the module's ceiling (2026-11-09).** Direct V4L2 capture measured a flat
~10 fps at every resolution whatever frame interval was requested (10 → 10.0, 25/30 → ~10.1), and
the vendor firmware's own stream gave ~7–8 fps at 1280×960 (59 frames in ~8 s, 174 KB each). The
UVC descriptors' 30/25/20/15 fps are nominal. Only a different camera module would change this.

**UVC frames carry no Huffman tables (2026-11-10).** The module's MJPEG frames have the
markers SOI, APP0, DQT, DQT, SOF0, SOS and no DHT. Chrome and Firefox supply the standard tables;
Safari refuses to decode. camd inserts the standard 420-byte DHT segment into every frame at
capture (the same tables ffmpeg's `mjpeg2jpeg` filter adds).

**Safari and MJPEG (2026-11-10).** Safari does not render `multipart/x-mixed-replace` in an
`<img>` yet keeps the connection open, and will not stream such a body through `fetch()`. Those
idle connections each pinned a civetweb worker and starved the control plane. Fixes: at most 2
concurrent streams with 6 workers, and `/stream.mjpg?raw=1`, which serves the same framing as
`application/octet-stream` for the SPA's own parser.

## Bootloader and settings

**TFTP recovery is signature-driven and does not touch the settings (2026-11-09).** Button at
power-on, server at `192.168.1.6`, client expected on `192.168.1.x`; `cr6c`/`cs6c` burns the
kernel, `r6cr` the rootfs, a boot-code signature the bootloader. A reflash leaves `0xC000` alone,
so it does not reset a forgotten password. Proven end to end: upload, checksum check, burn,
reboot, `md5sum /dev/mtdblock1` equal to the uploaded image.

**Settings blocks are LZSS-compressed.** `0x6000`/`0x8000`/`0xC000` start with `COMPHS` /
`COMPDS` / `COMPCS` and a 12-byte header, then Okumura LZSS (N=4096, F=18, THRESHOLD=2). An
early init script looked for a raw `H6` tag there and fell back to a hardcoded MAC; `getmac` and
`tools/mib.py` decode the block properly.
