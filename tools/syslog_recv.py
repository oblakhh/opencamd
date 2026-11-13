#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
"""Tiny UDP syslog collector for the camera's remote-log feature.

  python3 -I tools/syslog_recv.py [PORT] [LOGFILE]     # default 5514, build/fw/camera_syslog.txt

Prints each received RFC3164 syslog line with a local receive-timestamp and appends to LOGFILE.
Point the camera's remote log at <this-host>:<PORT> (busybox syslogd -R HOST:PORT). Read-only
apart from appending to LOGFILE.
"""
import datetime
import os
import socket
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5514
LOGFILE = sys.argv[2] if len(sys.argv) > 2 else os.path.join("build", "fw", "camera_syslog.txt")
os.makedirs(os.path.dirname(LOGFILE) or ".", exist_ok=True)

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", PORT))
print("syslog collector on udp/%d -> %s (Ctrl-C to stop)" % (PORT, LOGFILE))
with open(LOGFILE, "a") as f:
    while True:
        data, addr = s.recvfrom(4096)
        ts = datetime.datetime.now().strftime("%H:%M:%S")
        line = "%s %s %s" % (ts, addr[0], data.decode("latin1").rstrip())
        print(line, flush=True)
        f.write(line + "\n"); f.flush()
