#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
# Apply the wired network config to eth0. Called by camd (netcfg.c) via execv with
# validated arguments only:
#   netapply.sh dhcp   <hostname>
#   netapply.sh static <ip> <netmask> <gateway|-> <dns1|-> <dns2|->
#   netapply.sh down   (uplink moved to Wi-Fi: keep only the 192.168.2.3 fallback)
IF=eth0
PIDF=/var/run/udhcpc.pid

# Stop every DHCP client on $IF. Matched by command line, not the PID file:
# udhcpc 0.9.8 with -b writes the PID file before it backgrounds, so the file
# holds the dead parent's PID. Interface-specific so a wlan0 client survives.
# SIGKILL: this udhcpc build ignores SIGTERM (no lease release needed anyway).
stop_dhcp() {
	ps | grep "[u]dhcpc .*-i $IF " | while read pid rest; do kill -9 "$pid"; done
	rm -f "$PIDF"
}

case "$1" in
dhcp)
	stop_dhcp
	ifconfig "$IF:0" down 2>/dev/null
	udhcpc -b -i "$IF" -H "${2:-ic3115w}" -s /etc/udhcpc.script -p "$PIDF"
	;;
static)
	stop_dhcp
	ifconfig "$IF" "$2" netmask "$3" up || exit 1
	while route del default 2>/dev/null; do :; done
	[ "$4" != "-" ] && route add default gw "$4" dev "$IF"
	: > /var/resolv.conf
	for d in "$5" "$6"; do
		[ "$d" != "-" ] && echo "nameserver $d" >> /var/resolv.conf
	done
	# Rescue address for a direct cable, unless it is the static address itself.
	[ "$2" != "192.168.2.3" ] && ifconfig "$IF:0" 192.168.2.3 netmask 255.255.255.0 up 2>/dev/null
	echo "1 1" > /proc/gpio 2>/dev/null
	;;
down)
	stop_dhcp
	ifconfig "$IF:0" down 2>/dev/null
	ifconfig "$IF" 192.168.2.3 netmask 255.255.255.0 up
	;;
*)
	exit 2
	;;
esac
exit 0
