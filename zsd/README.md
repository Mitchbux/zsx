# ZSD archive transport

`zsd` packages all files into one deterministic tar-like **ZSDP v1** record
stream (not POSIX tar), then calls `ZsxLatest.encode(Bytes)` once over that
whole stream. New `pack`/`upload` outputs use the **ZSX3 v1** container with
the existing ZSL1 codec, not independently compressed files. It stores bounded pieces
through Short.io by default (with an optional legacy is.gd provider). Downloading
retrieves the chunk tree, verifies every length and SHA-256 digest, then calls
`ZsxLatest.decode(Bytes)` before staging extraction.
See [the exact ZSX3 wire specification](../ZSX3_FORMAT.md). `get`/`unpack`
also retain the legacy **ZSDC v1** reader; no native ZSX2 archive support is
implied. The codec's internal 12 MiB blocks remain independent of file boundaries.

## Commands

```text
zsd upload file1 folder2
zsd get et56g4
zsd upload [NETWORK OPTIONS] <file|folder|wildcard>...
zsd get [NETWORK OPTIONS] [--output DIR] [--force] SHORT_ID_OR_REFERENCE
zsd pack --output FILE.zsd [--force] <file|folder|wildcard>...
zsd unpack [--output DIR] [--force] FILE.zsd

NETWORK OPTIONS: --provider shortio|legacy  --domain HOST  --endpoint URL
                 --timeout-ms N  --retries N  --pace-ms N
```

The default provider is **Short.io**, and the compiled default shortening domain
is **pex.s.gy**. This works outside Replit with `SHORT_IO_DOMAIN` unset.
The normal/shareable build requires `SHORT_IO_API_KEY` securely in the runtime
environment and has no embedded credentials. The explicitly private build below
instead embeds its build-time key. Bare IDs such as `et56g4` select this domain.
The example ID illustrates syntax, not guaranteed available content.
`get` and `unpack` extract into the **current working directory** by default,
not a new ID-named folder. `--output DIR` selects another destination.
Existing directories are merged and unrelated files retained. A file collision
fails without changing existing contents unless `--force` explicitly allows
replacement. Full archive validation, staging, rollback, and symlink/traversal
protections apply equally to current-directory and explicit-output extraction.

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

### Short.io batched uploads (default)

```text
zsd upload file1 folder2
zsd get ROOT_PATH

# Optional custom shortening domain / destination:
zsd upload --domain YOUR_CONFIGURED_DOMAIN file1 folder2
zsd get zsd-sio-v1:YOUR_CONFIGURED_DOMAIN:ROOT_PATH --output restored
```

For the normal/shareable build, set `SHORT_IO_API_KEY` securely in the process
environment before either command. Do not put the credential in shareable
source, comments, command arguments, or carrier
URLs. Missing credentials fail explicitly. For uploads and bare-ID downloads,
domain priority is **explicit `--domain` > `SHORT_IO_DOMAIN` > `pex.s.gy`**;
an unset or empty environment domain uses the compiled default. The domain
must belong to the account authorized by your runtime credential.
A full `zsd-sio-v1:domain:path` reference always supplies its own domain,
ignoring `SHORT_IO_DOMAIN` and the default; an explicitly conflicting
`--domain` fails before network access. Typed references select Short.io even
with `--provider legacy`, preserving their unambiguous provider identity.
Downloads use authenticated `GET /links/expand` against the API.
They never follow redirects or navigate to carrier URLs. Credentials are sent
only to `https://api.short.io/` (or an explicitly selected HTTP loopback mock).

Every new Short.io data chunk **and metadata node** uses exactly
`https://pex.wf/#` followed by standard, canonical Base64 of its transport bytes,
including literal `+`, `/`, and `=` characters. No dot/underscore substitutions
or URL escaping are applied to this `originalURL`. The `pex.wf` carrier host is
distinct from the account's shortening domain selected by `SHORT_IO_DOMAIN` or
`--domain`; the latter is still sent as the bulk API's `domain`.
Default-domain uploads print a practical bare root path so `zsd get SAME_ID`
works. Custom-domain uploads print the full reference to prevent wrong-domain
downloads. If `--domain pex.s.gy` overrides a different environment domain,
the output is also a full reference, so the printed retrieval command still
works in that environment. Full references may always be saved for portability;
bare IDs use the receiving process's current domain configuration.
Carrier URLs are decoded locally, never visited.

Downloads accept this exact new prefix with strict canonical standard Base64
and also read old Short.io `http://ok.com/?` carriers using the historical
dot/underscore Base64 alphabet. Invalid prefixes, mixed alphabets, missing or
misplaced padding, and noncanonical padding bits in the new carrier are errors.
Legacy provider writing retains `http://ok.com/?` and its custom alphabet.
The carrier is independent of the archive format: new writes carry **ZSX3 v1**,
while legacy **ZSDC v1** containers remain readable. ZSDP v1, ZSL1 compression
and ZSDT v1 trees are unchanged; ZSX3 is a new archive frame, not a new codec.
Short.io still uses 1450-byte chunks and 12-child metadata nodes. The outer
`zsd-sio-v1:domain:path` reference identifies the provider without changing
legacy IDs, legacy uploads/downloads, or the compressed archive codec.
Bare legacy IDs are ambiguous with Short.io slugs: use **`--provider legacy`**
for old is.gd entries and compatible simple-API endpoints:

```text
zsd get --provider legacy OLD_ID --output restored
zsd upload --provider legacy file1 folder2
```

`--provider shortio` remains accepted but is no longer necessary.

Creation uses `POST /links/bulk`, `allowDuplicates:false`, at most **1000 links**
per request, and a conservative **2 MiB client request-body budget**. Each
target URL is limited to **2048 UTF-8 bytes by this client**. These last two
limits are conservative client policy, not claimed provider guarantees: the
published bulk OpenAPI currently specifies neither a target-URL maximum nor
a request-body byte maximum. Short.io is URL storage, not arbitrary blob
storage. Provider plan quotas/validation can still reject an upload.

All requests remain sequential. Bulk requests have a **3000 ms completion-to-
next-bulk-start** gate, respecting five bulk requests per ten seconds.
Lookups have a separate **100 ms completion-to-next-lookup-start** gate
(at most 10/s, below the documented expand limit of 20/s). These floors apply
even with `--pace-ms 0`; larger explicit values slow either endpoint further.
Short.io's default pace is 100 ms; the legacy default remains 500 ms.
Reconciling 1000 unresolved links therefore takes roughly 100 seconds plus
network time/backoff, rather than 50 minutes. `--retries` (default
2, maximum 8) bounds retry rounds; 429, 5xx, and network failures use exponential
backoff (up to 60 seconds plus up to one second jitter). `Retry-After` seconds
or HTTP dates impose an additional minimum wait that is **never capped** by
the backoff. Retry/backoff and Retry-After floors are global to the operation:
switching between lookup and bulk cannot bypass them. Pacing uses a monotonic
clock. Each wait uses one clock sample and a strictly positive rounded-up delay,
preventing a deadline-crossing race from becoming an infinite .NET sleep.
401/403 fail immediately. Response bodies are not echoed.

Bulk success is nontransactional. Successful items retain their original
order; only unresolved paths are reconciled through the lookup API and, if
absent, retried. Content-derived deterministic paths prevent duplicate links
after timeouts or reruns; an existing path with different content is an error.
Paths hash the complete carrier URL, so new-carrier uploads produce different
paths/root references than old-carrier uploads of identical data; they do not
reuse the old links. Existing old references remain readable.
No root reference is printed until every chunk and tree node is confirmed.
On failure, partial remote links may remain; rerun the **identical input and
domain** to recover them. There is no local checkpoint or rollback/deletion of
remote links. Cancellation is checked between requests and during waits, and
aborts an in-flight request. On .NET 4 a disposed-and-drained pooled timer polls
cancellation and a monotonic total deadline every 25 ms, calling
`HttpWebRequest.Abort()` across request writes, response headers, and body reads.
Thus `--timeout-ms` is a hard total in-flight budget (subject to timer/OS
scheduling), not merely a timeout restarted by each slow-drip response read.
No per-request worker thread or live timer is left behind. The response cap
remains 8 MiB.

These links remain public and non-durable. Keep the original data and save the
completed root reference; never use this transport for secrets or your only
backup. No live upload is performed by the tests.

API contracts:
- https://developers.short.io/reference/post_links-bulk
- https://developers.short.io/reference/get_links-expand
- https://developers.short.io/reference/post_links

Offline unit tests with an injected clock/transport:
`haxe -cp . -main zsd.test.ZsdShortIoTest --interp`.
They cover exact carrier encoding (including `+`, `/`, and `=`), strict invalid
carrier rejection, old-carrier reads, 1000-plus-remainder batches, byte-exact tree
roundtrips, retry floors, partial successes, stable reruns, timeouts, authorization failures, malformed
responses, and cancellation. A 1000-unresolved-link virtual-clock case takes
100.9 seconds including initial backoff; separate tests enforce bulk pacing
and cross-endpoint Retry-After floors. The standalone output is additionally compiled
with `mcs -sdk:4 -langversion:4`.
`zsd/test/shortio_e2e.py Zsd.exe ZsdShortIoTest.exe` exercises the compiled
program and compiled test harness against a loopback HTTP mock, checking every
bulk `originalURL`, multiple input roots/files in a multichunk archive, old
Short.io carrier reads, and invalid carrier rejection, including
HTTP-date/seconds `Retry-After` parsing, redirect refusal, slow-drip/header
total deadlines, response-size rejection, and in-flight cancellation. The harness
is built from `zsd.test.ZsdShortIoTest` with the same .NET 4/C# 4 settings.
Exact offline harness build commands are in
[ZSX3_FORMAT.md](../ZSX3_FORMAT.md#offline-regression-coverage).

The legacy provider's default endpoint is `https://is.gd/`. It is **not private storage**:
entries are publicly readable by anybody with the short ID, cannot be deleted
or modified, and can be removed by the provider at any time. Invoking `upload`
is intentional consent to public storage for either provider: **no prompt or
`--allow-public-upload` flag is required**. The old flag is accepted as a no-op
for existing upload scripts. A nonblocking public-storage warning is printed.
Do not send private or irreplaceable data. Use a private authenticated storage
service outside this protocol for sensitive data.

The legacy provider never navigates to a carrier URL during download. It only
consumes the text returned by `forward.php?format=simple`, requires the encoded
`http://ok.com/?...` dummy URL used by the historical protocol, and decodes
that response locally.

## Legacy endpoint contract and throttling

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

## Private embedded-credential build

Only when you explicitly want a private, credential-bearing deliverable, run:

```sh
bash tools/zsd/build-private-zsd.sh
```

Requires Haxe/hxcs installed under the project's `haxelib`, Mono (`mcs`,
`monodis`), Python 3, and an already configured `SHORT_IO_API_KEY`. Do not pass
the key on the command line. The script rejects empty/whitespace-only keys and
CR/LF, never prints the key or compiler diagnostics, and uses restrictive
permissions with cleanup on failure. Public builds continue to use
`tools/zsd/build-zsd.sh` without the private compile-time flag.

The `-D zsd_embedded_key` C# branch reads only the generated
`PrivateCredential.ApiKey` constant, with no runtime key lookup or fallback.
The generator appends a correctly escaped C# literal after combining the Haxe
output, then compiles using `mcs -sdk:4 -langversion:4`. No extra runtime
assembly is required beyond .NET's `mscorlib` and `System` 4.0.

The private directory `outputs/private-zsd/` is git-ignored, mode 0700; files
are 0600. Its ZIP contains `privateZsd.cs`, `Zsd.exe`, a minimal README, and
third-party notices. Both source and executable contain the actual key, so
**anyone receiving either can extract and use it**. Never share them publicly,
commit them, or upload them through this public URL-storage transport.
Changing a runtime environment variable does not change the compiled key.
Deleting/rebuilding files does not revoke older copies: revoke/rotate the old
key at Short.io and rebuild to replace it. The domain still uses
`--domain` > `SHORT_IO_DOMAIN` > compiled `pex.s.gy`; no domain variable is needed.
Upload/get syntax, the `pex.wf` carrier, and archive bytes are unchanged.

The same script rebuilds the **shareable** source ZIP from an explicit allowlist:
the existing 32 entries plus the non-secret private build and verification
scripts (34 entries). Every original source and every actual ZIP entry is
checked for the configured credential (including escaped/UTF-16 forms);
the old public ZIP is replaced only after the new archive passes. The public
ZIP does not contain private outputs or an executable with an embedded key.
Private ZIP/source/executable checks report only pass/fail, never key values.

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

`ZSDP` (record stream), `ZSX3` (new compressed archive container), legacy
`ZSDC` (read-only compressed container), and `ZSDT` (chunk-tree
metadata) are versioned binary formats with explicit byte lengths and SHA-256
digests. They avoid historical comma/colon/path ambiguity. Legacy
`csdownload` entries are intentionally not auto-detected: passing one produces
an explicit “not a ZSD” error rather than a potentially unsafe interpretation.

## Offline regression test

The Haxe test entry point `zsd.test.ZsdArchiveTest` validates exact ZSX3 v1
headers, a single globally compressed record stream, shared files/empty entries,
legacy ZSDC decoding, and malformed lengths/digests. It has no provider or
network dependency. Legacy protocol integration tests should use
`--provider legacy --endpoint` with a loopback implementation of the contract
above; do not test against is.gd.

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
trees, damaged/missing chunks, overwrite protection, and unsafe paths. It also
extracts an independently constructed legacy ZSDC fixture and checks that
truncated, corrupt or unsupported ZSX3 frames never create extraction output
or alter an existing destination. All fixtures are reproducible offline.