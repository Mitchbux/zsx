#!/usr/bin/env python3
"""Verify uploaded-model parity and complete-chunk selection on real packets."""
import ctypes as C
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "experimental"))
from bench_stream_video import packets

def build(args):
    subprocess.run([str(x) for x in args], check=True)

def setup(path):
    lib = C.CDLL(str(path))
    for name in ("zsx_ffmpeg_encode", "zsx_ffmpeg_encode_best", "zsx_ffmpeg_decode",
                 "zsx_ffmpeg_decode_profile", "zsx_stream_decode"):
        fn = getattr(lib, name)
        fn.argtypes = [C.c_void_p, C.c_size_t, C.POINTER(C.c_void_p), C.POINTER(C.c_size_t)]
    lib.zsx_stream_encode.argtypes = [C.c_void_p, C.c_size_t, C.c_uint,
                                    C.POINTER(C.c_void_p), C.POINTER(C.c_size_t)]
    lib.zsx_ffmpeg_free.argtypes = [C.c_void_p]
    return lib

with tempfile.TemporaryDirectory() as td:
    work = Path(td)
    build(["cc", "-O2", "-shared", "-fPIC", "-DZSX_LIBRARY", "-Dmain=zsx_cli_main",
           ROOT/"zsx.c", ROOT/"ffmpeg-zsx/src/zsx_core_bridge.c",
           ROOT/"ffmpeg-zsx/src/zsx_stream.c", "-o", work/"core.so"])
    lib = setup(work/"core.so")
    libc = C.CDLL(None)
    libc.free.argtypes = [C.c_void_p]
    def call(fn, data, profile=None):
        p, n = C.c_void_p(), C.c_size_t()
        args = [data, len(data)] + ([] if profile is None else [profile])
        assert fn(*args, C.byref(p), C.byref(n)) == 0
        result = C.string_at(p, n.value)
        (libc.free if fn in (lib.zsx_stream_encode, lib.zsx_stream_decode) else lib.zsx_ffmpeg_free)(p)
        return result
    with zipfile.ZipFile(ROOT/"attached_assets/zsx-stream-candidates-windows_1791563114539.zip") as z:
        src = z.read("codecs/zsx_stream_candidates.c").decode()
        src = src[src.index("typedef uint8_t byte;"):src.index("int zsx_bytes_read;")]
        src = re.sub(r"static volatile LONG zsx_memory_guard;.*?static void zsx_unlock_memory\(void\) \{.*?\}",
                     "static void zsx_lock_memory(void) {}\nstatic void zsx_unlock_memory(void) {}", src, flags=re.S)
        (work/"zsx_stream_candidates_model.h").write_bytes(z.read("codecs/zsx_stream_candidates_model.h"))
    src = "#include <stdint.h>\n#include <stdlib.h>\n#include <stdio.h>\n#include <string.h>\n#include <limits.h>\n" + src
    src += """
int oracle(const byte *input, size_t n, byte *out) {
byte *p=zsx_alloc(1,n+64); memcpy(p,input,n); zsx_len(p)=n;
zsx_high=0; zsx_encode(p); int size=zsx_len(p); memcpy(out,p,size);
zsx_del(p); return size;
}
"""
    (work/"oracle.c").write_text(src)
    oracles = {}
    for profile in range(1,5):
        flags = ["-Dzsx_mc="+str(20 if profile in (2,4) else 16)]
        if profile>=3: flags += ["-DZSX_CANDIDATES"]
        build(["cc","-O2","-shared","-fPIC",*flags,work/"oracle.c","-o",work/f"oracle{profile}.so"])
        oracle = C.CDLL(str(work/f"oracle{profile}.so")).oracle
        oracle.argtypes = [C.c_void_p,C.c_size_t,C.c_void_p]
        oracles[profile] = oracle
    results=[]
    for path in sys.argv[1:]:
        baseline_total=best_total=0
        selected={}
        frames=packets(Path(path),150)
        for packet in frames:
            at=36
            baseline_total += 36
            best_total += 36
            for _ in range(struct.unpack_from("<I",packet,32)[0]):
                wanted, n = struct.unpack_from("<II",packet,at); at+=8
                raw=call(lib.zsx_ffmpeg_decode,packet[at:at+n]); at+=n
                assert len(raw)==wanted
                baseline=call(lib.zsx_ffmpeg_encode,raw)
                choices=[baseline]
                for profile, oracle in oracles.items():
                    encoded=call(lib.zsx_stream_encode,raw,profile)
                    assert call(lib.zsx_stream_decode,encoded)==raw
                    buf=C.create_string_buffer(len(raw)+64)
                    size=oracle(raw,len(raw),buf)
                    assert encoded[8:]==buf.raw[:size], profile
                    if profile in (1,3): choices.append(encoded)
                best=call(lib.zsx_ffmpeg_encode_best,raw)
                assert len(best)==min(map(len,choices))
                assert call(lib.zsx_ffmpeg_decode_profile,best)==raw
                profile=struct.unpack_from("<I",best,4)[0] if best[:4]==b"ZSE1" else 0
                selected[profile]=selected.get(profile,0)+1
                baseline_total+=8+len(baseline);best_total+=8+len(best)
        results.append(dict(file=path,frames=len(frames),baseline_packet_bytes=baseline_total,
                            selected_packet_bytes=best_total,profiles=selected,oracle_parity=True))
    print(json.dumps(results,indent=2))
