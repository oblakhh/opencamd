#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
# Run shell commands on the camera over telnet and print the output
# (debug helper; CAM_HOST/CAM_PORT/CAM_USER/CAM_PW from the environment).

import socket, sys, time
import os
HOST = os.environ.get('CAM_HOST', '192.168.2.3')
PORT = int(os.environ.get('CAM_PORT', '23'))
USER = os.environ.get('CAM_USER', 'admin')
PW = os.environ.get('CAM_PW', 'admin')
def neg(data, sock):
    out=bytearray(); i=0
    while i < len(data):
        b=data[i]
        if b==255 and i+2<len(data):
            cmd,opt=data[i+1],data[i+2]
            if cmd in (251,252): sock.sendall(bytes([255,254,opt]))
            elif cmd in (253,254): sock.sendall(bytes([255,252,opt]))
            i+=3; continue
        out.append(b); i+=1
    return bytes(out)
def drain(sock, sentinel=None, total=20, idle=1.5):
    sock.settimeout(idle); buf=bytearray(); t0=time.time()
    while time.time()-t0<total:
        try: ch=sock.recv(4096)
        except socket.timeout:
            if sentinel is None: break
            else: continue
        if not ch: break
        buf+=neg(ch,sock)
        if sentinel and sentinel in buf: break
    return bytes(buf)
s=socket.create_connection((HOST,PORT),timeout=10)
print(repr(drain(s, b'login:', 8)[-60:]))
s.sendall(USER.encode()+b'\n'); print(repr(drain(s, b'assword', 6)[-40:]))
s.sendall(PW.encode()+b'\n'); print(repr(drain(s, b'# ', 8)[-80:]))
for c in sys.argv[1:]:
    s.sendall(c.encode()+b'; echo RC=$? "MARK""_DONE"\n')
    out=drain(s, b'MARK_DONE\r\n', 90, 3.0)
    print('----',c,'----'); sys.stdout.write(out.decode('latin1')); sys.stdout.flush()
s.sendall(b'exit\n'); time.sleep(0.2); s.close()
