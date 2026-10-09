#!/usr/bin/env python3
"""Build a small, deterministic distribution archive for the Windows player.

This is intentionally a packager, not a builder.  It refuses to make an
archive until the already-built PE files, sample, pinned vendor archives, and
all corresponding source inputs are present and verified.  The output is
written with a temporary file and atomically renamed only after the complete
payload has been checked.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tarfile
import tempfile
import zipfile


FFMPEG_VERSION = "6.1.2"
FFMPEG_SHA256 = "3b624649725ecdc565c903ca6643d41f33bd49239922e45c9b1442c63dca4e38"
SDL_ARCHIVE_NAME = "SDL2-devel-2.30.11-mingw.zip"
SDL_SHA256 = "6011545c022e5aac38077e4b2adf9ebef737aca6fbb645050b5b182f44bae581"
PACKAGE_FOLDER = "zsv-player-windows-x64"
ZIP_NAME = "zsv-player-windows-x64.zip"
METADATA_NAME = "zsv-player-windows-x64.json"


class PackageError(RuntimeError):
    """An expected input or package invariant is missing."""


def fail(message: str) -> "NoReturn":
    raise PackageError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require_regular(path: Path, description: str) -> Path:
    if path.is_symlink() or not path.is_file():
        fail(f"missing {description}: {path}")
    return path


def verify_digest(path: Path, expected: str, description: str) -> None:
    actual = sha256(path)
    if actual != expected:
        fail(f"{description} SHA-256 mismatch: expected {expected}, got {actual}")


def verify_x64_pe(path: Path, description: str) -> None:
    """Check both the DOS and PE signatures and the x86_64 machine type."""
    require_regular(path, description)
    with path.open("rb") as stream:
        header = stream.read(4096)
    if len(header) < 64 or header[:2] != b"MZ":
        fail(f"{description} is not a PE file with an MZ header: {path}")
    pe_offset = struct.unpack_from("<I", header, 0x3C)[0]
    if pe_offset + 6 > len(header) or header[pe_offset : pe_offset + 4] != b"PE\0\0":
        fail(f"{description} has no PE signature: {path}")
    machine = struct.unpack_from("<H", header, pe_offset + 4)[0]
    if machine != 0x8664:
        fail(f"{description} is not x86_64 PE (machine 0x{machine:04x}): {path}")


def copy_checked(source: Path, destination: Path, description: str) -> None:
    require_regular(source, description)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    # A source file may carry useful executable bits (notably the shell
    # recipes).  Preserve them without carrying timestamps into the archive.
    destination.chmod(source.stat().st_mode & 0o777)


def copy_tree_files(source_root: Path, destination_root: Path, description: str) -> int:
    """Copy all regular files below a source tree, with no symlinks allowed."""
    require_regular(source_root, description) if source_root.is_file() else None
    if not source_root.is_dir() or source_root.is_symlink():
        fail(f"missing {description} directory: {source_root}")
    count = 0
    for source in sorted(source_root.rglob("*")):
        relative_parts = source.relative_to(source_root).parts
        # A previous local Python run or editor must not turn a source offer
        # into a cache/PDB/private-build-data offer.
        if any(part in {"__pycache__", ".cache"} for part in relative_parts):
            continue
        if source.suffix.lower() in {".pyc", ".pyo", ".pdb"}:
            continue
        if source.is_dir():
            if source.is_symlink():
                fail(f"symlink in {description}: {source}")
            continue
        if source.is_symlink() or not source.is_file():
            fail(f"non-regular file in {description}: {source}")
        relative = source.relative_to(source_root)
        copy_checked(source, destination_root / relative, f"{description} file")
        count += 1
    return count


def archive_member_bytes(archive: Path, member_suffix: str, description: str) -> bytes:
    """Read one exact top-level release notice without extracting an archive."""
    try:
        with tarfile.open(archive, mode="r:*") as tar:
            names = [
                member.name
                for member in tar.getmembers()
                if member.isfile() and member.name.endswith("/" + member_suffix)
            ]
            if len(names) != 1:
                fail(f"{description} not found uniquely in {archive}")
            extracted = tar.extractfile(names[0])
            if extracted is None:
                fail(f"cannot read {description} from {archive}")
            return extracted.read()
    except (tarfile.TarError, OSError) as exc:
        fail(f"cannot read FFmpeg archive {archive}: {exc}")


def sdl_member_bytes(archive: Path, member_suffix: str) -> bytes:
    try:
        with zipfile.ZipFile(archive) as zipped:
            names = [
                name
                for name in zipped.namelist()
                if not name.endswith("/") and name.endswith("/" + member_suffix)
            ]
            if len(names) != 1:
                fail(f"SDL {member_suffix} not found uniquely in {archive}")
            return zipped.read(names[0])
    except (zipfile.BadZipFile, OSError) as exc:
        fail(f"cannot read SDL archive {archive}: {exc}")


def write_bytes(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    path.chmod(0o644)


def payload_records(stage: Path) -> list[dict[str, object]]:
    records: list[dict[str, object]] = []
    for path in sorted(p for p in stage.rglob("*") if p.is_file()):
        relative = path.relative_to(stage).as_posix()
        records.append(
            {
                "path": relative,
                "size_bytes": path.stat().st_size,
                "sha256": sha256(path),
            }
        )
    return records


def write_sha256sums(stage: Path) -> None:
    lines = []
    for record in payload_records(stage):
        path = str(record["path"])
        if path == "SHA256SUMS":
            continue
        lines.append(f'{record["sha256"]}  {path}')
    write_bytes(stage / "SHA256SUMS", ("\n".join(lines) + "\n").encode("utf-8"))


def make_zip(stage: Path, temporary_zip: Path) -> None:
    """Write a reproducible archive whose entries all have one fixed time."""
    prefix = PACKAGE_FOLDER + "/"
    with zipfile.ZipFile(
        temporary_zip,
        mode="w",
        compression=zipfile.ZIP_DEFLATED,
        compresslevel=9,
        strict_timestamps=True,
    ) as archive:
        for path in sorted(p for p in stage.rglob("*") if p.is_file()):
            relative = path.relative_to(stage).as_posix()
            info = zipfile.ZipInfo(prefix + relative, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = (path.stat().st_mode & 0o777) << 16
            archive.writestr(info, path.read_bytes())


def validate_zip(path: Path, expected_paths: set[str]) -> None:
    try:
        with zipfile.ZipFile(path) as archive:
            if archive.testzip() is not None:
                fail(f"CRC failure in generated ZIP: {path}")
            actual = set(archive.namelist())
            expected = {PACKAGE_FOLDER + "/" + item for item in expected_paths}
            if actual != expected:
                missing = sorted(expected - actual)
                extra = sorted(actual - expected)
                fail(f"generated ZIP entries differ (missing={missing}, extra={extra})")
    except zipfile.BadZipFile as exc:
        fail(f"generated ZIP is invalid: {exc}")


def atomic_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix="." + path.name + ".", suffix=".tmp", dir=str(path.parent)
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream, indent=2, sort_keys=True)
            stream.write("\n")
        os.replace(temporary_name, path)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def parse_args() -> argparse.Namespace:
    script_root = Path(__file__).resolve().parents[1]
    repository_root = script_root.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=repository_root)
    parser.add_argument(
        "--ffmpeg-archive",
        type=Path,
        default=Path(os.environ.get("FFMPEG_ARCHIVE", f"/tmp/ffmpeg-{FFMPEG_VERSION}.tar.xz")),
        help="verified pristine FFmpeg archive (default: /tmp/ffmpeg-6.1.2.tar.xz)",
    )
    parser.add_argument(
        "--sdl-archive",
        type=Path,
        default=None,
        help="SDL SDK archive; defaults to temporarybuildtree/windows-zsv-player/downloads",
    )
    parser.add_argument("--output-dir", type=Path, default=None)
    return parser.parse_args()


def package(arguments: argparse.Namespace) -> tuple[Path, Path]:
    root = arguments.root.resolve()
    ffmpeg_zsx = root / "ffmpeg-zsx"
    player_dir = root / "outputs" / "windows-zsv-player"
    sample = root / "outputs" / "zsv-player-tests" / "sample.zsv"
    output_dir = (arguments.output_dir or root / "outputs").resolve()
    ffmpeg_archive = arguments.ffmpeg_archive.expanduser().resolve()
    sdl_archive = (
        arguments.sdl_archive.expanduser().resolve()
        if arguments.sdl_archive is not None
        else root
        / "temporarybuildtree"
        / "windows-zsv-player"
        / "downloads"
        / SDL_ARCHIVE_NAME
    )

    verify_digest(
        require_regular(ffmpeg_archive, "pristine FFmpeg 6.1.2 archive"),
        FFMPEG_SHA256,
        "FFmpeg 6.1.2 archive",
    )
    verify_digest(
        require_regular(sdl_archive, "SDL2 MinGW SDK archive"),
        SDL_SHA256,
        "SDL2 MinGW SDK archive",
    )
    verify_x64_pe(player_dir / "zsvplay.exe", "zsvplay.exe")
    verify_x64_pe(player_dir / "SDL2.dll", "SDL2.dll")
    require_regular(sample, "ZSV2 sample")

    package_zip = output_dir / ZIP_NAME
    package_json = output_dir / METADATA_NAME
    output_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix=".zsv-player-package-", dir=output_dir) as temporary:
        stage = Path(temporary) / PACKAGE_FOLDER
        # Runtime payload is deliberately limited to the two actual binaries
        # and one checked-in sample; old test reports and build caches never
        # enter the package.
        copy_checked(player_dir / "zsvplay.exe", stage / "zsvplay.exe", "zsvplay.exe")
        copy_checked(player_dir / "SDL2.dll", stage / "SDL2.dll", "SDL2.dll")
        copy_checked(sample, stage / "sample.zsv", "ZSV2 sample")

        readme = ffmpeg_zsx / "PLAYER_PACKAGE_README.txt"
        copy_checked(readme, stage / "README.txt", "player package README")

        # Notices come directly from the pinned vendor archives, rather than
        # from a potentially modified extracted build tree.
        write_bytes(
            stage / "COPYING.LGPLv2.1",
            archive_member_bytes(ffmpeg_archive, "COPYING.LGPLv2.1", "FFmpeg LGPL notice"),
        )
        write_bytes(
            stage / "FFMPEG-LICENSE.md",
            archive_member_bytes(ffmpeg_archive, "LICENSE.md", "FFmpeg license notice"),
        )
        write_bytes(
            stage / "SDL2-LICENSE.txt",
            sdl_member_bytes(sdl_archive, "LICENSE.txt"),
        )

        # The corresponding source includes every integration input used by
        # the build recipe, but not build products, caches, fixtures, or a
        # duplicate modified FFmpeg tree.
        copy_checked(root / "zsx.c", stage / "source" / "zsx.c", "production ZSX source")
        source_ffmpeg_zsx = stage / "source" / "ffmpeg-zsx"
        for directory_name in ("src", "patches", "scripts"):
            copied = copy_tree_files(
                ffmpeg_zsx / directory_name,
                source_ffmpeg_zsx / directory_name,
                f"ffmpeg-zsx/{directory_name}",
            )
            if copied == 0:
                fail(f"ffmpeg-zsx/{directory_name} contains no source files")
        for document in ("Makefile", "README.md", "WINDOWS_PLAYER.md", "PLAYER_PACKAGE_README.txt"):
            copy_checked(ffmpeg_zsx / document, source_ffmpeg_zsx / document, f"ffmpeg-zsx/{document}")
        docs = ffmpeg_zsx / "docs"
        if docs.exists():
            copy_tree_files(docs, source_ffmpeg_zsx / "docs", "ffmpeg-zsx/docs")

        copy_checked(
            ffmpeg_archive,
            stage / "source" / f"ffmpeg-{FFMPEG_VERSION}.tar.xz",
            "pristine FFmpeg archive",
        )
        copy_checked(
            sdl_archive,
            stage / "source" / "temporarybuildtree" / "windows-zsv-player" / "downloads" / SDL_ARCHIVE_NAME,
            "SDL SDK archive",
        )

        # The manifest describes all files except the two self-referential
        # checksum files.  SHA256SUMS then covers the manifest as well.
        records = payload_records(stage)
        manifest_records = [
            record
            for record in records
            if record["path"] not in {"package-metadata.json", "SHA256SUMS"}
        ]
        metadata = {
            "package": PACKAGE_FOLDER,
            "format": "standalone Windows x86_64 player",
            "ffmpeg_version": FFMPEG_VERSION,
            "ffmpeg_archive_sha256": FFMPEG_SHA256,
            "sdl_archive": SDL_ARCHIVE_NAME,
            "sdl_archive_sha256": SDL_SHA256,
            "payload": manifest_records,
        }
        write_bytes(
            stage / "package-metadata.json",
            (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode("utf-8"),
        )
        write_sha256sums(stage)

        expected_paths = {
            path.relative_to(stage).as_posix()
            for path in stage.rglob("*")
            if path.is_file()
        }
        temporary_zip = output_dir / f".{ZIP_NAME}.tmp"
        try:
            if temporary_zip.exists():
                temporary_zip.unlink()
            make_zip(stage, temporary_zip)
            validate_zip(temporary_zip, expected_paths)
            os.replace(temporary_zip, package_zip)
        except BaseException:
            try:
                temporary_zip.unlink()
            except FileNotFoundError:
                pass
            raise

    with zipfile.ZipFile(package_zip) as archive:
        zip_records = []
        for info in sorted(archive.infolist(), key=lambda item: item.filename):
            zip_records.append(
                {
                    "path": info.filename,
                    "size_bytes": info.file_size,
                    "sha256": hashlib.sha256(archive.read(info)).hexdigest(),
                }
            )
    atomic_json(
        package_json,
        {
            "package": PACKAGE_FOLDER,
            "zip": {
                "path": str(package_zip),
                "size_bytes": package_zip.stat().st_size,
                "sha256": sha256(package_zip),
            },
            "payload": zip_records,
        },
    )
    return package_zip, package_json


def main() -> int:
    try:
        package_zip, package_json = package(parse_args())
    except (PackageError, OSError, ValueError, zipfile.BadZipFile) as exc:
        print(f"package-windows-player: error: {exc}", file=sys.stderr)
        return 1
    print(f"created {package_zip}")
    print(f"wrote {package_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())