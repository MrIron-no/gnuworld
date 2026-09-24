#!/usr/bin/env python3
"""A stand-in for Pushover's API, for test_pushover_delivery.

Every POST is appended to a capture file, one request per line, and answered the
way the real service answers a message it accepted:

    {"status":1,"request":"x"}

Two paths are not that:

    /fail   answers 500, so that a delivery failure can be provoked
    /slow   answers the success above, half a second later, so that a sink can
            be destroyed with requests still queued behind one in flight

The body is recorded when it ARRIVES, before /slow sleeps: what the capture file
holds is therefore everything that was really sent, which is what lets the test
assert that nothing was sent after a sink was destroyed.

A LINE OF THE CAPTURE FILE holds the request twice over, as tab separated
fields, every one of them escaped by _escape() below:

    <raw body> TAB <name> TAB <value> TAB <name> TAB <value> ...

the raw body exactly as it arrived, and then the form fields decoded out of it,
in the order they were sent and with every repetition of a name kept.  The test
needs both: the DECODED message is what has to equal the log sentence byte for
byte, and the RAW body is where it asserts that a sentence carrying "&user=..."
did not put a second "user" field on the wire.  parse_qsl and not parse_qs, for
the order and the repetitions; keep_blank_values, because an empty field that
was sent is a field that was sent.

Standard library only, and no configuration but the two arguments:

    gnuworld-pushover-endpoint --port <port> --capture <file>
"""

from __future__ import annotations

import argparse
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SUCCESS = b'{"status":1,"request":"x"}'

# One writer at a time: the server is threaded, and a captured request is a line
_capture_lock = threading.Lock()


def _escape(text: str) -> str:
    """text with nothing in it that the line format itself uses.

    The backslash first, or it would escape the escapes that follow it.  The
    reader of this file (test/pushover_delivery.cc, unescape()) undoes exactly
    these four.
    """
    return (
        text.replace("\\", "\\\\")
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )


def _capture_line(body: str) -> str:
    """One line holding the raw body and the fields decoded out of it."""
    parts = [_escape(body)]

    for name, value in urllib.parse.parse_qsl(body, keep_blank_values=True):
        parts.append(_escape(name))
        parts.append(_escape(value))

    return "\t".join(parts) + "\n"


class Handler(BaseHTTPRequestHandler):
    # The capture file, set by main()
    capture_path = ""

    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # noqa: D102 - quiet, this is a test double
        pass

    def do_POST(self):  # noqa: N802 - BaseHTTPRequestHandler's spelling
        length = int(self.headers.get("Content-Length", "0") or "0")
        body = self.rfile.read(length).decode("utf-8", errors="replace")

        # Recorded on arrival, whatever the answer is going to be
        with _capture_lock:
            with open(self.capture_path, "a", encoding="utf-8") as fh:
                fh.write(_capture_line(body))
                fh.flush()

        if self.path.endswith("/fail"):
            self.send_response(500)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        if self.path.endswith("/slow"):
            time.sleep(0.5)

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(SUCCESS)))
        self.end_headers()
        self.wfile.write(SUCCESS)

    def do_GET(self):  # noqa: N802 - a liveness check for the run script
        self.send_response(200)
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"ok")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=18999)
    parser.add_argument("--capture", required=True)
    args = parser.parse_args()

    Handler.capture_path = args.capture

    # Created empty, so that a reader never finds a file that is not there yet
    with open(args.capture, "w", encoding="utf-8"):
        pass

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.daemon_threads = True

    print(f"endpoint: http://127.0.0.1:{args.port}/ -> {args.capture}", flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass

    return 0


if __name__ == "__main__":
    sys.exit(main())
