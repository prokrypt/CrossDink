#!/usr/bin/env python3
"""Receives CrossDink bug reports (src/util/BugReport.cpp).

The device POSTs each report to the URL in /bugreports/url.txt as
`POST <url>?id=<report id>` with the report zip (report.txt, log.txt,
screen.png) as the body. This writes it to <out>/<id>.zip.

    python3 scripts/bugreport_receiver.py --port 8765 --out ./bugreports
    # on the SD card: /bugreports/url.txt = http://<this machine>:8765/report
"""

import argparse
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

SAFE_ID = re.compile(r"^[A-Za-z0-9-]{1,40}$")
MAX_BYTES = 8 * 1024 * 1024


def make_handler(out: Path):
    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            query = parse_qs(urlparse(self.path).query)
            report_id = query.get("id", [""])[0]
            length = int(self.headers.get("Content-Length", "0"))
            if not SAFE_ID.match(report_id) or not 0 < length <= MAX_BYTES:
                self.send_error(400, "bad id or size")
                return
            body = self.rfile.read(length)
            (out / f"{report_id}.zip").write_bytes(body)
            print(f"{self.client_address[0]}: {report_id}.zip {len(body)} B", flush=True)
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
