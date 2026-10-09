#!/usr/bin/env python3
"""Generate ZXH1: global word/byte frequencies, small dictionary, ONE residual."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
WORD = re.compile(rb"[A-Za-z]+")
COMMANDS = []


def run(args):
    start = time.monotonic()
    p = subprocess.run(list(map(str, args)), capture_output=True, timeout=290)
    COMMANDS.append({"argv": list(map(str, args)), "seconds": time.monotonic()-start,
                     "returncode": p.returncode})
    if p.returncode:
        raise RuntimeError(f"{args}: {p.stderr.decode(errors='replace')}")
    return p.stdout


def distinct(src, dst):
    if Path(src).resolve() == Path(dst).resolve() or (
            Path(src).exists() and Path(dst).exists() and os.path.samefile(src, dst)):
        raise ValueError("input/output alias rejected")


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for b in iter(lambda: f.read(1048576), b""):
            h.update(b)
    return h.hexdigest()


def pack(src, dst, words=2048, budget=32768, native=ROOT/"zsx", order=3, trained=False):
    distinct(src, dst)
    if Path(src).stat().st_size > 100000000:
        raise ValueError("experimental input bound is 100,000,000 bytes")
    if not 0 <= words <= 8192 or not 0 <= budget <= 65536:
        raise ValueError("dictionary bounds: 0..8192 words, 0..65536 bytes")
    if order not in (1, 2, 3):
        raise ValueError("context order must be 1, 2, or 3")
    started = time.monotonic()
    data = Path(src).read_bytes()
    counts = Counter(m.group().lower() for m in WORD.finditer(data))
    ranked = sorted((w for w, n in counts.items() if n >= 2 and len(w) <= 255),
                    key=lambda w: (-counts[w]*(len(w)+0.5), w))
    chosen, used = [], 0
    for w in ranked:
        if len(chosen) == words:
            break
        if used + len(w)+1 <= budget:
            chosen.append(w)
            used += len(w)+1
    selected = set(chosen)
    escaped = WORD.sub(lambda m: b"" if m.group().lower() in selected else m.group(), data)
    bytecounts = Counter(escaped)
    freq = [bytecounts[i] for i in range(256)] + [counts[w] for w in chosen]
    total = sum(freq)
    freq = [1 + (v * 1000000 // max(1, total)) for v in freq]
    cumulative = [0]
    for v in freq:
        cumulative.append(cumulative[-1]+v)
    top = max(range(len(freq)), key=freq.__getitem__)
    model = (f"#define N {len(freq)}\n#define TOP {top}\n#define ORDER {order}\n"
             "static unsigned cdf[N+1]={" + ",".join(map(str, cumulative)) + "};\n").encode()
    training_model = b"#define CTRAIN 0\nstatic unsigned trained[1][10]={{0}};\n"
    algorithm = (ROOT/"experimental/zxh_codec.c").read_bytes()
    source = model + training_model + algorithm
    dictionary = b"".join(bytes([len(w)])+w for w in chosen)
    del escaped, counts, ranked
    timings = {"model_training_seconds": time.monotonic()-started}
    with tempfile.TemporaryDirectory(prefix="zxh-generate-") as tmp:
        work = Path(tmp)
        (work/"source").write_bytes(source)
        (work/"dict").write_bytes(dictionary)
        (work/"encoder.c").write_bytes(source + (ROOT/"experimental/zxh_encoder.c").read_bytes())
        run(["cc", "-O2", "-std=c11", work/"encoder.c", "-o", work/"encoder"])
        trained_entries = 0
        if trained:
            train_start = time.monotonic()
            run([work/"encoder", Path(src).resolve(), work/"dict", work/"trained.c", "--train"])
            training_model = (work/"trained.c").read_bytes()
            trained_entries = int(re.search(rb"#define CTRAIN (\d+)", training_model)[1])
            source = model + training_model + algorithm
            if len(source) > 4194304:
                raise ValueError("generated source exceeds native 4 MiB raw-source bound")
            (work/"source").write_bytes(source)
            (work/"encoder.c").write_bytes(source + (ROOT/"experimental/zxh_encoder.c").read_bytes())
            run(["cc", "-O2", "-std=c11", work/"encoder.c", "-o", work/"encoder"])
            timings["context_training_and_compilation_seconds"] = time.monotonic()-train_start
        encode_start = time.monotonic()
        predictions = json.loads(run([work/"encoder", Path(src).resolve(),
                                      work/"dict", work/"residual"]))
        timings["residual_encoding_seconds"] = time.monotonic()-encode_start
        # The actual outer ZSX coder is unchanged. Arithmetic residual often
        # remains raw: never retain an outer encoding that increases bytes.
        sections, stats = [], {}
        for name in ("source", "dict", "residual"):
            raw = (work/name).read_bytes()
            run([native, work/name, work/(name+".zsx")])
            coded = (work/(name+".zsx")).read_bytes()
            mode = int(len(coded) < len(raw))
            stored = coded if mode else raw
            sections.append((len(raw), stored, mode))
            stats[name] = {"raw_bytes": len(raw), "stored_bytes": len(stored),
                           "outer_zsx_candidate_bytes": len(coded),
                           "storage": "ZSX1" if mode else "raw"}
        if len(sections[0][1]) > 1048576:
            raise ValueError("compressed source/model exceeds 1 MiB budget")
        header = b"ZXH1"+struct.pack("<I", len(data))
        header += b"".join(struct.pack("<III", n, len(s), m) for n, s, m in sections)
        archive = header+b"".join(s for _, s, _ in sections)
        # Atomic replacement; no stored hashes, checksums, or duplicate plans.
        fd, temporary = tempfile.mkstemp(prefix=Path(dst).name+".tmp-", dir=Path(dst).parent)
        try:
            with os.fdopen(fd, "wb") as f:
                f.write(archive)
            os.replace(temporary, dst)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
    predictions["overall_hit_rate"] = predictions["prediction_hits"]/max(1, predictions["events"])
    predictions["word_hit_rate"] = predictions["word_hits"]/max(1, predictions["word_events"])
    predictions["letter_hit_rate"] = predictions["letter_hits"]/max(1, predictions["letter_events"])
    return {"input_bytes": len(data), "archive_bytes": len(archive), "framing_bytes": 44,
            "dictionary_words": len(chosen), "dictionary_budget_bytes": budget,
            "requested_words": words, "context_order": order,
            "trained_context_model": trained, "trained_context_entries": trained_entries,
            "trained_context_entry_budget": 65536, "trained_context_minimum_support": 64,
            "trained_context_source_bytes": len(training_model),
            "model_table_source_bytes": len(model)+len(training_model),
            "algorithm_source_bytes": len(algorithm),
            "embedded_model_binary_equivalent_bytes": 4*len(cumulative)+40*max(1, trained_entries),
            "source_budget_bytes": 1048576, "source_budget_compliant": True,
            "sections": stats, "predictions": predictions, "timings": timings,
            "total_generation_seconds": time.monotonic()-started,
            "input_sha256_external": hashlib.sha256(data).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--words", type=int, default=2048)
    parser.add_argument("--dictionary-budget", type=int, default=32768)
    parser.add_argument("--order", type=int, choices=(1, 2, 3), default=3)
    parser.add_argument("--trained-contexts", action="store_true",
                        help="train at most 65536 prior-two-token top-four context rows (minimum support 64)")
    parser.add_argument("--native", type=Path, default=ROOT/"zsx")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if args.report:
        distinct(args.input, args.report)
        distinct(args.output, args.report)
    report = pack(args.input, args.output, args.words, args.dictionary_budget,
                  args.native.resolve(), args.order, args.trained_contexts)
    report["commands"] = COMMANDS
    if args.report:
        args.report.write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()