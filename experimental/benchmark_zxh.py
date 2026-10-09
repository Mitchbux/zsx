#!/usr/bin/env python3
"""Bounded candidate selection and independent full-corpus/native verification."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

from zxh_generate import ROOT, distinct, pack, sha


def check(command, cwd=None):
    start = time.monotonic()
    result = subprocess.run(list(map(str, command)), capture_output=True, cwd=cwd, timeout=290)
    record = {"argv": list(map(str, command)), "seconds": time.monotonic()-start,
              "returncode": result.returncode,
              "stdout": result.stdout.decode(errors="replace"),
              "stderr": result.stderr.decode(errors="replace")}
    if result.returncode:
        raise RuntimeError(json.dumps(record))
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=ROOT/"enwik8")
    parser.add_argument("--report", type=Path, default=ROOT/"experimental/zxh_enwik8.json")
    parser.add_argument("--refine-existing", action="store_true",
                        help="retain recorded original experiment; evaluate exactly one trained-context candidate")
    args = parser.parse_args()
    distinct(args.input, args.report)
    out = ROOT/"outputs/zxh-frequency"
    out.mkdir(parents=True, exist_ok=True)
    native = ROOT/"zsx"
    with args.input.open("rb") as f:
        (out/"prefix").write_bytes(f.read(1048576))
    report = {"format": "ZXH1", "inner_coder": "32-bit arithmetic prediction/correction",
              "outer_coder": "unchanged native ZSX1, or raw if smaller",
              "selection": "minimum COMPLETE 1MiB archive among three dictionary budgets",
              "baseline_full_bytes": 32858455, "previous_computed_full_bytes": 37278645,
              "candidates": [], "independent_tests": []}
    if args.refine_existing:
        report = json.loads(args.report.read_text())
        report.setdefault("previous_model_full", report["full"])
        report["independent_tests"] = []
    for words in (() if args.refine_existing else (512, 2048, 4096)):
        archive = out/f"candidate-{words}.zxh"
        result = pack(out/"prefix", archive, words=words, native=native)
        execution = check([native, "-x", "--trust-generated-code", archive, out/"prefix.out"])
        result["decode_seconds"] = execution["seconds"]
        result["output_sha256_external"] = sha(out/"prefix.out")
        result["external_sha256_equal"] = result["output_sha256_external"] == result["input_sha256_external"]
        assert result["external_sha256_equal"]
        report["candidates"].append(result)
    best = min(report["candidates"], key=lambda r: r["archive_bytes"])
    trained_prefix_archive = out/"trained-prefix.zxh"
    trained_prefix = pack(out/"prefix", trained_prefix_archive,
                          words=best["requested_words"], native=native, trained=True)
    execution = check([native, "-x", "--trust-generated-code", trained_prefix_archive,
                       out/"trained-prefix.out"])
    trained_prefix["decode_seconds"] = execution["seconds"]
    trained_prefix["output_sha256_external"] = sha(out/"trained-prefix.out")
    trained_prefix["external_sha256_equal"] = trained_prefix["output_sha256_external"] == trained_prefix["input_sha256_external"]
    assert trained_prefix["external_sha256_equal"]
    report["trained_refinement"] = {
        "configuration": "one fixed prior-two-token top-four model; max 65536 contexts, support >=64",
        "prefix": trained_prefix,
        "versus_original_prefix_bytes": trained_prefix["archive_bytes"]-best["archive_bytes"]}
    use_trained = trained_prefix["archive_bytes"] < best["archive_bytes"]
    report["selected_requested_words"] = best["requested_words"]
    archive, restored = out/"enwik8.zxh", out/"enwik8.out"
    candidate_archive = out/"trained-full.zxh" if use_trained else out/"candidate-full.zxh"
    if args.refine_existing and not use_trained:
        # No promising prefix result: don't rerun or replace the old best.
        candidate_archive = archive
        full = dict(report["previous_model_full"])
    else:
        full = pack(args.input, candidate_archive, words=best["requested_words"],
                    native=native, trained=use_trained)
    # Decode from an unrelated empty directory: no original/template paths in
    # the argv or cwd. strace independently records all file accesses.
    command = [native, "-x", "--trust-generated-code", candidate_archive, restored]
    trace = out/"decode-files.trace"
    if shutil.which("strace"):
        command = ["strace", "-f", "-e", "trace=%file", "-o", trace, *command]
    with tempfile.TemporaryDirectory(prefix="zxh-independent-decode-") as tmp:
        execution = check(command, cwd=tmp)
    full["decode_seconds"] = execution["seconds"]
    full["decode_returncode"] = execution["returncode"]
    full["output_sha256_external"] = sha(restored)
    full["external_sha256_equal"] = full["output_sha256_external"] == full["input_sha256_external"]
    assert full["external_sha256_equal"]
    if trace.exists():
        text = trace.read_text()
        opens = sorted(set(re.findall(r'open(?:at)?\([^"\n]*"([^"]+)"', text)))
        forbidden = [str(args.input.resolve()), str(ROOT/"experimental/zxh_codec.c"),
                     str(ROOT/"experimental/zxh_encoder.c")]
        violations = [p for p in opens if p in forbidden or p.endswith((".c", ".h", ".py"))]
        full["decode_file_access_check"] = {
            "tool": "strace -f -e trace=%file", "working_directory": "unrelated empty temp directory",
            "opened_paths": opens, "forbidden_opens": violations,
            "no_original_or_template_reads_observed": not violations}
        assert not violations
    else:
        full["decode_file_access_check"] = {"available": False, "reason": "strace unavailable"}
    full["versus_baseline_bytes"] = full["archive_bytes"]-report["baseline_full_bytes"]
    full["versus_previous_computed_bytes"] = full["archive_bytes"]-report["previous_computed_full_bytes"]
    full["tiny_residual_target_met"] = False
    if use_trained:
        report["trained_refinement"]["full"] = full
        reference = report.get("previous_model_full")
        if reference:
            full["versus_previous_model_bytes"] = full["archive_bytes"]-reference["archive_bytes"]
            if full["archive_bytes"] >= reference["archive_bytes"]:
                # Preserve the old best archive; never overwrite on a loss.
                report["trained_refinement"]["selected"] = False
                full = reference
                check([native, "-x", "--trust-generated-code", archive, restored])
            else:
                if not (out/"original-model-best.zxh").exists():
                    shutil.copyfile(archive, out/"original-model-best.zxh")
                shutil.copyfile(candidate_archive, archive)
                report["trained_refinement"]["selected"] = True
        else:
            shutil.copyfile(candidate_archive, archive)
            report["trained_refinement"]["selected"] = True
    else:
        if candidate_archive != archive:
            shutil.copyfile(candidate_archive, archive)
        report["trained_refinement"]["selected"] = False
    report["selected_model"] = "corpus-trained-contexts" if report["trained_refinement"]["selected"] else "adaptive-global"
    report["selection"] = "complete archive size; three original dictionary candidates plus ONE trained-context refinement"
    report["final_archive_path"] = str(archive.relative_to(ROOT))
    report["final_archive_sha256_external"] = sha(archive)
    report["full"] = full
    for command in [
        ["python3", ROOT/"experimental/test_zxh.py", out/"tests.json"],
        ["python3", ROOT/"test/zsx_high_cli.py", native],
        ["node", ROOT/"test/zsx_c_cross_compat.js", native],
        ["node", ROOT/"test/zsx_range_regression.js"],
        ["cc", "-O2", "-std=c11", ROOT/"zsx_high_test.c", "-o", out/"high-test"],
        [out/"high-test"],
    ]:
        report["independent_tests"].append(check(command, cwd=ROOT))
    report["native_test_summary"] = json.loads((out/"tests.json").read_text())
    report["all_tests_passed"] = True
    args.report.write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps({"archive_bytes": full["archive_bytes"],
                      "sections": full["sections"],
                      "source_budget_compliant": full["source_budget_compliant"],
                      "external_sha256_equal": full["external_sha256_equal"],
                      "report": str(args.report)}, indent=2))


if __name__ == "__main__":
    main()