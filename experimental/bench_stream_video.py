#!/usr/bin/env python3
"""Benchmark the uploaded streaming model on real ZSXD coefficient packets.

No production decoder is replaced. Generates Linux-only, single-threaded
benchmark adapters in temporary storage; never executes the supplied EXE.
"""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
WRAPPER = r"""
#include <time.h>
int trial(const unsigned char *src, size_t n, double *stats) {
 byte *input=zsx_alloc(1,n+64), *decoded;
 memcpy(input,src,n); zsx_len(input)=n;
 zsx_high=0;
 zsx_memory_peak=zsx_memory_used;
 clock_t a=clock();
 if(!zsx_encode(input)) return 1;
 clock_t b=clock();
 size_t stored=zsx_len(input);
 decoded=zsx_decode(input);
 clock_t c=clock();
 if(!decoded || zsx_len(decoded)!=n || memcmp(src,decoded,n)) return 2;
 stats[0]=stored; stats[1]=(double)(b-a)/CLOCKS_PER_SEC;
 stats[2]=(double)(c-b)/CLOCKS_PER_SEC;
 stats[3]=(double)zsx_memory_peak;
 if(decoded!=input) zsx_del(decoded);
 zsx_del(input);
 if(zsx_memory_used) return 3;
 return 0;
}
"""


def run(command):
    return subprocess.check_output([str(x) for x in command])


def packets(path, limit):
    data = json.loads(run([
        ROOT / "ffmpeg-zsx/bin/ffprobe", "-v", "error",
        "-select_streams", "v:0", "-read_intervals", f"%+#{limit}",
        "-show_packets", "-show_data", "-of", "json", path]))
    result = []
    for packet in data["packets"]:
        raw = bytearray()
        for line in packet["data"].splitlines():
            if ":" in line:
                raw.extend(bytes.fromhex(line.split(":", 1)[1].split("  ", 1)[0]))
        if raw[:4] != b"ZSXD":
            raise ValueError("Expected native ZSXD video packets")
        result.append(bytes(raw))
    if not result:
        raise ValueError("No video packets")
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("zip", type=Path)
    ap.add_argument("videos", nargs="+", type=Path)
    ap.add_argument("--packets", type=int, default=30)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    if not 1 <= args.packets <= 1000:
        ap.error("--packets must be between 1 and 1000")
    with tempfile.TemporaryDirectory(prefix="zsx-video-model-") as td:
        work = Path(td)
        with zipfile.ZipFile(args.zip) as z:
            source = z.read("codecs/zsx_stream_candidates.c").decode()
            model = z.read("codecs/zsx_stream_candidates_model.h")
        # Only the codec, not Win32 filesystem/thread/archive entry points.
        source = source[source.index("typedef uint8_t byte;"):
                        source.index("int zsx_bytes_read;")]
        source = re.sub(
            r"static volatile LONG zsx_memory_guard;.*?"
            r"static void zsx_unlock_memory\(void\) \{.*?\}",
            "static void zsx_lock_memory(void) {}\n"
            "static void zsx_unlock_memory(void) {}", source, flags=re.S)
        if "Interlocked" in source or "Sleep(" in source:
            raise ValueError("Unexpected platform dependencies in codec slice")
        source = ("#include <stdint.h>\n#include <stdio.h>\n"
                  "#include <stdlib.h>\n#include <string.h>\n"
                  "#include <limits.h>\n" + source + WRAPPER)
        (work / "model.c").write_text(source)
        (work / "zsx_stream_candidates_model.h").write_bytes(model)
        models = {}
        variants = [("stream_byte", []), ("stream_candidates", ["-DZSX_CANDIDATES"])]
        variants += [(f"stream_candidates_mc{bits}", ["-DZSX_CANDIDATES", f"-Dzsx_mc={bits}"])
                     for bits in (16, 20)]
        for name, flags in variants:
            lib = work / (name + ".so")
            run(["cc", "-O2", "-fPIC", "-shared", *flags,
                 work / "model.c", "-o", lib])
            handle = C.CDLL(str(lib), mode=C.RTLD_LOCAL)
            handle.trial.argtypes = [C.c_void_p, C.c_size_t, C.POINTER(C.c_double)]
            handle.trial.restype = C.c_int
            models[name] = handle
        corepath = work / "production.so"
        run(["cc", "-O2", "-fPIC", "-shared", "-Dmain=unused_zsx_main", "-DZSX_LIBRARY",
             ROOT / "zsx.c", ROOT / "ffmpeg-zsx/src/zsx_core_bridge.c",
             ROOT / "ffmpeg-zsx/src/zsx_stream.c",
             "-o", corepath])
        core = C.CDLL(str(corepath), mode=C.RTLD_LOCAL)
        core.zsx_ffmpeg_decode.argtypes = [C.c_void_p, C.c_size_t,
                                           C.POINTER(C.c_void_p), C.POINTER(C.c_size_t)]
        core.zsx_ffmpeg_free.argtypes = [C.c_void_p]
        results = []
        for path in args.videos:
            chunks, stored, packet_count = [], 0, 0
            for packet in packets(path, args.packets):
                count, = struct.unpack_from("<I", packet, 32)
                offset = 36
                for _ in range(count):
                    wanted, length = struct.unpack_from("<II", packet, offset)
                    offset += 8
                    encoded = packet[offset:offset + length]
                    offset += length
                    decoded, size = C.c_void_p(), C.c_size_t()
                    if core.zsx_ffmpeg_decode(encoded, len(encoded),
                                              C.byref(decoded), C.byref(size)) != 0:
                        raise RuntimeError("Production packet decode failed")
                    try:
                        if size.value != wanted:
                            raise RuntimeError("Wrong production output size")
                        chunks.append(C.string_at(decoded, size.value))
                    finally:
                        core.zsx_ffmpeg_free(decoded)
                    stored += length
                if offset != len(packet):
                    raise ValueError("Trailing video packet data")
                packet_count += 1
            item = {"video": str(path), "packets": packet_count,
                    "coefficient_bytes": sum(map(len, chunks)),
                    "production_chunk_bytes": stored, "models": {}}
            for name, model in models.items():
                totals = [0.0, 0.0, 0.0]
                peak = 0
                for chunk in chunks:
                    stats = (C.c_double * 4)()
                    if model.trial(chunk, len(chunk), stats):
                        raise RuntimeError(f"{name}: round-trip or leak failure")
                    totals = [a + b for a, b in zip(totals, stats)]
                    peak = max(peak, int(stats[3]))
                item["models"][name] = dict(
                    chunk_bytes=int(totals[0]), encode_cpu_seconds=totals[1],
                    decode_cpu_seconds=totals[2], byte_exact=True,
                    peak_tracked_bytes_including_buffers=peak,
                    change_vs_production_percent=100 * (totals[0] / stored - 1))
            results.append(item)
            print(json.dumps(item), flush=True)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({
            "source_zip_sha256": hashlib.sha256(args.zip.read_bytes()).hexdigest(),
            "scope": "One-pass per existing frame chunk; identical coefficient bytes. "
                     "16-byte chunk headers included; unchanged packet/container overhead excluded. "
                     "CPU times include codec allocations; Linux O2, one run.",
            "results": results}, indent=2) + "\n")


if __name__ == "__main__":
    main()
