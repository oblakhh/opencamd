#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
# Wi-Fi client control for wlan0 (RTL8188Cn). The WPA2 handshake is done by
# camd's own supplicant (wpa.c / wpa_drv.c) through the driver's userland
# supplicant interface (802_1x=1, psk_enable=0); the in-kernel supplicant is
# off, and the driver never sees the passphrase. Called by camd (wifi.c) via
# execve. camd's sequence is
#   prep <ssid> [mac] -> starttest -> camd: SET_RSNIE (join) -> port open -> dhcp
# (uplink) or probe (test). DHCP must not start before the port is open.
# With 802_1x=1 the driver does not join at 'ifconfig up' (no RSN IE yet), so
# the CCK SIFS fix is always in place before the first Auth frame.
#   wifiapply.sh prep <ssid> [mac]   wlan0 down, MAC, board config + station MIBs
#   wifiapply.sh starttest           wlan0 up + CCK SIFS fix (uplink and test)
#   wifiapply.sh dhcp [hostname]     udhcpc -b (routes per /var/run/uplink)
#   wifiapply.sh probe               one-shot DHCP lease check; prints DHCP_IP=<ip>,
#                                    configures nothing (no address, route or DNS)
#   wifiapply.sh down
#   wifiapply.sh scan                -> prints /proc/wlan0/SS_Result
W=wlan0

stop_dhcp() {
	ps | grep "[u]dhcpc .*-i $W " | while read pid rest; do kill -9 "$pid"; done
}

# Board config the stripped vendor userland used to apply, plus the station
# recipe. MIBs only take effect at the next 'ifconfig up'.
configure() {
	iwpriv $W set_mib MIMO_TR_mode=4                       # chip is 1T1R
	for m in $(/bin/getmac -rf); do iwpriv $W set_mib "$m"; done   # RF calibration
	# opmode 8 = station (16 is AP!); 802_1x=1 + psk_enable=0: EAPOL goes to
	# the driver event queue for camd's supplicant (8192cd_rx.c)
	for m in opmode=8 802_1x=1 wsc_enable=0 authtype=2 encmode=2 psk_enable=0 \
	         wpa2_cipher=8 band=3 oprates=4095 basicrates=15; do
		iwpriv $W set_mib $m
	done
}

# CCK response SIFS: the 10 us default makes our ACKs late and AVM APs reject
# them. Reset by every 'ifconfig up', so it is written right after each one.
fix_sifs() { iwpriv $W write_reg w,63c,0606; }

prep() {
	stop_dhcp
	ifconfig $W down
	[ -n "$2" ] && ifconfig $W hw ether "$2"
	configure >/dev/null
	iwpriv $W set_mib "ssid=$1" >/dev/null
}

start_dhcp() {
	udhcpc -b -i $W -H "${1:-ic3115w}" -s /etc/udhcpc-wlan.script -p /var/run/udhcpc-wlan.pid
}

case "$1" in
prep)
	prep "$2" "$3"
	;;
starttest)
	ifconfig $W up && fix_sifs
	;;
dhcp)
	# Only after camd's supplicant has opened the port (see wifi.c bring_up).
	start_dhcp "$2"
	;;
probe)
	# No -H: the test MAC must not claim the camera's hostname in the router's DNS.
	exec udhcpc -i $W -n -q -s /etc/udhcpc-probe.script
	;;
down)
	stop_dhcp
	ifconfig $W down
	;;
scan)
	was_up=$(ifconfig $W | grep -c ' UP ')
	if [ "$was_up" = 0 ]; then
		# Never let a scan join anything: non-existent SSID, and a locally
		# administered MAC so it can't collide with the cloned eth0 MAC.
		ifconfig $W hw ether 02:00:00:00:00:01
		configure; iwpriv $W set_mib ssid=camd-scan-noassoc; ifconfig $W up && fix_sifs
	fi
	iwpriv $W at_ss >/dev/null
	sleep 4
	cat /proc/$W/SS_Result
	[ "$was_up" = 0 ] && ifconfig $W down
	;;
*)
	exit 2
	;;
esac
exit 0
