#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
#
# Smoke test of the test image's userland under qemu-mips user emulation (generic MIPS-I; the
# Lexra RLX4181 also lacks unaligned load/store instructions, so this is a proxy, not proof).
# Run inside the camd-build container after tools/build_fw.sh:
#   docker run --rm -v "$PWD":/work camd-build tools/test_fw.sh
set -e
R=$(pwd)/build/fw/rootfs
T=$(pwd)/build/fw/test
Q="qemu-mips-static -L $R"
mkdir -p "$T" /tmp/bin
fail=0
check() { if "$@"; then echo "ok   $*"; else echo "FAIL $*"; fail=1; fi; }

# Applet wrappers so scripts find busybox applets on PATH.
for a in $(sed 's|.*/||' build/fw/busybox/busybox.links); do
	printf '#!/bin/bash\nexec qemu-mips-static -L %s %s/bin/busybox %s "$@" 2> >(grep -v ld.so.cache >&2)\n' \
		"$R" "$R" "$a" > /tmp/bin/$a
	chmod +x /tmp/bin/$a
done
export PATH=/tmp/bin:$PATH
run() { $Q "$R/bin/busybox" sh "$@" 2> >(grep -v ld.so.cache >&2); }

for s in etc/init.d/rcS etc/udhcpc.script www/cgi-bin/info www/cgi-bin/snapshot; do
	check run -n "$R/$s"
done
for a in udhcpc httpd telnetd hexdump dd tftp wget; do
	check grep -q "/$a\$" build/fw/busybox/busybox.links
done
check bash -c "$Q $R/bin/camprobe -x 2>&1 | grep -q usage:"

# MAC parsing from the HW settings block, against a fake valid and a blank flash.
python3 -I - "$T" <<'PY'
import sys
good = bytearray(b'\xff' * 0x7000)
good[0x6000:0x600d] = b'H601' + b'\x01\x00' + b'\x00' + bytes([0x74, 0xda, 0x38, 0x12, 0x34, 0x56])
open(sys.argv[1] + '/good.bin', 'wb').write(good)
open(sys.argv[1] + '/blank.bin', 'wb').write(b'\xff' * 0x7000)
PY
sed -n '/^MAC=/,/^fi/p' "$R/etc/init.d/rcS" > "$T/mac.sh"
echo 'echo "MAC=$MAC"' >> "$T/mac.sh"
mac() { sed "s|/dev/mtdblock0|$T/$1.bin|" "$T/mac.sh" > "$T/run.sh"; run "$T/run.sh" 2>/dev/null | tail -n 1; }
check test "$(mac good)" = "MAC=74:da:38:12:34:56"
check test "$(mac blank)" = "MAC=00:11:09:01:02:04"

check python3 -I tools/fwimage.py verify build/fw/IC3115W_test_upg.bin > /dev/null
[ $fail = 0 ] && echo "all checks passed" || { echo "SOME CHECKS FAILED"; exit 1; }
