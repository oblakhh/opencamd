# tools/

Build scripts, firmware image tools and bench helpers. Nothing here writes to a camera by itself.
The build is described in [`docs/building.md`](../docs/building.md); every script's header
comment has its exact usage.

| Tool | Purpose |
|---|---|
| `build_all.sh` | The full build run by `docker compose run --rm build`: host tests, camd, web UI, image, TFTP split |
| `build_camd.sh` | Cross-compile Mbed TLS (out of tree) and camd with the rsdk toolchain |
| `build_image.sh` | Assemble the rootfs (pruned stock base + `firmware/rootfs/` + camd + UI) and pack the upgrade image |
| `env.sh` | Sourced by the build scripts: toolchain and vendor input locations (`IC3115W_GPL`, `IC3115W_STOCK`) |
| `package_release.sh` | Release assets in `dist/`: renamed images, TFTP files, the GPL source archive |
| `source_archive.sh` | Complete source archive of the repository including submodules |
| `fwimage.py` | Verify, pack and split `*_upg.bin` upgrade images (pure Python, no dependencies) |
| `prune_rootfs.py` | Reduce the unpacked stock rootfs to the allow list of GPL/LGPL components |
| `unsquash.py` | Pure-Python squashfs 4.0 (LZMA/zlib) extractor for inspecting images |
| `scan_firmware.py` | Read-only scan of a firmware image for compressed/filesystem signatures |
| `mib.py` | Decode the LZSS-compressed flash settings blocks (MACs, calibration) from a dump |
| `syslog_recv.py` | Minimal UDP syslog collector for the camera's remote-log feature |
| `camsh.py` | Run shell commands on the camera over telnet (`CAM_HOST`, `CAM_PW` from the environment) |
| `serve.py`, `recv.py` | Tiny HTTP file servers for moving files to and from the camera with its `wget` |
| `camwatch.sh` | Unattended health monitor (uptime, memory, reboot/hang detection) over telnet |
| `dhcp-server.sh` | Bench DHCP server (dnsmasq) on the bootloader's recovery subnet |
| `Dockerfile.analysis` | Native image with squashfs/xz/binutils for firmware inspection without the vendor toolchain |
| `build_gpl.sh` | *Legacy.* Non-interactive full build of the vendor GPL tree (needs the tarball extracted into `build/IC3115W_GPL`) |
| `build_fw.sh`, `test_fw.sh` | *Legacy.* Early test image from the GPL build and its qemu-user smoke test |
| `build_debloat.sh` | *Legacy.* Stock v2.03 rootfs with the cloud phone-home disabled |

Treat everything downloaded from the vendor and every flash dump as untrusted input: extract into
fresh directories outside the repository and run the Python tools with `python3 -I`.
