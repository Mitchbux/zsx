#!/usr/bin/env python3
"""Small deterministic baseline/high ZSX benchmark with machine-readable output."""

from __future__ import annotations

import argparse
import csv
import io
import json
from pathlib import Path
import random
import re
import subprocess
import tempfile
import time


LIMIT = 1024 * 1024
HIGH_LOG = re.compile(
    rb"selected=(?P<selected>\w+).*?hits=(?P<hits>\d+).*?"
    rb"replacements=(?P<replacements>\d+).*?collisions=(?P<collisions>\d+)"
)


def corpora(root: Path) -> list[tuple[str, bytes]]:
    result: list[tuple[str, bytes]] = []
    enwik = root / "enwik8"
    if enwik.is_file():
        with enwik.open("rb") as source:
            result.append(("enwik8-first-1MiB", source.read(LIMIT)))
    result.extend(
        [
            ("repeat", (b"abc123 -- repeatable payload --\n" * 40_000)[:LIMIT]),
            (
                "english",
                (
                    b"The archive stores a sequence of independently coded chunks. "
                    b"Common words and punctuation make this realistic prose input. "
                )
                * 12_000,
            ),
            (
                "code",
                (
                    b"static int decode_chunk(const unsigned char *input, size_t length) {\n"
                    b"    if (input == NULL || length == 0) return ERROR_INVALID;\n"
                    b"    return write_output(input, length);\n}\n"
                )
                * 7_000,
            ),
            ("random", random.Random(0x5A58).randbytes(LIMIT)),
        ]
    )
    return [(name, data[:LIMIT]) for name, data in result]


def invoke(command: list[str]) -> tuple[subprocess.CompletedProcess[bytes], float]:
    started = time.perf_counter()
    completed = subprocess.run(
        command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120
    )
    elapsed = time.perf_counter() - started
    if completed.returncode:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {' '.join(command)}\n"
            f"{completed.stderr.decode('utf-8', 'replace')}"
        )
    return completed, elapsed


def benchmark(binary: Path, root: Path) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    with tempfile.TemporaryDirectory(prefix="zsx-high-benchmark-") as temporary:
        work = Path(temporary)
        for index, (name, payload) in enumerate(corpora(root)):
            source = work / f"{index}.bin"
            baseline = work / f"{index}.base.zsx"
            high = work / f"{index}.high.zsx"
            restored = work / f"{index}.restored"
            source.write_bytes(payload)
            _, baseline_seconds = invoke([str(binary), str(source), str(baseline)])
            high_run, high_seconds = invoke(
                [str(binary), "-h", str(source), str(high)]
            )
            _, decode_seconds = invoke([str(binary), "-d", str(high), str(restored)])
            if restored.read_bytes() != payload:
                raise RuntimeError(f"roundtrip mismatch for {name}")

            matches = list(HIGH_LOG.finditer(high_run.stderr))
            rows.append(
                {
                    "corpus": name,
                    "source_bytes": len(payload),
                    "baseline_bytes": baseline.stat().st_size,
                    "high_bytes": high.stat().st_size,
                    "magic": high.read_bytes()[:4].decode("ascii", "replace"),
                    "hash_chunks": sum(
                        match["selected"] == b"hash" for match in matches
                    ),
                    "hash_hits": sum(int(match["hits"]) for match in matches),
                    "hash_replacements": sum(
                        int(match["replacements"]) for match in matches
                    ),
                    "hash_collisions": sum(
                        int(match["collisions"]) for match in matches
                    ),
                    "baseline_seconds": round(baseline_seconds, 6),
                    "high_seconds": round(high_seconds, 6),
                    "decode_seconds": round(decode_seconds, 6),
                    "roundtrip": True,
                }
            )
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", nargs="?", default="./zsx")
    parser.add_argument("--format", choices=("json", "csv"), default="json")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    rows = benchmark(Path(args.binary).resolve(), root)
    totals = {
        "corpus": "TOTAL",
        "source_bytes": sum(int(row["source_bytes"]) for row in rows),
        "baseline_bytes": sum(int(row["baseline_bytes"]) for row in rows),
        "high_bytes": sum(int(row["high_bytes"]) for row in rows),
        "magic": "",
        "hash_chunks": sum(int(row["hash_chunks"]) for row in rows),
        "hash_hits": sum(int(row["hash_hits"]) for row in rows),
        "hash_replacements": sum(int(row["hash_replacements"]) for row in rows),
        "hash_collisions": sum(int(row["hash_collisions"]) for row in rows),
        "baseline_seconds": round(
            sum(float(row["baseline_seconds"]) for row in rows), 6
        ),
        "high_seconds": round(sum(float(row["high_seconds"]) for row in rows), 6),
        "decode_seconds": round(
            sum(float(row["decode_seconds"]) for row in rows), 6
        ),
        "roundtrip": all(bool(row["roundtrip"]) for row in rows),
    }
    output_rows = [*rows, totals]
    if args.format == "json":
        print(json.dumps(output_rows, indent=2))
    else:
        stream = io.StringIO()
        writer = csv.DictWriter(stream, fieldnames=list(totals))
        writer.writeheader()
        writer.writerows(output_rows)
        print(stream.getvalue(), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())