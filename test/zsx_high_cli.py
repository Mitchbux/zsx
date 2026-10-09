#!/usr/bin/env python3
"""Black-box tests for the native ZSX high-mode command line interface."""

from __future__ import annotations

import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


CHUNK_SIZE = 0xC00000
TIMEOUT = 120
SENTINEL = b"pre-existing output must survive"
CLI_BINARY = "./zsx"


class HighCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.binary = Path(CLI_BINARY).resolve()
        if not cls.binary.is_file():
            raise unittest.SkipTest(f"ZSX executable not found: {cls.binary}")

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="zsx-high-test-")
        self.directory = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_cli(
        self, *arguments: os.PathLike[str] | str, check: bool = True
    ) -> subprocess.CompletedProcess[bytes]:
        result = subprocess.run(
            [str(self.binary), *(str(argument) for argument in arguments)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=TIMEOUT,
        )
        if check and result.returncode:
            self.fail(
                f"{self.binary.name} {' '.join(map(str, arguments))} failed "
                f"with {result.returncode}\nstdout={result.stdout!r}\n"
                f"stderr={result.stderr!r}"
            )
        return result

    def archive(self, payload: bytes, high: bool, stem: str = "data") -> tuple[Path, bytes]:
        source = self.directory / f"{stem}.bin"
        output = self.directory / f"{stem}.{'high' if high else 'base'}.zsx"
        source.write_bytes(payload)
        arguments = ("-h", source, output) if high else (source, output)
        result = self.run_cli(*arguments)
        return output, result.stderr

    def roundtrip(self, payload: bytes, stem: str) -> tuple[bytes, bytes]:
        archive, log = self.archive(payload, True, stem)
        restored = self.directory / f"{stem}.restored"
        self.run_cli("-d", archive, restored)
        self.assertEqual(restored.read_bytes(), payload)
        return archive.read_bytes(), log

    @staticmethod
    def chunk_marks(archive: bytes) -> list[bool]:
        if len(archive) < 16:
            raise AssertionError("archive has no complete header")
        count = struct.unpack_from("<I", archive, 8)[0]
        offset = 16
        marks = []
        for _ in range(count):
            if offset + 4 > len(archive):
                raise AssertionError("truncated chunk length")
            encoded = struct.unpack_from("<I", archive, offset)[0]
            marks.append(bool(encoded & 0x80000000))
            offset += 4 + (encoded & 0x7FFFFFFF)
        if offset != len(archive):
            raise AssertionError("archive chunk framing does not consume file")
        return marks

    def test_empty_odd_binary_utf8_seeded_random_and_vocabulary(self) -> None:
        rng = random.Random(0x5A58)
        payloads = {
            "empty": b"",
            "odd": bytes(range(256)) + b"\x00\xff\x80",
            "utf8": (
                "ZSX handles naïve café text, Ελληνικά, 日本語, and emoji 🗜️.\n"
                * 97
            ).encode("utf-8"),
            "random": rng.randbytes(131_071),
            "vocabulary": (
                b"compressor decoder dictionary archive transform token hash "
                b"stream source buffer function return const unsigned length "
                b"compressor decoder dictionary archive transform token hash\n"
            )
            * 12_000,
        }
        saw_hash = False
        for name, payload in payloads.items():
            with self.subTest(name=name):
                archive, log = self.roundtrip(payload, name)
                self.assertIn(archive[:4], (b"ZSX1", b"ZSH1"))
                saw_hash |= archive[:4] == b"ZSH1" and any(self.chunk_marks(archive))
                self.assertIn(b"high chunk", log) if payload else None
        self.assertTrue(
            saw_hash,
            "no representative corpus selected a high-mode chunk; the high branch "
            "and its transform decoder were not exercised",
        )

    def test_help_and_default_paths(self) -> None:
        for option in ("-h", "--help"):
            result = self.run_cli(option)
            self.assertIn(b"Usage:", result.stdout)
            self.assertIn(b"-h INPUT [OUTPUT]", result.stdout)

        source = self.directory / "defaults.bin"
        payload = b"default filename behavior\n" * 50
        source.write_bytes(payload)
        self.run_cli(source)
        archive = Path(f"{source}.zsx")
        self.assertTrue(archive.is_file())
        source.unlink()
        self.run_cli("-d", archive)
        self.assertEqual(source.read_bytes(), payload)

        source.unlink()
        archive.unlink()
        source.write_bytes(payload)
        self.run_cli("-h", source)
        self.assertTrue(archive.is_file())

    def test_no_gain_is_byte_identical_and_high_never_larger(self) -> None:
        rng = random.Random(20250308)
        no_gain = rng.randbytes(257_003)
        baseline, _ = self.archive(no_gain, False, "nogain")
        high, _ = self.archive(no_gain, True, "nogain")
        self.assertEqual(high.read_bytes(), baseline.read_bytes())
        self.assertEqual(high.read_bytes()[:4], b"ZSX1")

        useful = (
            b"alpha beta gamma delta alpha beta gamma delta "
            b"function value result return function value result return\n"
        ) * 18_000
        baseline, _ = self.archive(useful, False, "size")
        high, _ = self.archive(useful, True, "size")
        self.assertLessEqual(high.stat().st_size, baseline.stat().st_size)

    def test_corruption_and_truncation_preserve_existing_output(self) -> None:
        payload = (
            b"realistic dictionary vocabulary repeated transform checksum token\n"
            * 10_000
        )
        good, _ = self.archive(payload, True, "valid")
        data = good.read_bytes()
        candidates = {
            "bad-magic": b"BAD!" + data[4:],
            "short-header": data[:11],
            "truncated-payload": data[:-1],
            "trailing-data": data + b"x",
        }
        if data[:4] == b"ZSH1":
            damaged = bytearray(data)
            damaged[-1] ^= 0x40
            candidates["damaged-high"] = bytes(damaged)

        for name, malformed in candidates.items():
            with self.subTest(name=name):
                archive = self.directory / f"{name}.zsx"
                output = self.directory / f"{name}.out"
                archive.write_bytes(malformed)
                output.write_bytes(SENTINEL)
                result = self.run_cli("-d", archive, output, check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(output.read_bytes(), SENTINEL)
                self.assertEqual(
                    list(self.directory.glob(f"{name}.out.zsx-tmp-*")), []
                )

    def test_mixed_multichunk_archive(self) -> None:
        # One incompressible full chunk followed by vocabulary makes both the
        # baseline fallback and hash candidate decisions occur in one archive.
        rng = random.Random(0xC0DEC0DE)
        first = rng.randbytes(CHUNK_SIZE)
        second = (
            b"request response parser encoder dictionary token function value "
            b"request response parser encoder dictionary token function value\n"
        ) * 14_000
        payload = first + second
        archive, _ = self.roundtrip(payload, "multichunk")
        marks = self.chunk_marks(archive)
        self.assertEqual(len(marks), 2)
        self.assertEqual(archive[:4], b"ZSH1")
        self.assertIn(False, marks)
        self.assertIn(True, marks)

    def test_noncanonical_high_modes_are_rejected(self) -> None:
        for payload in (b"", b"ordinary bytes"):
            archive, _ = self.archive(payload, False, "nohash")
            archive.write_bytes(b"ZSH1" + archive.read_bytes()[4:])
            output = self.directory / "protected"
            output.write_bytes(SENTINEL)
            self.assertNotEqual(self.run_cli("-d", archive, output, check=False).returncode, 0)
            self.assertEqual(output.read_bytes(), SENTINEL)

        # A valid benchmark-only sequential transform must not be accepted as
        # the production hash mode, even after baseline coding and valid CRCs.
        original = b"catcatcat"
        transform = (b"ZHP1\x01\x09" + struct.pack("<I", zlib.crc32(original))
                     + b"\x01\x03\x00cat" + b"\x82\x00" * 3)
        transform += struct.pack("<I", zlib.crc32(transform))
        inner, _ = self.archive(transform, False, "indices")
        inner_bytes = inner.read_bytes()
        marked = (b"ZSH1" + struct.pack("<III", len(original), 1, 0)
                  + struct.pack("<I", struct.unpack_from("<I", inner_bytes, 16)[0] | 0x80000000)
                  + inner_bytes[20:])
        inner.write_bytes(marked)
        output.write_bytes(SENTINEL)
        self.assertNotEqual(self.run_cli("-d", inner, output, check=False).returncode, 0)
        self.assertEqual(output.read_bytes(), SENTINEL)

    def test_output_alias_is_refused_without_modifying_input(self) -> None:
        source = self.directory / "alias.bin"
        original = b"alias safety" * 100
        source.write_bytes(original)
        result = self.run_cli("-h", source, source, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(source.read_bytes(), original)

        hardlink = self.directory / "hardlink.bin"
        try:
            os.link(source, hardlink)
        except OSError:
            return
        result = self.run_cli(source, hardlink, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(source.read_bytes(), original)


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        CLI_BINARY = sys.argv.pop(1)
    unittest.main()