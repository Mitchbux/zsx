"""Reproducible offline wire fixtures, independent of the production writer.

ZSDC v1 was the pre-ZSX3 container. Its fixed ZSL1 raw-block representation
below does not call the new pack command or relabel a ZSX3 output. No network,
environment configuration, native code, or generated C# is read.
"""
import hashlib
import struct


SHARED = b"Shared bytes in two independently named files.\n" * 20
ENTRIES = [
    ("fixture", True, b""),
    ("fixture/a.bin", False, SHARED),
    ("fixture/b.bin", False, SHARED),
    ("fixture/empty", True, b""),
    ("fixture/zero.bin", False, b""),
]


def record_stream():
    result = b"ZSDP" + struct.pack(">HI", 1, len(ENTRIES))
    for path, directory, data in ENTRIES:
        name = path.encode("utf-8")
        result += struct.pack(">BH", int(directory), len(name)) + name
        result += struct.pack(">Q", len(data)) + hashlib.sha256(data).digest() + data
    return result


def stored_stream(archive):
    assert 0 < len(archive) <= 0xC00000
    chunk = struct.pack("<IIi", len(archive), len(archive), -1) + archive
    return b"ZSL1" + struct.pack("<III", len(archive), 1, 0) + struct.pack("<I", len(chunk)) + chunk


def container(archive, magic=b"ZSDC", stream=None):
    stream = stored_stream(archive) if stream is None else stream
    return (magic + struct.pack(">HQ", 1, len(archive)) + hashlib.sha256(archive).digest()
            + struct.pack(">Q", len(stream)) + stream)


def legacy_fixture():
    return container(record_stream())