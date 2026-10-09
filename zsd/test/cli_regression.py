#!/usr/bin/env python3
"""Regression coverage for the compiled .NET 4 ZSD command-line program.

This deliberately uses the loopback provider from mock_e2e.py.  It never
contacts a public shortening service and passes wildcard arguments directly
to subprocess rather than through a shell.
"""
import base64
import hashlib
import http.server
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest

from mock_e2e import MockIsGd
from format_fixtures import ENTRIES, container, legacy_fixture, record_stream, stored_stream


EXE = os.environ.get("ZSD_EXE", "/tmp/Zsd-verified.exe")
MONO = shutil.which("mono")


def digest(path):
    hasher = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(65536), b""):
            hasher.update(block)
    return hasher.hexdigest()


def tree_hashes(root):
    """Return directory names and SHA-256s, relative to root."""
    directories = set()
    files = {}
    for base, names, filenames in os.walk(root):
        rel_base = os.path.relpath(base, root)
        if rel_base != ".":
            directories.add(rel_base.replace(os.sep, "/"))
        for name in names:
            rel = os.path.relpath(os.path.join(base, name), root)
            directories.add(rel.replace(os.sep, "/"))
        for name in filenames:
            rel = os.path.relpath(os.path.join(base, name), root)
            files[rel.replace(os.sep, "/")] = digest(os.path.join(base, name))
    return directories, files


def carrier_bytes(carrier):
    prefix = "http://ok.com/?"
    if not carrier.startswith(prefix):
        raise AssertionError("mock has a non-carrier value: %r" % (carrier,))
    encoded = carrier[len(prefix):].replace(".", "+").replace("_", "/")
    return base64.b64decode(encoded)


def make_carrier(raw):
    return "http://ok.com/?" + base64.b64encode(raw).decode("ascii").replace(
        "+", ".").replace("/", "_")


def tree_node(raw):
    """Parse the small, documented ZSDT envelope used by the local mock."""
    if raw[:4] != b"ZSDT":
        raise AssertionError("expected ZSDT node, got %r" % (raw[:4],))
    if len(raw) < 49:
        raise AssertionError("truncated ZSDT node")
    version = int.from_bytes(raw[4:6], "big")
    kind = raw[6]
    length = int.from_bytes(raw[7:15], "big")
    count = int.from_bytes(raw[47:49], "big")
    if version != 1 or kind not in (0, 1):
        raise AssertionError("unexpected ZSDT header version=%d kind=%d" % (version, kind))
    at = 49
    items = []
    for _ in range(count):
        if at + 42 > len(raw):
            raise AssertionError("truncated ZSDT item")
        item_length = int.from_bytes(raw[at:at + 8], "big")
        at += 8 + 32
        id_length = int.from_bytes(raw[at:at + 2], "big")
        at += 2
        if at + id_length > len(raw):
            raise AssertionError("truncated ZSDT item id")
        short_id = raw[at:at + id_length].decode("ascii")
        at += id_length
        items.append((short_id, item_length))
    if at != len(raw):
        raise AssertionError("unexpected ZSDT trailing data")
    if sum(item[1] for item in items) != length:
        raise AssertionError("ZSDT declared length does not match items")
    return kind, items


class CliRegression(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        if not MONO:
            raise RuntimeError("mono is required to run the .NET 4 regression suite")
        if not os.path.isfile(EXE):
            raise RuntimeError("compiled executable is missing: %s" % EXE)

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="zsd-cli-regression-")
        self.command = [MONO, EXE]

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def invoke(self, args, expect=0, cwd=None):
        command = self.command + list(args)
        result = subprocess.run(
            command,
            cwd=cwd or self.root,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            universal_newlines=True,
            timeout=45,
        )
        if expect == 0 and result.returncode != 0:
            self.fail("command failed (%d): %r\n%s" % (
                result.returncode, command, result.stdout))
        if expect != 0 and result.returncode == 0:
            self.fail("command unexpectedly succeeded: %r\n%s" % (command, result.stdout))
        return result.stdout

    def require_failure(self, args, cwd=None):
        return self.invoke(args, expect=2, cwd=cwd)

    def write(self, relative, data):
        path = os.path.join(self.root, relative)
        parent = os.path.dirname(path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(data)
        return path

    def test_offline_pack_unpack_and_force(self):
        source = os.path.join(self.root, "source tree")
        os.makedirs(os.path.join(source, "empty-folder"))
        os.makedirs(os.path.join(source, "nested", "also-empty"))
        self.write("source tree/repetitive.txt", (b"ZSD repeated payload.\n" * 6000)[:102400])
        self.write("source tree/random.bin", os.urandom(102400))
        self.write("source tree/empty.bin", b"")
        self.write("source tree/nested/odd name [brackets] #.dat", b"odd filename\n")
        expected = tree_hashes(source)

        archive = os.path.join(self.root, "bundle.zsd")
        self.invoke(["pack", "--output", archive, source])
        original_archive = digest(archive)

        restored = os.path.join(self.root, "restored")
        self.invoke(["unpack", "--output", restored, archive])
        self.assertEqual(expected, tree_hashes(os.path.join(restored, "source tree")))

        # Neither command may silently replace a result.
        unpack_error = self.require_failure(["unpack", "--output", restored, archive])
        self.assertIn("refusing to overwrite", unpack_error)
        self.assertEqual(expected, tree_hashes(os.path.join(restored, "source tree")))
        self.write("source tree/repetitive.txt", b"changed source data")
        pack_error = self.require_failure(["pack", "--output", archive, source])
        self.assertIn("refusing to overwrite", pack_error)
        self.assertEqual(original_archive, digest(archive))

        # --force replaces both an existing archive and existing extracted data.
        self.invoke(["pack", "--force", "--output", archive, source])
        self.assertNotEqual(original_archive, digest(archive))
        self.write("restored/source tree/repetitive.txt", b"locally modified")
        self.invoke(["unpack", "--force", "--output", restored, archive])
        self.assertEqual(tree_hashes(source), tree_hashes(os.path.join(restored, "source tree")))

    def test_wildcards_and_multiple_input_paths(self):
        wild = os.path.join(self.root, "wild cards")
        other = os.path.join(self.root, "other root")
        os.makedirs(wild)
        os.makedirs(other)
        self.write("wild cards/alpha.txt", b"alpha")
        self.write("wild cards/beta.txt", b"beta")
        self.write("wild cards/question1.dat", b"one")
        self.write("wild cards/question12.dat", b"not selected")
        self.write("wild cards/skip.bin", b"not selected")
        self.write("other root/second.txt", b"second root")

        archive = os.path.join(self.root, "wildcards.zsd")
        # '*' and '?' are literal argv values here: no shell can expand them.
        self.invoke([
            "pack", "--output", archive,
            os.path.join(wild, "*.txt"),
            os.path.join(wild, "question?.dat"),
            other,
        ])
        output = os.path.join(self.root, "wild-output")
        self.invoke(["unpack", "--output", output, archive])
        self.assertEqual(digest(os.path.join(wild, "alpha.txt")),
                         digest(os.path.join(output, "wild cards", "alpha.txt")))
        self.assertEqual(digest(os.path.join(wild, "beta.txt")),
                         digest(os.path.join(output, "wild cards", "beta.txt")))
        self.assertEqual(digest(os.path.join(wild, "question1.dat")),
                         digest(os.path.join(output, "wild cards", "question1.dat")))
        self.assertEqual(digest(os.path.join(other, "second.txt")),
                         digest(os.path.join(output, "other root", "second.txt")))
        self.assertFalse(os.path.exists(os.path.join(output, "wild cards", "question12.dat")))
        self.assertFalse(os.path.exists(os.path.join(output, "wild cards", "skip.bin")))

    def test_conflicts_and_pack_output_in_input_are_rejected(self):
        one = self.write("one/duplicate.txt", b"one")
        two = self.write("two/duplicate.txt", b"two")
        conflict = os.path.join(self.root, "conflict.zsd")
        error = self.require_failure(["pack", "--output", conflict, one, two])
        self.assertIn("collide", error)
        self.assertFalse(os.path.exists(conflict))

        upper = self.write("upper/Case.txt", b"upper")
        lower = self.write("lower/case.txt", b"lower")
        case_archive = os.path.join(self.root, "case.zsd")
        error = self.require_failure(["pack", "--output", case_archive, upper, lower])
        self.assertIn("collide", error)
        self.assertFalse(os.path.exists(case_archive))

        same = self.write("same.zsd", b"existing archive data must remain")
        before = digest(same)
        error = self.require_failure(["pack", "--force", "--output", same, same])
        self.assertIn("included by an input", error)
        self.assertEqual(before, digest(same))

    def test_corrupt_archive_does_not_create_output(self):
        source = self.write("damage-source.txt", os.urandom(4096))
        archive = os.path.join(self.root, "good.zsd")
        self.invoke(["pack", "--output", archive, source])
        damaged = os.path.join(self.root, "damaged.zsd")
        with open(archive, "rb") as handle:
            data = bytearray(handle.read())
        data[-1] ^= 0x80
        with open(damaged, "wb") as handle:
            handle.write(data)
        output = os.path.join(self.root, "must-not-exist")
        self.require_failure(["unpack", "--output", output, damaged])
        self.assertFalse(os.path.exists(output), "damaged archive created extraction output")

    def test_zsx3_headers_legacy_fixture_and_fail_before_extract(self):
        # Explicit, independent pre-ZSX3 wire fixture (not the current encoder).
        legacy = self.write("legacy.zsd", legacy_fixture())
        restored = os.path.join(self.root, "legacy-restored")
        self.invoke(["unpack", "--output", restored, legacy])
        for path, directory, data in ENTRIES:
            target = os.path.join(restored, path)
            if directory:
                self.assertTrue(os.path.isdir(target))
            else:
                with open(target, "rb") as handle:
                    self.assertEqual(data, handle.read())

        packed = os.path.join(self.root, "new.zsd")
        message = self.invoke(["pack", "--output", packed, os.path.join(restored, "fixture")])
        self.assertIn("ZSX3 v1", message)
        with open(packed, "rb") as handle:
            good = handle.read()
        archive = record_stream()
        self.assertEqual(b"ZSX3\x00\x01", good[:6])
        self.assertEqual(len(archive), int.from_bytes(good[6:14], "big"))
        self.assertEqual(hashlib.sha256(archive).digest(), good[14:46])
        self.assertEqual(len(good) - 54, int.from_bytes(good[46:54], "big"))
        self.assertEqual(b"ZSL1", good[54:58])
        self.assertEqual(len(archive), int.from_bytes(good[58:62], "little"))
        self.assertEqual(1, int.from_bytes(good[62:66], "little"))
        self.assertEqual(bytes(4), good[66:70])
        new_output = os.path.join(self.root, "new-restored")
        self.invoke(["unpack", "--output", new_output, packed])
        self.assertEqual(tree_hashes(restored), tree_hashes(new_output))
        self.assertIn("ZSX3 v1", self.invoke(["--help"]))
        bad = [good[:n] for n in (0, 3, 5, 13, 45, 53, 54, len(good) - 1)]
        bad += [good + b"\0"]
        for offset in (0, 5, 6, 13, 14, 46, 53, 54, 58, len(good) - 1):
            altered = bytearray(good)
            altered[offset] ^= 1
            bad.append(bytes(altered))
        # Bounds are rejected before allocation, including a length above 256 MiB.
        bad.append(good[:6] + (0x10000001).to_bytes(8, "big") + good[14:])
        bad.append(good[:46] + (0x40000001).to_bytes(8, "big") + good[54:])
        # Valid container digest, invalid *last* file digest: no early files extracted.
        broken_records = archive[:-1] + bytes([archive[-1] ^ 1])
        bad.append(container(broken_records, b"ZSX3"))
        # Correct outer checksum cannot opt v1 into a different inner format/codec.
        bad.append(container(b"ZSDP\0\2" + archive[6:], b"ZSX3"))
        bad.append(container(b"OTHER!" + archive[6:], b"ZSX3"))
        bad.append(container(archive, b"ZSX3", stored_stream(archive)[16:]))
        for index, data in enumerate(bad):
            damaged = self.write("bad-%d.zsd" % index, data)
            output = os.path.join(self.root, "absent-%d" % index)
            self.require_failure(["unpack", "--output", output, damaged])
            self.assertFalse(os.path.exists(output), "invalid archive created extraction output")
        # Also leave an existing destination untouched even with --force.
        protected = os.path.join(self.root, "protected")
        sentinel = self.write("protected/fixture/a.bin", b"keep existing content")
        damaged = self.write("bad-records.zsd", container(broken_records, b"ZSX3"))
        self.require_failure(["unpack", "--force", "--output", protected, damaged])
        with open(sentinel, "rb") as handle:
            self.assertEqual(b"keep existing content", handle.read())
        self.assertEqual({"fixture/a.bin"}, set(tree_hashes(protected)[1]))

    @unittest.skipIf(os.name == "nt", "POSIX filenames are required to create trailing spaces")
    def test_windows_device_and_trailing_component_names_are_rejected(self):
        for name in ("CON.txt", "trailing.", "trailing "):
            source = self.write(name, b"must not be archived")
            archive = os.path.join(self.root, name.replace(" ", "_") + ".zsd")
            error = self.require_failure(["pack", "--output", archive, source])
            self.assertTrue(
                "Windows archive path component" in error
                or "reserved Windows archive path component" in error,
                error)
            self.assertFalse(os.path.exists(archive))

    @unittest.skipIf(os.name == "nt", "symlink creation is exercised on POSIX")
    def test_symlink_source_and_ancestor_are_rejected(self):
        target = os.path.join(self.root, "real")
        os.makedirs(os.path.join(target, "child"))
        self.write("real/child/data.txt", b"real data")

        source_link = os.path.join(self.root, "source-link")
        try:
            os.symlink(target, source_link)
        except OSError as exc:
            self.skipTest("cannot create symlinks: %s" % exc)
        output = os.path.join(self.root, "link-source.zsd")
        error = self.require_failure(["pack", "--output", output, source_link])
        self.assertIn("symbolic link or reparse point", error)
        self.assertFalse(os.path.exists(output))

        ancestor_link = os.path.join(self.root, "ancestor-link")
        os.symlink(target, ancestor_link)
        output = os.path.join(self.root, "link-ancestor.zsd")
        error = self.require_failure([
            "pack", "--output", output, os.path.join(ancestor_link, "child", "data.txt")
        ])
        self.assertIn("symbolic link or reparse point", error)
        self.assertFalse(os.path.exists(output))

    def test_loopback_multileaf_multilevel_and_damaged_leaves(self):
        MockIsGd.values = {}
        MockIsGd.uploads = []
        MockIsGd.next_id = 0
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MockIsGd)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            endpoint = "http://127.0.0.1:%d/" % server.server_port
            self.assertRegex(endpoint, r"^http://127\.0\.0\.1:\d+/$")
            source = os.path.join(self.root, "network-root")
            os.makedirs(os.path.join(source, "deep", "level", "empty"))
            payload = self.write("network-root/deep/level/random-100k.bin", os.urandom(102400))
            self.write("network-root/small.txt", b"network integration")
            expected = tree_hashes(source)

            uploaded = self.invoke([
                "upload", "--provider", "legacy", "--endpoint", endpoint, "--retries", "0", "--pace-ms", "0", source
            ])
            match = re.search(r"ZSD entry: ([A-Za-z0-9_]{5,30})", uploaded)
            self.assertIsNotNone(match, uploaded)
            root_id = match.group(1)
            self.assertGreater(len(MockIsGd.uploads), 55, "random data did not make many leaves")
            root_kind, root_items = tree_node(carrier_bytes(MockIsGd.values[root_id]))
            self.assertEqual(1, root_kind, "100 KiB transfer must have a multi-level root")
            self.assertGreaterEqual(len(root_items), 2)
            child_kind, child_items = tree_node(
                carrier_bytes(MockIsGd.values[root_items[0][0]]))
            self.assertEqual(0, child_kind, "root children must be leaf-list nodes")
            leaf_id = child_items[0][0]

            restored = os.path.join(self.root, "network-restored")
            self.invoke([
                "get", "--provider", "legacy", "--endpoint", endpoint, "--retries", "0", "--pace-ms", "0",
                "--output", restored, root_id
            ])
            self.assertEqual(expected, tree_hashes(os.path.join(restored, "network-root")))
            self.assertEqual(digest(payload), digest(os.path.join(
                restored, "network-root", "deep", "level", "random-100k.bin")))

            original_leaf = MockIsGd.values[leaf_id]
            corrupt = bytearray(carrier_bytes(original_leaf))
            corrupt[0] ^= 0x01
            MockIsGd.values[leaf_id] = make_carrier(bytes(corrupt))
            absent = os.path.join(self.root, "corrupt-leaf-output")
            self.require_failure([
                "get", "--provider", "legacy", "--endpoint", endpoint, "--retries", "0", "--pace-ms", "0",
                "--output", absent, root_id
            ])
            self.assertFalse(os.path.exists(absent), "corrupted leaf created output")

            MockIsGd.values[leaf_id] = original_leaf
            del MockIsGd.values[leaf_id]
            protected = os.path.join(self.root, "protected-output")
            os.makedirs(protected)
            sentinel = self.write("protected-output/unchanged.txt", b"do not change")
            before = digest(sentinel)
            self.require_failure([
                "get", "--provider", "legacy", "--endpoint", endpoint, "--retries", "0", "--pace-ms", "0",
                "--output", protected, root_id
            ])
            self.assertEqual(before, digest(sentinel))
            self.assertFalse(os.path.exists(os.path.join(protected, "network-root")))
        finally:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    unittest.main(verbosity=2, failfast=True)