# Recovery: bootloader TFTP mode and factory reset

The RTL8196C bootloader has a built-in TFTP recovery mode. It is the first-install path and the
way back from a bad flash; it works as long as the bootloader itself is intact, which opencamd never
writes. Verified on a camera (bootloader source: `bootcode_rtl8196c_98/boot/init/eth_tftpd.c` and
`utility.c` in the GPL release).

## How it works

- Entered by holding the **reset button while powering on** (about 10 s), or by pressing ESC on
  the serial console.
- In recovery the **camera is a TFTP server at `192.168.1.6`** (fixed, since camd leaves the
  vendor's settings block empty). Your PC is the TFTP client and **uploads** (PUT) the files.
- The bootloader decides what to burn from each file's **internal signature**, not its name:
  `cr6c`/`cs6c` = kernel, `r6cr` = root filesystem. A file with the **boot-code signature
  overwrites the bootloader: never upload one.**
- It validates signature and checksum before writing, burns, and reboots.
- It wants the raw blocks, not the `*_upg.bin` upgrade image. Releases ship them as
  **`camd-<version>-tftp-linux.bin`** (kernel) and **`camd-<version>-tftp-root.bin`** (rootfs).
  From a locally built image: `python3 -I tools/fwimage.py split IC3115W_camd_upg.bin <dir>` →
  `linux.bin`, `root.bin`.

## What you need

- An Ethernet cable from the PC straight to the camera (or through a dumb switch), off your main
  LAN so nothing else answers on `192.168.1.x`.
- The PC on a static address `192.168.1.116`, netmask `255.255.255.0` (any `192.168.1.x` except
  `.6` works; `.116` matches the bootloader's static ARP entry).
- A TFTP client in **binary/octet** mode.
- A full backup of the camera's flash, kept somewhere safe, as the last resort.

## Step 1: enter recovery (read-only)

1. Set the PC's static address and connect the cable.
2. Power the camera on **while holding the reset button**, for about 10 s.
3. From the PC, confirm the camera answers at `192.168.1.6`: `ping 192.168.1.6` (it may not reply;
   an ARP entry is enough), or a TFTP **GET** of any name to see the server respond.

If it responds, the recovery channel works and a bad flash is always recoverable. Do this once
before the first flash, without uploading anything.

## Step 2: flash

With the camera in recovery mode, upload the two files in binary mode (syntax varies by client;
this is the usual Unix `tftp`):

```
tftp -m binary 192.168.1.6 -c put camd-<version>-tftp-linux.bin
tftp -m binary 192.168.1.6 -c put camd-<version>-tftp-root.bin
```

The bootloader checks each file and reboots after burning. The camera then comes up on DHCP
(fallback `192.168.2.3`), user `admin`, password `admin`. Because the kernel never changes between
opencamd releases, uploading only the root file is enough once an opencamd kernel block is in place; both
files are always safe.

To go back to the vendor firmware, upload the kernel and root blocks split from a stock image or
from your backup the same way.

## Rules

- Only upload kernel (`cr6c`) and root (`r6cr`) blocks. Never a bootloader image.
- Never write flash below `0x010000` (bootloader) or the hardware settings at `0x6000`
  (MAC address, Wi-Fi calibration); camd refuses to, and so should you.
- Keep the full flash backup off the machine you experiment with.

## Factory reset / forgotten password

A forgotten admin password is recovered **without a PC**: **hold the reset button for about 10 s
while the camera is running** (not at power-on, which enters TFTP recovery). camd polls the
button (GPIOA3) every 500 ms; a sustained hold erases the settings and reboots to the defaults
(DHCP with `192.168.2.3` fallback, `admin`/`admin`).

Safety: camd honours a hold only after it has seen the button released once since boot, so a
stuck or misread input can never loop-wipe, and it logs the button's startup reading
(`reset-watch: started (button reads up …)`).

Note that a TFTP reflash does **not** reset the password: the settings live at `0xC000`, below
the regions the bootloader burns. The button (or erasing the settings from a serial console) is
the only path that needs no password.
