#!/usr/bin/env python3
"""Serve regtable_panel.html on localhost and open it.

    python tools/panel.py          # http://127.0.0.1:8321, opens the browser
    python tools/panel.py 9000     # a different port

Web Serial needs a secure context, and http://localhost qualifies;
opening the file directly (file://) does not reach the serial API in
every browser, so this stays the one-command way in. Stop with Ctrl-C.
"""

import http.server
import functools
import sys
import webbrowser
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8321


class NoCacheHandler(http.server.SimpleHTTPRequestHandler):
    """The panel is edited and reloaded a lot; a cached copy has
    already cost one confusing debugging session."""

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, *args):
        pass


def main():
    handler = functools.partial(NoCacheHandler, directory=str(ROOT))
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), handler)
    url = f"http://127.0.0.1:{PORT}/regtable_panel.html"
    print(f"regtable panel: {url}  (Ctrl-C stops)")
    webbrowser.open(url)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
