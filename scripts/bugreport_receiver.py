#!/usr/bin/env python3
"""Receives CrossDink bug reports (src/util/BugReport.cpp).

The device POSTs each file of a report to the URL in /bugreports/url.txt as
`POST <url>?id=<report id>&file=<report.txt|log.txt|screen.bmp>` with the raw
file as the body. This writes it to <out>/<id>/<file>.

    python3 scripts/bugreport_receiver.py --port 8765 --out ./bugreports
    # on the SD card: /bugreports/url.txt = http://<this machine>:8765/report
"""

import argparse
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

SAFE_ID = re.compile(r"^[A-Za-z0-9-]{1,40}$")
FILES = {"report.txt", "log.txt", "screen.bmp"}
MAX_BYTES = 8 * 1024 * 1024


def make_handler(out: Path):
    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            query = parse_qs(urlparse(self.path).query)
            report_id = query.get("id", [""])[0]
            name = query.get("file", [""])[0]
            length = int(self.headers.get("Content-Length", "0"))
            if not SAFE_ID.match(report_id) or name not in FILES or not 0 < length <= MAX_BYTES:
                self.send_error(400, "bad id, file or size")
                return
            body = self.rfile.read(length)
            target = out / report_id
            target.mkdir(parents=True, exist_ok=True)
            (target / name).write_bytes(body)
            print(f"{self.client_address[0]}: {report_id}/{name} {len(body)} B", flush=True)
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--out", type=Path, default=Path("bugreports"))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    print(f"listening on :{args.port}, writing to {args.out.resolve()}", flush=True)
    ThreadingHTTPServer(("", args.port), make_handler(args.out)).serve_forever()


if __name__ == "__main__":
    main()
