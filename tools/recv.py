#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
"""Minimal HTTP receiver for pulling files off the camera with its wget --post-file.

  python3 -I tools/recv.py [PORT] [OUTDIR]        # default: 8000, current dir

On the device (busybox/GNU wget 1.10):
  wget -q -O /dev/null --post-file=/tmp/mtd0.bin http://<PC-IP>:8000/mtd0.bin

The path's basename is the saved filename (sanitised). Prints size and md5 of each
file received so you can compare against the device's own `md5sum`. Read-only tool:
it only writes the files it is sent, into OUTDIR.
"""
import hashlib
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

OUTDIR = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.getcwd()


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        name = os.path.basename(self.path) or "upload.bin"
        name = "".join(c for c in name if c.isalnum() or c in "._-") or "upload.bin"
        n = int(self.headers.get("Content-Length", 0))
        dest = os.path.join(OUTDIR, name)
        h = hashlib.md5()
        got = 0
        with open(dest, "wb") as f:
            while got < n:
                chunk = self.rfile.read(min(65536, n - got))
                if not chunk:
                    break
                f.write(chunk)
                h.update(chunk)
                got += len(chunk)
        print("received %s: %d bytes, md5=%s%s" % (dest, got, h.hexdigest(),
              "" if got == n else "  *** SHORT, expected %d ***" % n))
        self.send_response(200)
        self.end_headers()

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    print("receiving into %s, listening on 0.0.0.0:%d (Ctrl-C to stop)" % (OUTDIR, port))
    HTTPServer(("0.0.0.0", port), Handler).serve_forever()
