#!/usr/bin/env python3
"""
AxisOS Google API bridge.

AxisOS can only speak plain http, but Google's APIs require https. Run this
small helper on your PC: AxisOS talks to it over http, and it forwards the
request to Google over https and returns the reply. This lets AxisOS use the
YouTube Data API (and other Google APIs) with your own API key.

Get a free API key at https://console.cloud.google.com/apis/credentials
(enable "YouTube Data API v3"), then in AxisOS run:
    google bridge <your-PC-ip>:8099
    google key youtube <YOUR_API_KEY>
    classictube            (now shows real YouTube results)

Usage:
    python3 google-bridge.py              # listens on 0.0.0.0:8099
    python3 google-bridge.py 9000         # custom port

Only forwards to Google-owned hosts (googleapis.com, ytimg.com, ...). For
thumbnails it downscales the image to 16-colour text - that needs Pillow
(pip install pillow); without it, titles still work, just no thumbnails.

This runs on YOUR computer with YOUR key; it is a convenience bridge, not a
public service - keep it on your local network.
"""
import sys
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ALLOW_SUFFIXES = (
    ".googleapis.com", ".ytimg.com", ".ggpht.com", ".youtube.com", ".gstatic.com",
)

# VGA 16-colour palette (index -> RGB), matches AxisOS
VGA = [
    (0, 0, 0), (0, 0, 170), (0, 170, 0), (0, 170, 170),
    (170, 0, 0), (170, 0, 170), (170, 85, 0), (170, 170, 170),
    (85, 85, 85), (85, 85, 255), (85, 255, 85), (85, 255, 255),
    (255, 85, 85), (255, 85, 255), (255, 255, 85), (255, 255, 255),
]


def nearest(r, g, b):
    best, bi = 1 << 30, 0
    for i, (cr, cg, cb) in enumerate(VGA):
        d = (r - cr) ** 2 + (g - cg) ** 2 + (b - cb) ** 2
        if d < best:
            best, bi = d, i
    return bi


def allowed(url):
    try:
        host = urllib.parse.urlparse(url).hostname or ""
    except ValueError:
        return False
    host = host.lower()
    return any(host == s[1:] or host.endswith(s) for s in ALLOW_SUFFIXES)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/octet-stream"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        p = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(p.query)
        if p.path == "/g":
            url = (q.get("url") or [""])[0]
            if not url.startswith("https://") or not allowed(url):
                self._send(403, b"blocked: only Google https hosts are allowed")
                return
            try:
                req = urllib.request.Request(url, headers={"User-Agent": "AxisOS-GoogleBridge/1.0"})
                with urllib.request.urlopen(req, timeout=15) as r:
                    data = r.read(4 * 1024 * 1024)
                    self._send(200, data, r.headers.get("Content-Type", "application/json"))
                print("OK  %s" % url[:90])
            except urllib.error.HTTPError as e:
                body = e.read()
                self._send(e.code, body, "application/json")
                print("ERR %d %s" % (e.code, url[:80]))
            except Exception as e:
                self._send(502, ("bridge error: %s" % e).encode())
                print("ERR %s" % e)
            return
        if p.path == "/thumb":
            vid = (q.get("id") or [""])[0]
            w = int((q.get("w") or ["72"])[0])
            h = int((q.get("h") or ["40"])[0])
            w = max(8, min(w, 120))
            h = max(8, min(h, 60))
            try:
                from PIL import Image
                import io
                src = None
                for name in ("mqdefault.jpg", "hqdefault.jpg", "default.jpg"):
                    try:
                        u = "https://i.ytimg.com/vi/%s/%s" % (urllib.parse.quote(vid), name)
                        with urllib.request.urlopen(u, timeout=10) as r:
                            src = r.read()
                        break
                    except Exception:
                        continue
                if not src:
                    self._send(404, b"no thumbnail")
                    return
                im = Image.open(io.BytesIO(src)).convert("RGB").resize((w, h))
                px = im.load()
                out = bytearray(w * h)
                for y in range(h):
                    for x in range(w):
                        r, g, b = px[x, y]
                        out[y * w + x] = nearest(r, g, b)
                self._send(200, bytes(out))
            except ImportError:
                self._send(503, b"install Pillow for thumbnails: pip install pillow")
            except Exception as e:
                self._send(502, ("thumb error: %s" % e).encode())
            return
        if p.path == "/" or p.path == "/status":
            self._send(200, b"AxisOS Google bridge is running. Point AxisOS at this host:port.", "text/plain")
            return
        self._send(404, b"not found")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
    httpd = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print("AxisOS Google bridge listening on http://0.0.0.0:%d" % port)
    print("In AxisOS:  google bridge <this-pc-ip>:%d" % port)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
