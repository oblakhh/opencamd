#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
#
# Unattended read-only health monitor for the IC-3115W. Logs uptime + memory every INTERVAL,
# flags reboots (uptime reset) and hangs (probe failure). Pair with the remote-syslog collector:
#   probe fails + fresh boot lines in syslog  -> camera rebooted (telnetd is off after reboot)
#   probe fails + syslog silent               -> camera hung
#   probe ok                                  -> healthy
# Usage: tools/camwatch.sh [ITERATIONS] [INTERVAL_SEC]   (default 96 x 300s = 8h)
set -u
cd "$(dirname "$0")/.."
CAMSH=tools/camsh.py
OUT=build/fw/camera_monitor.txt
ITER=${1:-96}
INT=${2:-300}
last_up=""
echo "$(date '+%F %T')  monitor start (on Wi-Fi), ITER=$ITER INT=${INT}s" >> "$OUT"
i=0
while [ "$i" -lt "$ITER" ]; do
	ts=$(date '+%F %T')
	out=$(timeout 45 python3 -I "$CAMSH" 'cat /proc/uptime; free | grep Mem' 2>/dev/null \
	      | tr -d '\r' | grep -vE 'MARK|login:|Password:|built-in|help|^#')
	up=$(echo "$out" | grep -oE '^[0-9]+\.[0-9]+' | head -1 | cut -d. -f1)
	mem=$(echo "$out" | grep -E '^ *Mem:' | tail -1 | sed 's/  */ /g; s/^ *//')
	if [ -z "$up" ]; then
		echo "$ts  PROBE_FAILED (no shell — hung, rebooting, or telnetd off)" >> "$OUT"
	else
		note=""
		if [ -n "$last_up" ] && [ "$up" -lt "$last_up" ]; then
			note="  *** UPTIME RESET -> camera rebooted ***"
		fi
		echo "$ts  up=${up}s  ${mem}${note}" >> "$OUT"
		last_up=$up
	fi
	i=$((i+1))
	sleep "$INT"
done
echo "$(date '+%F %T')  monitor finished" >> "$OUT"
