#!/usr/bin/env python3
"""Package an already-built portable FFmpeg using an explicit public allowlist."""
import hashlib
from pathlib import Path
import re
import shutil
import tarfile

ROOT = Path(__file__).resolve().parents[2]
PACKAGE = ROOT / "generated-artifacts/zsv-ffmpeg-linux-x86_64"
UPSTREAM = Path("/tmp/ffmpeg-6.1.2.tar.xz")
UPSTREAM_SHA = "3b624649725ecdc565c903ca6643d41f33bd49239922e45c9b1442c63dca4e38"
ZIG = Path(shutil.which("zig")).resolve().parent.parent


def copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def main():
    assert hashlib.sha256(UPSTREAM.read_bytes()).hexdigest() == UPSTREAM_SHA
    for name in ("ffmpeg", "ffprobe"):
        assert (PACKAGE / "bin" / name).is_file(), "Build portable executables first"
    copy(ROOT / "ffmpeg-zsx/LINUX_PACKAGE_README.md", PACKAGE / "README.md")
    copy(ROOT / "ffmpeg-zsx/scripts/transcode-mp4.sh", PACKAGE / "transcode-mp4.sh")
    (PACKAGE / "transcode-mp4.sh").chmod(0o755)
    copy(UPSTREAM, PACKAGE / "source/ffmpeg-6.1.2.tar.xz")
    copy(ROOT / "zsx.c", PACKAGE / "source/zsx.c")
    for name in ("src", "patches"):
        shutil.copytree(ROOT / "ffmpeg-zsx" / name,
                        PACKAGE / "source/ffmpeg-zsx" / name, dirs_exist_ok=True)
    copy(ROOT / "ffmpeg-zsx/scripts/build.sh", PACKAGE / "source/ffmpeg-zsx/scripts/build.sh")
    with tarfile.open(UPSTREAM) as upstream:
        license_bytes = upstream.extractfile("ffmpeg-6.1.2/COPYING.LGPLv2.1").read()
    (PACKAGE / "licenses").mkdir(exist_ok=True)
    (PACKAGE / "licenses/FFMPEG-LGPL-2.1.txt").write_bytes(license_bytes)
    copy(ZIG / "lib/zig/libc/musl/COPYRIGHT", PACKAGE / "licenses/MUSL-COPYRIGHT.txt")
    # Deliberately no workspace-wide glob: private embedded-key builds must
    # never enter public archives. Check selected content without printing it.
    suspicious = re.compile(rb"SHORT_IO_API_KEY|sk_live_[A-Za-z0-9]+|ghp_[A-Za-z0-9]+|"
                            rb"Bearer [A-Za-z0-9_.-]{24,}|pex\.s\.gy|outputs/private-zsd")
    for path in PACKAGE.rglob("*"):
        if path.is_file() and path.name != "ffmpeg-6.1.2.tar.xz":
            if suspicious.search(path.read_bytes()):
                raise RuntimeError(f"Public package credential check failed: {path.relative_to(PACKAGE)}")
    files = sorted(p for p in PACKAGE.rglob("*") if p.is_file() and p.name != "SHA256SUMS")
    (PACKAGE / "SHA256SUMS").write_text("".join(
        f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(PACKAGE)}\n"
        for p in files))
    archive = PACKAGE.with_suffix(".tar.gz")
    with tarfile.open(archive, "w:gz") as output:
        output.add(PACKAGE, arcname=PACKAGE.name)
    with tarfile.open(archive) as result:
        for entry in result.getmembers():
            if entry.isfile() and not entry.name.endswith("/ffmpeg-6.1.2.tar.xz"):
                if suspicious.search(result.extractfile(entry).read()):
                    raise RuntimeError(f"Archive credential check failed: {entry.name}")
    print(f"Public package checks passed: {archive.relative_to(ROOT)} ({archive.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
