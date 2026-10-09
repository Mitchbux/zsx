#!/usr/bin/env python3
"""Offline-only .NET 4 Short.io integration. Never uses a real API credential."""
import base64
import email.utils
import hashlib
import http.server
import json
import os
import pathlib
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse


class MockShortIo(http.server.BaseHTTPRequestHandler):
    values = {}
    writes = []
    chunks = []
    metadata = []
    redirected = False
    previous = None
    expected_domain = "offline.example"
    lookups = []

    def log_message(self, *_):
        pass

    def reply(self, status, data, headers=None):
        wire = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(wire)))
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(wire)

    def authenticated(self):
        if self.headers.get("Authorization") != "offline-test-only":
            self.reply(401, {"error": "unauthorized"})
            return False
        return True

    def do_POST(self):
        if not self.authenticated():
            return
        assert self.path == "/links/bulk"
        size = int(self.headers["Content-Length"])
        assert size <= 2097152
        payload = json.loads(self.rfile.read(size))
        assert payload["domain"] == self.expected_domain
        assert payload["allowDuplicates"] is False
        links = payload["links"]
        assert 0 < len(links) <= 1000
        for link in links:
            assert link["originalURL"].startswith("https://pex.wf/#")
            assert len(link["originalURL"].encode()) <= 2048
            encoded = link["originalURL"][len("https://pex.wf/#"):]
            chunk = base64.b64decode(encoded, validate=True)
            assert base64.b64encode(chunk).decode("ascii") == encoded
            assert len(chunk) <= 1450
            if chunk.startswith(b"ZSDT"):
                self.metadata.append(chunk)
            else:
                self.chunks.append(chunk)
            prior = self.values.get(link["path"])
            assert prior is None or prior == link["originalURL"]
            self.values[link["path"]] = link["originalURL"]
        self.writes.append(time.monotonic())
        self.reply(200, links)

    def do_GET(self):
        if self.path == "/redirect-target":
            MockShortIo.redirected = True
            self.reply(500, {})
            return
        if not self.authenticated():
            return
        if self.path == "/slowheaders":
            time.sleep(3)
            try:
                self.reply(200, {})
            except (BrokenPipeError, ConnectionResetError):
                pass
        elif self.path in ("/slowdrip", "/oversize", "/denieddrip") or (
                self.path.startswith("/links/expand?") and
                urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query).get("path") == ["slowdrip"]):
            self.send_response(401 if self.path == "/denieddrip" else 200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "8388609" if self.path == "/oversize" else "100000")
            self.end_headers()
            try:
                # Each read progresses much faster than ReadWriteTimeout,
                # but completion would take over an hour without a total deadline.
                for _ in range(100000):
                    self.wfile.write(b" ")
                    self.wfile.flush()
                    time.sleep(0.05)
            except (BrokenPipeError, ConnectionResetError):
                pass
        elif self.path == "/seconds":
            self.reply(429, {}, {"Retry-After": "7"})
        elif self.path == "/date":
            self.reply(503, {}, {"Retry-After": email.utils.formatdate(time.time() + 120, usegmt=True)})
        elif self.path == "/redirect":
            self.reply(302, {}, {"Location": "/redirect-target"})
        else:
            parsed = urllib.parse.urlparse(self.path)
            assert parsed.path == "/links/expand"
            query = urllib.parse.parse_qs(parsed.query)
            assert query["domain"] == [self.expected_domain]
            self.lookups.append((self.expected_domain, time.monotonic()))
            path = query["path"][0]
            if path in self.values:
                self.reply(200, {"path": path, "originalURL": self.values[path]})
            else:
                self.reply(404, {"error": "not found"})


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: shortio_e2e.py /absolute/Zsd.exe /absolute/ZsdShortIoTest.exe")
    mono = shutil.which("mono")
    assert mono
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MockShortIo)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    endpoint = "http://127.0.0.1:%d/" % server.server_port
    try:
        with tempfile.TemporaryDirectory(prefix="zsd-shortio-offline-") as root:
            # Deliberately do not inherit or inspect any real credential.
            env = {"HOME": root, "PATH": os.defpath, "SHORT_IO_API_KEY": "offline-test-only",
                   "SHORT_IO_DOMAIN": "offline.example"}
            def run(args, expected=0, environment=None, executable=None, cwd=None):
                result = subprocess.run([mono, executable or sys.argv[1]] + args, cwd=cwd or root,
                                        env=env if environment is None else environment,
                                        capture_output=True, text=True, timeout=60)
                assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
                assert "offline-test-only" not in result.stdout + result.stderr
                return result.stdout

            wire = run(["--wire", endpoint], executable=sys.argv[2])
            assert "ZsdShortIoWireTest OK" in wire
            print(wire.strip())
            assert not MockShortIo.redirected
            source = pathlib.Path(root) / "input"
            source.mkdir()
            (source / "empty").mkdir()
            original = b"Short.io local byte-exact archive fixture.\n" * 80
            (source / "file.txt").write_bytes(original)
            (source / "shared.txt").write_bytes(original)
            (source / "zero.bin").write_bytes(b"")
            rng = random.Random(42)
            binary = bytes(rng.randrange(256) for _ in range(7000))
            additional = bytes(rng.randrange(256) for _ in range(9000))
            (source / "binary.dat").write_bytes(binary)
            extra = pathlib.Path(root) / "extra.bin"
            extra.write_bytes(additional)
            inputs = [str(source), str(extra)]
            upload = ["upload", "--endpoint", endpoint] + inputs
            missing = run(upload, expected=2, environment={
                "HOME": root, "PATH": os.defpath, "SHORT_IO_DOMAIN": "offline.example"})
            assert "SHORT_IO_API_KEY" in missing and not MockShortIo.writes
            failed = run(upload, expected=2, environment={
                "HOME": root, "PATH": os.defpath, "SHORT_IO_API_KEY": "invalid-offline-test",
                "SHORT_IO_DOMAIN": "offline.example"})
            assert "HTTP 401" in failed and "ZSD entry:" not in failed and not MockShortIo.writes
            packed = pathlib.Path(root) / "all-inputs.zsd"
            run(["pack", "--output", str(packed)] + inputs)
            uploaded = run(upload)
            ref = re.search(r"ZSD entry: (zsd-sio-v1:offline\.example:[A-Za-z0-9_]+)", uploaded)
            assert ref
            assert len(MockShortIo.chunks) > 1 and MockShortIo.metadata
            reconstructed = b"".join(MockShortIo.chunks)
            assert reconstructed == packed.read_bytes()
            assert reconstructed[:6] == b"ZSX3\x00\x01"
            assert int.from_bytes(reconstructed[46:54], "big") == len(reconstructed) - 54
            assert reconstructed[54:58] == b"ZSL1"
            assert int.from_bytes(reconstructed[6:14], "big") == int.from_bytes(reconstructed[58:62], "little")
            captured = pathlib.Path(root) / "transport-capture.zsd"
            captured.write_bytes(reconstructed)
            run(["unpack", "--output", str(pathlib.Path(root) / "capture-restored"), str(captured)])
            assert (pathlib.Path(root) / "capture-restored/input/shared.txt").read_bytes() == original
            assert (pathlib.Path(root) / "capture-restored/input/file.txt").read_bytes() == original
            assert (pathlib.Path(root) / "capture-restored/input/binary.dat").read_bytes() == binary
            assert (pathlib.Path(root) / "capture-restored/extra.bin").read_bytes() == additional
            assert (pathlib.Path(root) / "capture-restored/input/zero.bin").read_bytes() == b""
            assert (pathlib.Path(root) / "capture-restored/input/empty").is_dir()
            assert any("+" in value[len("https://pex.wf/#"):] for value in MockShortIo.values.values())
            assert any("/" in value[len("https://pex.wf/#"):] for value in MockShortIo.values.values())
            assert any(value.endswith("=") for value in MockShortIo.values.values())
            assert len(MockShortIo.writes) >= 2
            assert all(b - a >= 2.95 for a, b in zip(MockShortIo.writes, MockShortIo.writes[1:]))
            count = len(MockShortIo.values)
            resumed = run(upload)
            assert "ZSD entry: " + ref.group(1) in resumed and len(MockShortIo.values) == count
            run(["get", "--endpoint", endpoint, "--output", str(pathlib.Path(root) / "restored"), ref.group(1)])
            assert (pathlib.Path(root) / "restored/input/file.txt").read_bytes() == original
            assert (pathlib.Path(root) / "restored/input/shared.txt").read_bytes() == original
            assert (pathlib.Path(root) / "restored/input/zero.bin").read_bytes() == b""
            assert (pathlib.Path(root) / "restored/input/binary.dat").read_bytes() == binary
            assert (pathlib.Path(root) / "restored/extra.bin").read_bytes() == additional
            assert (pathlib.Path(root) / "restored/input/empty").is_dir()

            # Change only the stored carriers, not reference paths or payloads.
            # This represents records written by the old Short.io implementation.
            new_values = MockShortIo.values.copy()
            MockShortIo.values = {
                path: "http://ok.com/?" + value[len("https://pex.wf/#"):].replace("+", ".").replace("/", "_")
                for path, value in new_values.items()
            }
            old_output = pathlib.Path(root) / "restored-old"
            run(["get", "--endpoint", endpoint, "--output", str(old_output), ref.group(1)])
            assert (old_output / "input/file.txt").read_bytes() == original
            assert (old_output / "input/shared.txt").read_bytes() == original
            assert (old_output / "input/zero.bin").read_bytes() == b""
            assert (old_output / "input/binary.dat").read_bytes() == binary
            assert (old_output / "extra.bin").read_bytes() == additional
            assert (old_output / "input/empty").is_dir()
            MockShortIo.values = new_values
            root_path = ref.group(1).split(":")[2]
            root_carrier = new_values[root_path]
            invalid_carriers = [
                " https://pex.wf/#AAAA", "https://pex.wf/#AAAA\n",
                "http://pex.wf/#AAAA", "https://pex.wf.evil/#AAAA",
                "https://pex.wf/#._8=", "https://pex.wf/#%2B%2F8%3D",
                "https://pex.wf/#+/8", "https://pex.wf/#+/9=",
                "https://pex.wf/#" + base64.b64encode(bytes(1451)).decode("ascii"),
            ]
            for index, invalid in enumerate(invalid_carriers):
                MockShortIo.values[root_path] = invalid
                rejected = pathlib.Path(root) / ("invalid-%d" % index)
                failure = run(["get", "--endpoint", endpoint, "--output", str(rejected), ref.group(1)], expected=2)
                assert "provider returned" in failure and not rejected.exists()
            MockShortIo.values[root_path] = root_carrier
            # Custom reference wins over both the compiled default and an unrelated
            # environment domain, but never over an explicit conflicting flag.
            no_domain = {k: v for k, v in env.items() if k != "SHORT_IO_DOMAIN"}
            assert "SHORT_IO_DOMAIN" not in no_domain
            for label, environment in (
                    ("no-env", no_domain), ("other-env", dict(env, SHORT_IO_DOMAIN="other.example"))):
                run(["get", "--endpoint", endpoint, "--output", str(pathlib.Path(root) / label), ref.group(1)],
                    environment=environment)
            before_requests = len(MockShortIo.lookups)
            failure = run(["get", "--endpoint", endpoint, "--domain", "other.example", ref.group(1)], expected=2)
            assert "conflicts with explicit --domain" in failure
            assert len(MockShortIo.lookups) == before_requests
            # Explicit domain beats environment on uploads and bare lookups.
            explicit = run(["upload", "--endpoint", endpoint, "--domain", "offline.example"] + inputs,
                           environment=dict(env, SHORT_IO_DOMAIN="other.example"))
            assert "ZSD entry: " + ref.group(1) in explicit
            run(["get", "--endpoint", endpoint, "--domain", "offline.example",
                 "--output", str(pathlib.Path(root) / "explicit"), root_path],
                environment=dict(env, SHORT_IO_DOMAIN="other.example"))
            run(["get", "--provider", "legacy", "--endpoint", endpoint,
                 "--output", str(pathlib.Path(root) / "typed-over-legacy"), ref.group(1)], environment=no_domain)

            # Standalone defaults: no Replit/domain configuration, no provider,
            # no opt-in flag, no output option; only the existing loopback seam.
            MockShortIo.expected_domain = "pex.s.gy"
            default_upload = run(upload, environment=no_domain)
            default_id = re.search(r"^ZSD entry: ([A-Za-z0-9_]{5,30})$", default_upload, re.M)
            assert default_id and "zsd-sio-v1:" not in default_upload
            slug = default_id.group(1)
            assert "Retrieve with: zsd get --endpoint " + endpoint + " " + slug in default_upload
            cwd = pathlib.Path(root) / "current-directory"
            cwd.mkdir()
            (cwd / "unrelated.txt").write_bytes(b"keep this")
            (cwd / "input").mkdir()  # Merge into existing nested directories.
            (cwd / "input/unrelated.txt").write_bytes(b"keep nested")
            get = ["get", "--endpoint", endpoint, slug]
            start = len(MockShortIo.lookups)
            run(get, environment=no_domain, cwd=cwd)
            lookup_times = [stamp for _, stamp in MockShortIo.lookups[start:]]
            assert all(b - a >= .095 for a, b in zip(lookup_times, lookup_times[1:]))
            assert not (cwd / slug).exists()
            assert (cwd / "input/file.txt").read_bytes() == original
            assert (cwd / "input/shared.txt").read_bytes() == original
            assert (cwd / "input/binary.dat").read_bytes() == binary
            assert (cwd / "extra.bin").read_bytes() == additional
            assert (cwd / "input/empty").is_dir()
            assert (cwd / "input/zero.bin").read_bytes() == b""

            def snapshot(path):
                return {str(p.relative_to(path)): None if p.is_dir() else p.read_bytes()
                        for p in path.rglob("*")}

            (cwd / "input/file.txt").write_bytes(b"existing file")
            before = snapshot(cwd)
            failure = run(get, expected=2, environment=no_domain, cwd=cwd)
            assert "refusing to overwrite" in failure and snapshot(cwd) == before
            run(get + ["--force"], environment=no_domain, cwd=cwd)
            assert (cwd / "input/file.txt").read_bytes() == original
            assert (cwd / "unrelated.txt").read_bytes() == b"keep this"
            assert (cwd / "input/unrelated.txt").read_bytes() == b"keep nested"
            assert not list(cwd.glob(".zsd-*"))
            empty_cwd = pathlib.Path(root) / "empty-current-directory"
            empty_cwd.mkdir()
            run(get, environment=no_domain, cwd=empty_cwd)
            assert (empty_cwd / "extra.bin").read_bytes() == additional
            run(get + ["--output", str(pathlib.Path(root) / "default-output")], environment=no_domain)
            MockShortIo.values["et56g4"] = MockShortIo.values[slug]
            run(["get", "--endpoint", endpoint, "et56g4", "--output", str(pathlib.Path(root) / "six-char")],
                environment=no_domain)
            missing_key = {k: v for k, v in no_domain.items() if k != "SHORT_IO_API_KEY"}
            failure = run(get, expected=2, environment=missing_key, cwd=cwd)
            assert "SHORT_IO_API_KEY" in failure
            # If an explicit default domain overrides a custom environment, emit
            # a full ref rather than a bare ID that the same environment misroutes.
            portable = run(["upload", "--endpoint", endpoint, "--domain", "pex.s.gy",
                            "--allow-public-upload"] + inputs, environment=env)
            assert "ZSD entry: zsd-sio-v1:pex.s.gy:" + slug in portable
            # Full default references still work, even when env specifies a different domain.
            run(["get", "--endpoint", endpoint, "--output", str(pathlib.Path(root) / "typed-default"),
                 "zsd-sio-v1:pex.s.gy:" + slug], environment=env)
            for bad in ["", "abc", "../et56g4", "a" * 31, "et56g4/evil", "https://pex.s.gy/et56g4",
                        "et56g4?x", "et56g4\n", "zsd-sio-v1:pex.s.gy:et56g4:extra"]:
                count = len(MockShortIo.lookups)
                run(["get", "--endpoint", endpoint, bad], expected=2, environment=no_domain, cwd=cwd)
                assert len(MockShortIo.lookups) == count
            # Valid transport digests surrounding a corrupt container: all archive
            # validation must still precede any writes to an existing cwd.
            bad_container = b"ZSX3\x00\x7f" + b"\x00" * 48
            leaf = "badcontainer"
            carrier = lambda data: "https://pex.wf/#" + base64.b64encode(data).decode("ascii")
            length_digest = len(bad_container).to_bytes(8, "big") + hashlib.sha256(bad_container).digest()
            node = (b"ZSDT\x00\x01\x00" + len(bad_container).to_bytes(8, "big") +
                    hashlib.sha256(length_digest).digest() + b"\x00\x01" +
                    length_digest + len(leaf).to_bytes(2, "big") + leaf.encode("ascii"))
            MockShortIo.values[leaf] = carrier(bad_container)
            MockShortIo.values["badroot"] = carrier(node)
            before = snapshot(cwd)
            failure = run(["get", "--endpoint", endpoint, "--force", "badroot"],
                          expected=2, environment=no_domain, cwd=cwd)
            assert "version" in failure and snapshot(cwd) == before
            # A symlink parent must not redirect extraction, even with --force.
            unsafe = pathlib.Path(root) / "unsafe-current-directory"
            unsafe.mkdir()
            outside = pathlib.Path(root) / "outside"
            outside.mkdir()
            (unsafe / "input").symlink_to(outside, target_is_directory=True)
            run(get + ["--force"], expected=2, environment=no_domain, cwd=unsafe)
            assert list(outside.iterdir()) == [] and not (unsafe / "extra.bin").exists()
            assert (unsafe / "input").is_symlink()
            # Force may replace files, never directories. A late commit failure
            # restores earlier overwritten files and removes new files/directories.
            rollback = pathlib.Path(root) / "rollback-current-directory"
            (rollback / "input/shared.txt").mkdir(parents=True)
            (rollback / "input/shared.txt/keep").write_bytes(b"directory must remain")
            (rollback / "extra.bin").write_bytes(b"restore overwritten first file")
            (rollback / "input/file.txt").write_bytes(b"restore overwritten nested file")
            before = snapshot(rollback)
            failure = run(get + ["--force"], expected=2, environment=no_domain, cwd=rollback)
            assert "rolled back" in failure and snapshot(rollback) == before
            print("Short.io .NET mock e2e OK: exact canonical carrier in every POST, shortening-domain config, "
                  "multichunk multi-input archive, secure config errors, pacing, stable rerun, "
                  "byte-exact new/old carrier extraction, invalid carrier rejection; standalone defaults, "
                  "bare IDs, custom reference/env/flag precedence, current-directory merge/collision/force, "
                  "malformed IDs, corrupt container fail-before-write, symlink protection")
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()