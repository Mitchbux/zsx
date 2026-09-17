# ZSD archive transport

`zsd` packages a deterministic directory/file manifest, compresses it with
`ZsxLatest.encode(Bytes)`, and stores the compressed bytes as bounded pieces
through the is.gd simple URL API (or a compatible local endpoint). Downloading
retrieves the chunk tree, verifies every length and SHA-256 digest, then calls
`ZsxLatest.decode(Bytes)` before staging extraction.

## Commands

```text
zsd upload [--endpoint URL] [--timeout-ms N] [--retries N] [--pace-ms N] \
           --allow-public-upload <file|folder|wildcard>...
zsd get [--endpoint URL] [--timeout-ms N] [--retries N] [--pace-ms N] \
        [--output DIR] [--force] SHORT_ID
zsd pack --output FILE.zsd [--force] <file|folder|wildcard>...
zsd unpack [--output DIR] [--force] FILE.zsd
```

`pack` and `unpack` are offline verification commands. `pack` refuses to
overwrite by default, refuses an output included by its input selection, and
writes a same-directory staging file before rename. `--force` is required to
replace an existing output. `get` and `unpack` likewise do not replace files
without `--force`.

Inputs can be files, folders, and deterministic `*`/`?` wildcards. Folder
trees include empty directories. Multiple input roots are represented by their
portable basename; an archive-path collision is an error rather than an
implicit overwrite. ZSD rejects symbolic links/junctions/reparse points,
absolute/traversal paths, Windows-reserved names, and non-portable filename
characters. Extraction happens under `.zsd-stage-*` only after the entire
archive has decompressed and verified.

## Public-service warning

The default endpoint is `https://is.gd/`. It is **not private storage**:
entries are publicly readable by anybody with the short ID, cannot be deleted
or modified, and can be removed by the provider at any time. For that reason,
the default service requires the explicit `--allow-public-upload` flag. Do not
send private or irreplaceable data there. Use an authenticated storage service
outside this protocol for sensitive data.

The program never navigates to a carrier URL during download. It only consumes
the text returned by `forward.php?format=simple`, requires the encoded
`http://ok.com/?...` dummy URL used by the historical protocol, and decodes
that response locally.

## Endpoint contract and throttling

`--endpoint` must be an origin URL ending in (or normalized to) `/`, without
credentials, query strings, fragments, or a subdirectory path. HTTPS is required
except for loopback test endpoints:

```text
GET {endpoint}create.php?format=simple&url={URL-encoded dummy URL}
  -> a short URL or a 5–30 character [A-Za-z0-9_] ID

GET {endpoint}forward.php?format=simple&shorturl={ID}
  -> http://ok.com/?{base64 where + is . and / is _}
```

ZSD URL-encodes the whole dummy URL, accepts provider IDs from 5 through 30
characters (not a fixed six characters), limits every uploaded payload and
metadata node to 1900 bytes, sends sequential requests, and defaults to
500 ms pacing. `--pace-ms 0` is appropriate only for a local mock.

The is.gd simple API documents HTTP **502 / error code 3** as a rate limit.
ZSD waits at least 60 seconds before a retry of that condition; it does not
rapidly retry or use concurrent/rate-limit-evasion behavior. Other transient
transport failures use bounded retries (`--retries`, default 2) and increasing
one-second delays. Requests use a bounded `HttpWebRequest` timeout (default
30,000 ms) on .NET Framework 4.0.

## Format compatibility

### .NET 4.0 and HTTPS

The generated source is compiled against .NET Framework 4.0 reference
assemblies and C# 4 syntax. HTTPS explicitly requires TLS 1.2 without disabling
certificate validation. Older Windows/.NET 4 installations may need their
security updates for TLS 1.2; unpatched installations can still use offline
pack/unpack but may not connect to the public provider. Native Windows runtime
execution has not been verified in this Linux workspace.

### Size and filesystem limits

The portable compressor accepts at most 256 MiB of uncompressed archive data;
network transfers accept at most 128 MiB of compressed container data. These
are bounded, in-memory operations, not streaming large-file backups. Network
responses and tree traversal are separately capped to reject abusive entries.

Extraction rejects reparse points/symlinks, unsafe Windows names, and
case-insensitive collisions. Use a destination tree you control: managed
cross-platform checks do not eliminate races with another process actively
replacing directories during extraction. Staging and rollback handle ordinary
write failures, not power loss or concurrent filesystem attacks.

`ZSDP` (manifest), `ZSDC` (compressed container), and `ZSDT` (chunk-tree
metadata) are versioned binary formats with explicit byte lengths and SHA-256
digests. They avoid historical comma/colon/path ambiguity. Legacy
`csdownload` entries are intentionally not auto-detected: passing one produces
an explicit “not a ZSD” error rather than a potentially unsafe interpretation.

## Offline regression test

The Haxe test entry point `zsd.test.ZsdArchiveTest` validates a directory/file
manifest round trip and rejects a tampered file digest. It has no provider or
network dependency. Protocol integration tests should point `--endpoint` at a
loopback implementation of the contract above; do not test against is.gd.

`zsd/test/mock_e2e.py` is that loopback integration test. After the standalone
binary is built, run (for example)

```text
mcs -sdk:4 -langversion:4 -out:outputs/Zsd.exe outputs/Zsd.cs
python3 zsd/test/mock_e2e.py "mono $(pwd)/outputs/Zsd.exe"
ZSD_EXE="$(pwd)/outputs/Zsd.exe" python3 zsd/test/cli_regression.py
```

It asserts a small single-leaf upload/download has exactly the data payload and
one `ZSDT` root node, that each is at most 1900 bytes, and that an empty
directory survives extraction. The broader regression suite additionally covers
compressed and random data, multiple inputs and wildcards, multi-level network
trees, damaged/missing chunks, overwrite protection, and unsafe paths.