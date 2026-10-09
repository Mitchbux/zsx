#!/usr/bin/env python3
"""Read-only live fixture verification; never uploads, deletes, or prints secrets."""
import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading


ROOT_ID = "zsd_dbfd9a516bc75bab1db22fee1b"
EXPECTED_BYTES = 49001
EXPECTED_SHA256 = "fdd6970d0cca7e58c164d4684b5aa8e9f3c5220870595db8d72ed5d920a1b35e"
root = Path(__file__).resolve().parents[2]
stage = "initialization"
env = dict(os.environ)
# Explicitly remove BOTH, rather than setting them to empty strings.
env.pop("SHORT_IO_API_KEY", None)
env.pop("SHORT_IO_DOMAIN", None)
os.umask(0o077)


def check(condition):
    if not condition:
        raise RuntimeError()


def run(args, cwd, expected=0):
    result = subprocess.run(args, cwd=cwd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=300)
    check(result.returncode == expected)
    return result


try:
    mono = shutil.which("mono")
    check(mono is not None)
    with tempfile.TemporaryDirectory(prefix="zsd-private-verify-", dir="/tmp") as tmp:
        work = Path(tmp)
        public = work / "PublicZsd.exe"
        stage = "public .NET 4 compilation"
        run(["mcs", "-sdk:4", "-langversion:4", "-target:exe",
             "-out:" + str(public), str(root / "outputs/Zsd.cs")], work)
        stage = "public missing-key loopback verification"

        class Loopback(BaseHTTPRequestHandler):
            requests = 0

            def do_GET(self):
                Loopback.requests += 1
                self.send_error(500)

            do_POST = do_GET

            def log_message(self, *args):
                pass

        server = HTTPServer(("127.0.0.1", 0), Loopback)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            failure = subprocess.run(
                [mono, str(public), "get", "--endpoint",
                 "http://127.0.0.1:" + str(server.server_port) + "/", ROOT_ID],
                cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=30)
            check(failure.returncode != 0)
            check(b"SHORT_IO_API_KEY" in failure.stdout + failure.stderr)
            check(Loopback.requests == 0)
        finally:
            server.shutdown()
            thread.join()
            server.server_close()
        print("Public CLI missing-key failure; loopback requests = 0: true")

        stage = "private bare-ID read-only live download"
        restored = work / "restored"
        restored.mkdir(mode=0o700)
        # No options: compiled pex.s.gy, official Short.io lookup, embedded auth,
        # and default current-directory extraction. No upload/delete commands.
        run([mono, str(root / "outputs/private-zsd/Zsd.exe"), "get", ROOT_ID],
            restored)
        check("SHORT_IO_API_KEY" not in env and "SHORT_IO_DOMAIN" not in env)
        files = [p for p in restored.rglob("*") if p.is_file()]
        check(len(files) == 15)
        check(not any(p.name.startswith(".zsd-stage-") for p in restored.rglob("*")))
        print("Private get existing bare ID with both variables removed: true")
        print("Compiled default pex.s.gy; current-directory extraction; files = 15: true")

        stage = "public repack of the downloaded tree"
        packed = work / "reconstructed.zsd"
        inputs = [str(p) for p in sorted(restored.iterdir())]
        run([mono, str(public), "pack", "--output", str(packed)] + inputs, restored)
        data = packed.read_bytes()
        check(len(data) == EXPECTED_BYTES)
        check(hashlib.sha256(data).hexdigest() == EXPECTED_SHA256)
        print("Reconstructed .zsd bytes = 49001; SHA-256 = " + EXPECTED_SHA256)
        print("Known source archive byte-for-byte match: true")
        print("No live uploads/deletions; isolated temporary tree removed on exit.")
except BaseException:
    # Provider/compiler output and exception representations are not disclosed.
    print("Verification failed during " + stage + "; raw diagnostics suppressed.",
          file=sys.stderr)
    sys.exit(1)