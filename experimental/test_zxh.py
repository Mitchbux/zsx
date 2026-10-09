#!/usr/bin/env python3
"""Independent black-box native/libtcc and archive structure tests."""
import json
import os
from pathlib import Path
import random
import re
import struct
import subprocess
import sys
import tempfile
import time
import unittest

from zxh_generate import pack

ROOT = Path(__file__).resolve().parent.parent
BINARY = (ROOT/"zsx").resolve()
RESULTS = []


class ZXHTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="test-zxh-")
        self.p = Path(self.tmp.name)
        self.start = time.monotonic()

    def tearDown(self):
        RESULTS.append({"test": self.id(), "seconds": time.monotonic()-self.start})
        self.tmp.cleanup()

    def command(self, *args, good=True, binary=BINARY, cwd=None):
        result = subprocess.run([str(binary), *map(str, args)], capture_output=True,
                                timeout=120, cwd=cwd)
        self.assertEqual(result.returncode == 0, good, result.stderr.decode(errors="replace"))
        return result

    def archive(self, data, words=64):
        src, archive = self.p/"input", self.p/"bundle.zxh"
        src.write_bytes(data)
        stats = pack(src, archive, words=words, native=BINARY)
        return src, archive, stats

    def decode(self, archive, good=True):
        output = self.p/"restored"
        output.write_bytes(b"preserve me")
        result = self.command("-x", "--trust-generated-code", archive, output, good=good)
        if not good:
            self.assertEqual(output.read_bytes(), b"preserve me")
            self.assertFalse(list(self.p.glob("restored.zsx-tmp-*")))
        return output, result

    def rawparts(self, archive):
        data = archive.read_bytes()
        wanted = struct.unpack_from("<I", data, 4)[0]
        parts, at = [], 44
        for i in range(3):
            raw, size, mode = struct.unpack_from("<III", data, 8+12*i)
            section = data[at:at+size]
            at += size
            if mode:
                (self.p/"section.zsx").write_bytes(section)
                self.command("-d", self.p/"section.zsx", self.p/"section.raw")
                section = (self.p/"section.raw").read_bytes()
            self.assertEqual(len(section), raw)
            parts.append(section)
        self.assertEqual(at, len(data))
        return wanted, parts

    def rawarchive(self, wanted, parts):
        dest = self.p/"changed.zxh"
        dest.write_bytes(b"ZXH1"+struct.pack("<I", wanted)+
                         b"".join(struct.pack("<III", len(s), len(s), 0) for s in parts)+
                         b"".join(parts))
        return dest

    def test_empty_binary_unicode_case_and_dictionary(self):
        rng = random.Random(73)
        for data, words in [(b"", 64), (bytes(range(256))*8, 0),
                            (rng.randbytes(32768), 64),
                            (("hello HELLO Hello hElLo Straße 日本語 \0\n"*100).encode(), 64),
                            (b"a A AA aa Aa aA unknown words " * 100, 0)]:
            with self.subTest(size=len(data), words=words):
                src, archive, stats = self.archive(data, words)
                # Remove original before native execution. Change cwd as well.
                src.unlink()
                out, _ = self.decode(archive)
                self.assertEqual(out.read_bytes(), data)
                self.assertTrue(stats["source_budget_compliant"])
                self.assertEqual(stats["archive_bytes"], 44+sum(
                    s["stored_bytes"] for s in stats["sections"].values()))

    def test_predictions_hits_misses_and_dictionary_budget(self):
        data = (b"the quick brown fox and the slow brown dog.\n"*500 +
                b"unexpected ABC aBc AaA ABC aBc AaA\n"*20)
        _, archive, stats = self.archive(data)
        p = stats["predictions"]
        self.assertGreater(p["prediction_hits"], 0)
        self.assertGreater(p["events"], p["prediction_hits"])
        self.assertGreater(p["word_hits"], 0)
        self.assertGreater(p["letter_hits"], 0)
        self.assertLessEqual(stats["sections"]["dict"]["raw_bytes"], 32768)
        out, _ = self.decode(archive)
        self.assertEqual(out.read_bytes(), data)

    def test_trained_context_hits_misses_and_model_bounds(self):
        data = (b"the quick brown fox and the slow brown dog.\n"*500 +
                b"the quick blue fish and the slow red dog.\n"*300 +
                b"Different CASE mixed CaSe punctuation!? " * 100 +
                b"the zero the one the two the three the four the five the six the seven "*100)
        src, archive = self.p/"train-input", self.p/"trained.zxh"
        src.write_bytes(data)
        stats = pack(src, archive, words=64, native=BINARY, trained=True)
        self.assertGreater(stats["trained_context_entries"], 0)
        self.assertLessEqual(stats["trained_context_entries"], 65536)
        self.assertGreater(stats["predictions"]["trained_hits"], 0)
        self.assertGreater(stats["predictions"]["trained_misses"], 0)
        self.assertEqual(stats["predictions"]["trained_lookups"],
                         stats["predictions"]["trained_hits"]+stats["predictions"]["trained_misses"])
        out, _ = self.decode(archive)
        self.assertEqual(out.read_bytes(), data)
        wanted, parts = self.rawparts(archive)
        source = parts[0]
        match = re.search(rb"(static unsigned trained\[\d+\]\[10\]=\{\n)\{([^}]+)\}", source)
        self.assertIsNotNone(match)
        row = list(map(int, match[2].split(b",")))
        bad_sources = [
            re.sub(rb"#define CTRAIN \d+", b"#define CTRAIN 65537", source),
            re.sub(rb"#define N \d+", b"#define N 8449", source),
            source.replace(b"cdf[N+1]={0,", b"cdf[N+1]={1,", 1),
        ]
        for field, value in [(0, 0xffffffff), (1, 8448), (2, 100000001), (9, 100000001)]:
            changed = row.copy()
            changed[field] = value
            bad_sources.append(source[:match.start(2)]+
                               b",".join(str(v).encode() for v in changed)+source[match.end(2):])
        duplicate = row.copy()
        duplicate[3] = duplicate[1]
        bad_sources.append(source[:match.start(2)]+b",".join(str(v).encode() for v in duplicate)+
                           source[match.end(2):])
        for bad_source in bad_sources:
            self.decode(self.rawarchive(wanted, [bad_source, parts[1], parts[2]]), good=False)
        # Native pre-compilation raw-source and compressed-source budget checks.
        original = archive.read_bytes()
        for offset, value in [(8, 4194305), (12, 1048577)]:
            corrupt = bytearray(original)
            struct.pack_into("<I", corrupt, offset, value)
            (self.p/"oversize.zxh").write_bytes(corrupt)
            self.decode(self.p/"oversize.zxh", good=False)
        for edge in (b"", bytes(range(256))*5,
                     ("hello HELLO Hello hElLo Straße 日本語 \0\n"*100).encode()):
            src.write_bytes(edge)
            pack(src, archive, words=64, native=BINARY, trained=True)
            out, _ = self.decode(archive)
            self.assertEqual(out.read_bytes(), edge)

    def test_opt_in_alias_and_normal_d_rejection(self):
        src, archive, _ = self.archive(b"Hello hello HELLO " * 30)
        self.command("-x", archive, self.p/"new", good=False)
        self.assertFalse((self.p/"new").exists())
        self.command("-d", archive, self.p/"new", good=False)
        original = archive.read_bytes()
        self.command("-x", "--trust-generated-code", archive, archive, good=False)
        for kind in ("hard", "sym"):
            alias = self.p/kind
            if kind == "hard":
                os.link(archive, alias)
            else:
                alias.symlink_to(archive)
            self.command("-x", "--trust-generated-code", archive, alias, good=False)
            with self.assertRaises(ValueError):
                pack(archive, alias, native=BINARY)
        self.assertEqual(archive.read_bytes(), original)
        with self.assertRaises(ValueError):
            pack(src, src, native=BINARY)

    def test_structural_corruption_atomic_preservation(self):
        _, archive, _ = self.archive(b"Hello hello HELLO hElLo " * 50)
        original = archive.read_bytes()
        for mutated in (original[:10], original[:-1], original+b"x",
                        b"ZSH1"+original[4:],
                        original[:4]+struct.pack("<I", 100000001)+original[8:],
                        original[:16]+struct.pack("<I", 2)+original[20:]):
            bad = self.p/"bad.zxh"
            bad.write_bytes(mutated)
            self.decode(bad, good=False)
        wanted, parts = self.rawparts(archive)
        for index, replacement in [(0, b"this is not C"), (0, b"\0"),
                                   (0, b"int other(void){return 1;}"),
                                   (1, b"\xff"), (2, parts[2][:-8])]:
            changed = parts.copy()
            changed[index] = replacement
            self.decode(self.rawarchive(wanted, changed), good=False)

    def test_archived_code_is_authoritative_real_libtcc(self):
        # No host predictor or validation plan can produce this result.
        code = (b"int reconstruct(unsigned char*r,unsigned n,unsigned char*d,unsigned z,"
                b"unsigned char*out,unsigned wanted){unsigned i;"
                b"for(i=0;i<wanted;i++)out[i]=(unsigned char)(i*17);return 1;}")
        archive = self.rawarchive(100, [code, b"", b""])
        out, _ = self.decode(archive)
        self.assertEqual(out.read_bytes(), bytes((i*17)&255 for i in range(100)))

    def test_portable_build_and_original_modes(self):
        portable = self.p/"portable"
        self.command("-O2", "-std=c11", ROOT/"zsx.c", "-o", portable, binary=Path("cc"))
        _, archive, _ = self.archive(b"ordinary text " * 100)
        r = self.command("-x", "--trust-generated-code", archive, self.p/"unused",
                         good=False, binary=portable)
        self.assertIn(b"unsupported", r.stderr)
        for binary in (BINARY, portable):
            for mode in ([], ["-h"]):
                src = self.p/"ordinary"
                src.write_bytes(b"aabccddeeff abcde words Words \0"*100)
                self.command(*mode, src, self.p/"ordinary.zsx", binary=binary)
                self.command("-d", self.p/"ordinary.zsx", self.p/"ordinary.out", binary=binary)
                self.assertEqual(src.read_bytes(), (self.p/"ordinary.out").read_bytes())
            self.command("--help", binary=binary)
            self.command("-h", binary=binary)


if __name__ == "__main__":
    report_path = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ZXHTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    report = {"tests_run": result.testsRun, "failures": len(result.failures),
              "errors": len(result.errors), "passed": result.wasSuccessful(), "tests": RESULTS}
    if report_path:
        report_path.write_text(json.dumps(report, indent=2)+"\n")
    sys.exit(not result.wasSuccessful())