"""Local preview server for the web portal.

Live-renders the pages from web/ (templates + fragments) and serves them with
the shared assets and mock API responses, so you can iterate on the design in a
normal browser without flashing the device.

    python3 scripts/preview_web.py                      # http://localhost:8000, mock API
    python3 scripts/preview_web.py 9000                  # custom port
    python3 scripts/preview_web.py --device 10.0.1.67    # real API: proxy everything else to the device
    python3 scripts/preview_web.py --push 10.0.1.67      # upload rendered pages to /debug/web on the SD card

Pages are re-rendered per request and the browser reloads itself when a web/
file changes. --device needs any build with File transfer open; --push needs an
x4-pro-debug build (only it reads /debug/web) and File transfer open."""
import os
import re
import sys
import json
import time
import uuid
import argparse
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, urlencode

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "web")
JSZIP = os.path.join(ROOT, "src", "network", "html", "js", "jszip.min.js")

# slug -> (route, title, active, extra <head> markup)
PAGES = {
    "home":     ("/",         "CrossDink",                   "home",     ""),
    "files":    ("/files",    "Files - CrossDink",           "files",    '  <script src="/js/jszip.min.js"></script>'),
    "settings": ("/settings", "Settings - CrossDink Reader", "settings", ""),
    "fonts":    ("/fonts",    "Fonts - CrossDink",           "fonts",    ""),
    "logs":     ("/logs",     "Logs - CrossDink",            "logs",     ""),
}
ROUTE_TO_SLUG = {route: slug for slug, (route, *_rest) in PAGES.items()}
ROUTE_TO_SLUG["/battery"] = "logs"  # same page, Battery tab, as on the device

DEVICE = None  # --device host: unknown paths are proxied there instead of mocked

# Polls /__mtime and reloads when a web/ file changes. Preview only, never pushed.
RELOAD_JS = """<script>(function(){let m=null;setInterval(async()=>{try{const r=await fetch('/__mtime');
const t=await r.text();if(m!==null&&t!==m)location.reload();m=t;}catch(e){}},1000);})();</script>"""

def web_mtime():
    return max(os.path.getmtime(os.path.join(d, f)) for d, _, fs in os.walk(WEB) for f in fs)

def read(*parts):
    with open(os.path.join(*parts), encoding="utf-8") as f:
        return f.read()

def render_page(slug, live=True):
    _route, title, active, head_extra = PAGES[slug]
    if live:
        head_extra += RELOAD_JS
        if DEVICE:  # files.js opens its upload WebSocket here instead of on localhost
            head_extra += f'<script>window.WS_HOST="{DEVICE}";</script>'

    js = read(WEB, "pages", f"{slug}.js").strip()
    values = {
        "title": title, "v": "dev", "head_extra": head_extra,
        "styles": read(WEB, "pages", f"{slug}.css"),
        "body": read(WEB, "pages", f"{slug}.html"),
        "script": f"<script>\n{js}\n</script>" if js else "",
        "cls_home": "", "cls_files": "", "cls_settings": "", "cls_fonts": "", "cls_logs": "",
    }
    values[f"cls_{active}"] = ' class="active"'
    base = read(WEB, "templates", "base.html")
    return re.sub(r"{{\s*(\w+)\s*}}", lambda m: values[m.group(1)], base)

# Representative mock data so the pages render populated.
MOCK_API = {
    "/api/status": {"version": "dev-preview", "ip": "192.168.4.1", "freeHeap": 142000},
    "/api/files": [
        {"name": "Books", "isDirectory": True, "isEpub": False, "size": 0},
        {"name": "Read", "isDirectory": True, "isEpub": False, "size": 0},
        {"name": "The Great Gatsby.epub", "isDirectory": False, "isEpub": True, "size": 384512},
        {"name": "Moby Dick.epub", "isDirectory": False, "isEpub": True, "size": 612000},
        {"name": "notes.txt", "isDirectory": False, "isEpub": False, "size": 2048},
    ],
    "/api/fonts": {"families": [
        {"name": "Bookerly", "sizes": [10, 12, 14], "files": [{"size": 120000}, {"size": 140000}]},
        {"name": "Literata", "sizes": [12], "files": [{"size": 160000}]},
    ]},
    "/api/wifi": [
        {"ssid": "HomeNetwork", "hasPassword": True, "isLastConnected": True},
        {"ssid": "Library Guest", "hasPassword": False, "isLastConnected": False},
    ],
    "/api/opds": [
        {"name": "Project Gutenberg", "url": "https://m.gutenberg.org/ebooks.opds/",
         "username": "", "hasPassword": False, "filenameFormat": "author_title"},
    ],
    "/api/settings": [
        {"key": "darkMode", "name": "Dark Mode", "category": "Display", "type": "toggle", "value": 1},
        {"key": "fontSize", "name": "Font Size", "category": "Reader", "type": "enum",
         "value": 2, "options": ["10 pt", "12 pt", "14 pt", "16 pt"]},
        {"key": "margin", "name": "Page Margin", "category": "Reading", "type": "value",
         "value": 20, "min": 0, "max": 50, "step": 5},
        {"key": "sleepScreen", "name": "Sleep Screen", "category": "Reading", "type": "enum",
         "value": 0, "options": ["Cover", "Minimal", "Blank"]},
        {"key": "quickResumeSleepScreen", "name": "Quick Resume on Timeout",
         "category": "Reading", "type": "toggle", "value": 0},
        {"key": "deviceName", "name": "Device Name", "category": "Network", "type": "string",
         "value": "CrossDink-01"},
    ],
}

ASSETS = {
    "/style.css": (os.path.join(WEB, "assets", "style.css"), "text/css"),
    "/logo.png": (os.path.join(WEB, "assets", "logo.png"), "image/png"),
    "/js/jszip.min.js": (JSZIP, "application/javascript"),
}

def proxy(handler, method):
    """Forward the request to the device and stream the answer back (some
    endpoints, like /api/psram-log/stream, never end)."""
    length = int(handler.headers.get("Content-Length") or 0)
    body = handler.rfile.read(length) if length else None
    headers = {k: v for k, v in handler.headers.items() if k.lower() not in ("host", "connection", "accept-encoding")}
    conn = http.client.HTTPConnection(DEVICE, 80, timeout=60)
    try:
        conn.request(method, handler.path, body, headers)
        resp = conn.getresponse()
        handler.send_response(resp.status)
        for k, v in resp.getheaders():
            if k.lower() not in ("connection", "transfer-encoding", "content-length"):
                handler.send_header(k, v)
        handler.send_header("Connection", "close")
        handler.end_headers()
        while chunk := resp.read1(4096):
            handler.wfile.write(chunk)
            handler.wfile.flush()
    except OSError as e:
        handler.send_error(502, f"device {DEVICE}: {e}")
    finally:
        conn.close()

class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ROUTE_TO_SLUG:
            try:
                self._send(200, render_page(ROUTE_TO_SLUG[path]), "text/html; charset=utf-8")
            except Exception as e:  # surface template errors in the browser
                self._send(500, f"render error: {e}", "text/plain")
            return
        if path == "/__mtime":
            self._send(200, str(web_mtime()), "text/plain")
            return
        if path in ASSETS:
            fpath, ctype = ASSETS[path]
            with open(fpath, "rb") as f:
                self._send(200, f.read(), ctype)
            return
        if DEVICE:
            proxy(self, "GET")
            return
        if path in MOCK_API:
            self._send(200, json.dumps(MOCK_API[path]), "application/json")
            return
        self._send(404, "not found", "text/plain")

    def do_POST(self):
        if DEVICE:
            proxy(self, "POST")
            return
        # Accept saves/uploads/deletes during preview so the JS does not error.
        self._send(200, json.dumps({"ok": True}), "application/json")

    def log_message(self, fmt, *args):
        pass  # keep the console quiet

# /debug/web names the firmware's WEB_OVERRIDE handlers look for.
PUSH_NAMES = {f"{'index' if slug == 'home' else slug}.html": slug for slug in PAGES}

def post(host, path, body=b"", ctype="application/x-www-form-urlencoded"):
    conn = http.client.HTTPConnection(host, 80, timeout=60)
    try:
        conn.request("POST", path, body, {"Content-Type": ctype})
        resp = conn.getresponse()
        return resp.status, resp.read().decode("utf-8", "replace")
    finally:
        conn.close()

def push(host):
    files = {name: render_page(slug, live=False).encode("utf-8") for name, slug in PUSH_NAMES.items()}
    for path, (fpath, _ctype) in ASSETS.items():
        with open(fpath, "rb") as f:
            files[os.path.basename(path)] = f.read()
    # Errors here just mean the folders already exist.
    post(host, "/mkdir", urlencode({"name": "debug", "path": "/"}).encode())
    post(host, "/mkdir", urlencode({"name": "web", "path": "/debug"}).encode())
    # Upload refuses to overwrite; a missing file only fails its own entry.
    post(host, "/delete", urlencode({"paths": json.dumps([f"/debug/web/{n}" for n in files])}).encode())
    failed = 0
    for name, data in files.items():
        boundary = uuid.uuid4().hex
        body = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="{name}"\r\n'
                f"Content-Type: application/octet-stream\r\n\r\n").encode() + data + f"\r\n--{boundary}--\r\n".encode()
        status, text = post(host, "/upload?" + urlencode({"path": "/debug/web"}), body,
                            f"multipart/form-data; boundary={boundary}")
        print(f"{name:16} {len(data):>7}B  {status} {text.strip()[:60]}")
        failed += status != 200
    print("done; hard-reload the device page" if not failed else f"{failed} upload(s) failed")
    return 1 if failed else 0

if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="CrossDink web portal preview")
    ap.add_argument("port", nargs="?", type=int, default=8000)
    ap.add_argument("--device", help="proxy API calls to this device IP instead of the mock API")
    ap.add_argument("--push", metavar="HOST", help="upload rendered pages to /debug/web on this device and exit")
    args = ap.parse_args()
    if args.push:
        sys.exit(push(args.push))
    DEVICE = args.device
    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    src = f"API from {DEVICE}" if DEVICE else "mock API"
    print(f"CrossDink web preview: http://localhost:{args.port}  ({src}, Ctrl+C to stop)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
