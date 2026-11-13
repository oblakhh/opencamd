#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
"""Serve files on GET and save POST bodies — the camera's wget talks to this.

  python3 -I tools/serve.py DIR [PORT]      # default PORT 8000, binds 0.0.0.0

GET /<name>  -> sends DIR/<name>. POST /<name> -> saves body to DIR/<name>, prints md5.
Used to push camprobe/camrun.sh to the device and collect their output. Read-only
apart from the POST files it is sent, which land in DIR.
"""
import hashlib
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

DIR = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.getcwd()


def safe(path):
    return "".join(c for c in os.path.basename(path) if c.isalnum() or c in "._-")


class H(BaseHTTPRequestHandler):
    def do_GET(self):
        p = os.path.join(DIR, safe(self.path))
        if not os.path.isfile(p):
            self.send_response(404); self.end_headers(); return
        data = open(p, "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        print("served %s (%d bytes)" % (p, len(data)))

    def do_POST(self):
        name = safe(self.path) or "upload.bin"
        n = int(self.headers.get("Content-Length", 0))
        h = hashlib.md5(); got = 0
        dest = os.path.join(DIR, name)
        with open(dest, "wb") as f:
            while got < n:
                c = self.rfile.read(min(65536, n - got))
                if not c:
                    break
                f.write(c); h.update(c); got += len(c)
        print("received %s: %d bytes md5=%s%s" % (dest, got, h.hexdigest(),
              "" if got == n else " *SHORT*"))
        self.send_response(200); self.end_headers()

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8000
    print("serving %s on 0.0.0.0:%d" % (DIR, port))
    HTTPServer(("0.0.0.0", port), H).serve_forever()
