# Hardware

Facts about the IC-3115W that the firmware depends on. Sources: the Edimax GPL release
(SDK headers, `rtl_gpio.c`, bootloader source), the stock firmware v2.03 image, and readings
from a running unit (`dmesg`, `/proc`, flash dumps).

## 1. Board

| Item | Detail |
|---|---|
| SoC | Realtek RTL8196C (rev B): Lexra RLX4181 core, MIPS-like, big-endian, no FPU, ~400 MHz. `/proc/cpuinfo` reports `RTL8196B` revid 4 while the kernel applies the 8196C PHY patch; normal for this part. |
| RAM | 32 MB SDRAM (Winbond W9825G6KH); roughly 26 MB usable by Linux |
| Flash | Macronix MX25L3205D, 4 MB SPI NOR, 4 KB sectors, mapped at `0xBFE00000` |
| Ethernet | 10/100 through the SoC's built-in switch; the port's link state is visible only in the switch's port status registers (see §4) |
| Wi-Fi | Realtek RTL8188CN (PCI `10ec:8191`), 802.11b/g/n 2.4 GHz, **1T1R**; vendor driver `rtl8192cd` with station and AP modes. No 802.11w/PMF, no SAE, no way to inject management frames from userland. |
| Camera | SuYin "USB 2.0 Camera", USB UVC 1.00, VID:PID `064e:c221`, module `HF1016-P80A-SS01E-VS-R01.00.00`, hardware MJPEG (and YUYV), isochronous with 11 bandwidth alt-settings; `/dev/video0` via the in-kernel `uvcvideo` |
| Kernel | Linux 2.6.30.9, Realtek RLX SDK v2.3, squashfs 4.0 + LZMA rootfs |
| Toolchain | `rsdk-1.3.6-4181-EB-2.6.30-0.9.30`: GCC 3.4.6, uClibc 0.9.30, linuxthreads |

### Camera module frame rates

Modes (MJPEG and YUYV): 160×120, 176×144, 320×240, 352×288, 640×480, 1280×720, 1280×800,
1280×960, 1280×1024. The UVC descriptors advertise 30/25/20/15/10 fps, but the module delivers
**about 10 fps at every resolution** (about 7–8 fps at 1280×960 with large, high-quality
frames). This was measured both with direct V4L2 capture and from the vendor firmware's own
stream, so it is a module limit, not a driver problem. YUYV is bandwidth-limited to 7.5 fps at
1280 pixels wide. The frames carry no Huffman tables (DHT); camd inserts the standard ones.

## 2. GPIO, LEDs, button

From `rtl_gpio.c` (port A, `IC3015` board branch; LEDs are active-low):

| GPIO | Function | Notes |
|---|---|---|
| A3 | Reset button (shared with WPS) | active-low; camd polls it through `/dev/mem` (`GPIO_BASE 0xB8003500`, registers CNR `+0x00`, DIR `+0x08`, DAT `+0x0C`, bit 3) |
| A11 | Power LED | `/proc/gpio` LED id 0 |
| A10 | Network LED | `/proc/gpio` LED id 1 |
| mux | `0xB8000040 \|= 0x0034000F` | pin-mux for these pins |

`/proc/gpio` accepts `"<led> <state>"` with state 0 off, 1 on, 2 blink, 3 data blink, 4 WPS
blink, 9 stop the timer. camd's init blinks the power LED while booting, turns it on when boot is
done, and turns the network LED on when an address is configured. There is no IR LED or IR-cut
control in this driver.

Button semantics: held **at power-on** the bootloader enters TFTP recovery (§6); held for
**10 s while running** camd erases the settings and reboots (factory reset).

## 3. UART

`ttyS0`, 38400 8N1, kernel command line `console=ttyS0,38400 root=/dev/mtdblock1`. The header is
not populated on the board. Both the stock firmware and opencamd respawn a root shell on the console
without a login. Nothing in the normal install or recovery flow needs the serial port.

## 4. Switch port status

eth0 sits behind the SoC's switch, and the Ethernet driver's carrier flag never changes. The
switch's port status registers do: `PSRP0..4` at physical `0x1B804128` (switch core
`0xBB800000` + `0x4100` + `0x128`, one 32-bit register per port), bit 4 = link up. camd maps that
page from `/dev/mem` and treats "any port up" as cable present.

## 5. Flash map

| Range | Content | MTD |
|---|---|---|
| `0x000000–0x005FFF` | bootloader (`boot.bin`, v1.1f) | mtd0 `boot+cfg+linux` (`0x000000`, size `0x110000`) |
| `0x006000–0x007FFF` | HW settings: MAC addresses, Wi-Fi RF calibration. **Never written.** | mtd0 |
| `0x008000–0x00BFFF` | vendor default settings (unused by camd) | mtd0 |
| `0x00C000–0x00FFFF` | current settings: **camd's A/B JSON config** | mtd0 |
| `0x010000–0x10FFFF` | kernel, 1 MiB slot | mtd0; also mtd2 `ipcam` (`0x010000`, size `0x3F0000`) |
| `0x110000–0x3FFFFF` | rootfs squashfs, 3,080,192 bytes max | mtd1 `root fs` (`root=/dev/mtdblock1`); also mtd2 |

The kernel is built with `CONFIG_MTD_BLOCK` only (`CONFIG_MTD_CHAR` off): there is no
`/dev/mtdN` and no `MEMERASE`; all flash writes go through `/dev/mtdblockN`, which erases a sector
on write. camd refuses any write below `0x8000`.

## 6. Bootloader

The RTL8196C bootcode (`bootcode_rtl8196c_98` in the GPL release) looks for the kernel at
`0x10000–0x40000` and the rootfs at `0x110000–0x130000` in `0x1000` steps, by signature.

**TFTP recovery mode** is entered by holding the reset button at power-on (or pressing ESC on the
serial console). The camera then runs a TFTP server at `192.168.1.6` (hardcoded while the
current-settings block holds no vendor config, which is the case with camd) and accepts a client
on `192.168.1.x`; `192.168.1.116` matches its static ARP entry. The bootloader decides what to
burn from the uploaded file's **internal signature**, not its name:

| Signature | Burned as |
|---|---|
| `cr6c` / `cs6c` | kernel (code image region, `0x10000`) |
| `r6cr` | root filesystem (`0x110000`) |
| boot-code signature | **the bootloader itself: never upload one** |

It validates the signature and checksum before writing and reboots afterwards. TFTP recovery
never touches the settings block, so it does not reset a forgotten password; the running-system
button hold does. Runbook: [`recovery.md`](recovery.md).

## 7. Upgrade image format

The web/OTA upgrade image (`*_upg.bin`, the vendor's format, produced by `tools/fwimage.py`):

| Offset | Content |
|---|---|
| `0x000000` | 128-byte header `IC3115W  EDIMAX`, zero padded |
| `0x000080` | kernel block: 16-byte Realtek header (`cr6c`, start address, burn address `0x10000`, length, big-endian) then `length` bytes whose big-endian 16-bit word sum is 0; zero padded to 1 MiB |
| `0x100080` | rootfs: squashfs whose superblock word at `+8` holds the filesystem length, followed by a 2-byte checksum so that the first `len + 640 + 2` bytes sum to 0 (cvimg `r6cr` block without its 16-byte header) |
| last byte | XOR of every byte after the 128-byte header |

Everything from `0x80` on is what the bootloader writes at `0x10000`. `fwimage.py split` turns the
image into the two TFTP files (`linux.bin` with its `cr6c` header, `root.bin` with an `r6cr`
header); `fwimage.py verify` checks all of the above.

## 8. Settings blocks

The three vendor settings blocks (`0x6000` HW, `0x8000` default, `0xC000` current) are
LZSS-compressed: a 12-byte header `COMPRESS_MIB_HEADER_T { char sig[6]; u16 compRate; u32 compLen; }`
with signature `COMPHS` / `COMPDS` / `COMPCS`, then LZSS data (Okumura, N=4096, F=18,
THRESHOLD=2, the `Decode()` of the SDK's goahead utility). camd only ever reads the HW block and
uses the current-settings region for its own store.

The decoded HW block is `H601` + u16 length, then a record stream `id (u16 BE) + len (u16 BE) +
data`. The MAC records are `id (1 byte) + 0x0006 + 6 bytes`: `0xC9` = eth0, `0xCA`/`0xCB` = WLAN
(Realtek `00:E0:4C` defaults). The Ethernet driver does **not** read the MAC from flash (it
defaults to `00:12:34:56:78:90`); `getmac` decodes the block at boot and sets it. `tools/mib.py`
decodes a dump on the host.

### RF calibration

Nested record `0x80C9` of the HW block holds the per-board Wi-Fi calibration as 14-byte
per-channel arrays, which the vendor application applied at boot and the driver defaults to zero:

| Record | Driver MIB |
|---|---|
| `0x0385` | `pwrlevelCCK_A` |
| `0x0387` | `pwrlevelHT40_1S_A` |
| `0x038A` | `pwrdiffHT20` |
| `0x038B` | `pwrdiffOFDM` |

`getmac -rf` prints them as `mib=hex` lines; `/etc/wifiapply.sh` feeds them to
`iwpriv wlan0 set_mib` together with `MIMO_TR_mode=4` (the chip is 1T1R; the driver's default is
2T2R) before every bring-up. Without them the radio transmits at zero power.
