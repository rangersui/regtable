"""Serve the Web Serial panel on localhost and open it.

    regtable serve              # http://127.0.0.1:8321, opens the browser
    regtable serve --port 9000

Web Serial needs a secure context, and http://localhost qualifies;
opening the file directly (file://) does not reach the serial API in
every browser, so this stays the one-command way in. Only the panel
itself is served; nothing else on disk is reachable. Stop with Ctrl-C.
"""

import http.server
import sys
import webbrowser
from pathlib import Path

HTML = "regtable_panel.html"


def panel_path():
    """The installed copy next to this file, or the repo root's when
    running from a checkout."""
    here = Path(__file__).resolve().parent
    for p in (here / HTML, here.parent.parent / HTML):
        if p.is_file():
            return p
    sys.exit(f"regtable serve: {HTML} not found next to {here}")


class PanelHandler(http.server.BaseHTTPRequestHandler):
    body = b""

    def do_GET(self):
        if self.path.split("?", 1)[0] not in ("/", "/" + HTML):
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(self.body)))
        # the panel is edited and reloaded a lot; a cached copy has
        # already cost one confusing debugging session
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(self.body)

    def log_message(self, *args):
        pass


def serve(port=8321, open_browser=True):
    PanelHandler.body = panel_path().read_bytes()
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", port), PanelHandler)
    url = f"http://127.0.0.1:{port}/{HTML}"
    print(f"regtable panel: {url}  (Ctrl-C stops)", flush=True)
    if open_browser:
        webbrowser.open(url)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(serve(int(sys.argv[1]) if len(sys.argv) > 1 else 8321))
