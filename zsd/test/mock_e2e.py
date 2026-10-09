#!/usr/bin/env python3
"""Loopback-only ZSD CLI integration test; never contacts is.gd."""
import base64
import http.server
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import urllib.parse


class MockIsGd(http.server.BaseHTTPRequestHandler):
    values = {}
    uploads = []
    next_id = 0

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        if parsed.path.endswith("/create.php"):
            carrier = query.get("url", [""])[0]
            assert carrier.startswith("http://ok.com/?"), carrier
            encoded = carrier[len("http://ok.com/?"):]
            raw = base64.b64decode(encoded.replace(".", "+").replace("_", "/"))
            assert len(raw) <= 1900, len(raw)
            MockIsGd.next_id += 1
            short_id = "mock%05d" % MockIsGd.next_id
            MockIsGd.values[short_id] = carrier
            MockIsGd.uploads.append(raw)
            body = "http://%s/%s" % (self.headers["Host"], short_id)
            self.reply(body)
        elif parsed.path.endswith("/forward.php"):
            short_id = query.get("shorturl", [""])[0]
            self.reply(MockIsGd.values.get(short_id, "Error: unknown id"))
        else:
            self.send_error(404)

    def reply(self, body):
        data = body.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def run(command, cwd):
    return subprocess.check_output(command, cwd=cwd, stderr=subprocess.STDOUT,
                                   universal_newlines=True)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: mock_e2e.py 'mono /path/to/Zsd.exe'")
    executable = shlex.split(sys.argv[1])
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MockIsGd)
    thread = threading.Thread(target=server.serve_forever)
    thread.daemon = True
    thread.start()
    root = tempfile.mkdtemp(prefix="zsd-mock-e2e-")
    try:
        source = os.path.join(root, "input")
        os.makedirs(os.path.join(source, "empty"))
        with open(os.path.join(source, "note.txt"), "wb") as handle:
            handle.write(b"single leaf local protocol test\n")
        endpoint = "http://127.0.0.1:%d/" % server.server_port
        uploaded = run(executable + ["upload", "--provider", "legacy", "--endpoint", endpoint, "--pace-ms", "0", source], root)
        match = re.search(r"ZSD entry: ([A-Za-z0-9_]{5,30})", uploaded)
        assert match, uploaded
        # Tiny input produces one data leaf plus one root tree node. This
        # explicitly verifies the <=1900-byte single-leaf path in both ways.
        assert len(MockIsGd.uploads) == 2, len(MockIsGd.uploads)
        assert all(len(value) <= 1900 for value in MockIsGd.uploads)
        root_node = MockIsGd.uploads[-1]
        assert root_node[:4] == b"ZSDT" and root_node[6] == 0, root_node[:7]
        destination = os.path.join(root, "out")
        run(executable + ["get", "--provider", "legacy", "--endpoint", endpoint, "--pace-ms", "0", "--output", destination, match.group(1)], root)
        with open(os.path.join(destination, "input", "note.txt"), "rb") as handle:
            assert handle.read() == b"single leaf local protocol test\n"
        assert os.path.isdir(os.path.join(destination, "input", "empty"))
        print("ZSD mock e2e OK")
    finally:
        server.shutdown()
        server.server_close()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()