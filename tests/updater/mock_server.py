"""Mock GitHub release server for tests/updater (started by run_e2e.ps1).

Serves a folder on 127.0.0.1 like `python -m http.server`, plus byte ranges (206 + Content-Range) the way GitHub's
asset CDN answers them. Below /norange/files/ the same files are served while Range is ignored (a plain 200), which
is what `python -m http.server` itself does.

    python mock_server.py <folder> <port>
"""
import os
import sys
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


class Handler(SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("mock: " + (fmt % args) + "\n")

    def do_GET(self):
        if self.path.startswith("/norange/files/"):
            self.path = self.path[len("/norange"):]
            return super().do_GET()
        path = self.translate_path(self.path)
        spec = self.headers.get("Range")
        if not spec or not os.path.isfile(path):
            return super().do_GET()
        size = os.path.getsize(path)
        try:
            unit, span = spec.split("=", 1)
            first, last = span.split("-", 1)
            first = int(first)
            last = int(last) if last else size - 1
        except ValueError:
            return super().do_GET()
        if unit.strip() != "bytes" or first >= size or last < first:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        last = min(last, size - 1)
        with open(path, "rb") as f:
            f.seek(first)
            data = f.read(last - first + 1)
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {first}-{last}/{size}")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    folder, port = sys.argv[1], int(sys.argv[2])
    server = ThreadingHTTPServer(("127.0.0.1", port), partial(Handler, directory=folder))
    server.serve_forever()


if __name__ == "__main__":
    main()
