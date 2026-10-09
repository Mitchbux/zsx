#!/usr/bin/env bash
# Explicitly private deliverable: never run under shell tracing with secrets.
set +x
set -euo pipefail
umask 077
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/../.." && pwd)
export HAXELIB_PATH="$project_dir/haxelib"
# Only credential-free compiler intermediates live outside the private output.
work=$(mktemp -d /tmp/zsd-private-build.XXXXXXXX)
trap 'rm -rf -- "$work"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
python3 - "$project_dir" "$work" <<'PY'
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import zipfile

os.umask(0o077)
root, work = map(Path, sys.argv[1:])
private = root / "outputs/private-zsd"
stage = "initialization"
complete = False

def stop(signum, frame):
    raise InterruptedError()

signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)

def check(condition):
    if not condition:
        raise RuntimeError()

def run(args):
    # Never echo compiler output: even an offending source line could contain
    # the key. Only the fixed stage name is reported on error.
    result = subprocess.run(args, cwd=root, env=child_env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    check(result.returncode == 0)
    return result.stdout

try:
    stage = "private output safety"
    check(not private.is_symlink())
    check(not private.exists() or private.is_dir())
    child_env = dict(os.environ)
    key = child_env.pop("SHORT_IO_API_KEY", "")
    child_env["HAXELIB_PATH"] = str(root / "haxelib")
    run(["git", "check-ignore", "-q", "outputs/private-zsd/privateZsd.cs"])
    check(not run(["git", "ls-files", "--", "outputs/private-zsd"]).strip())
    if private.exists():
        shutil.rmtree(private)
    stage = "build credential validation"
    check(bool(key.strip()) and "\r" not in key and "\n" not in key)
    literal = json.dumps(key, ensure_ascii=True)
    # JSON's ASCII string escaping is valid C# 4 (no escaped forward slash).
    needles = (key.encode("utf-8"), key.encode("utf-16le"),
               key.encode("utf-16be"), literal[1:-1].encode("ascii"))
    stage = "public source compilation"
    run(["bash", str(root / "tools/zsd/build-zsd.sh")])
    stage = "shareable source bundle validation"
    public_files = [
        "README.md", "TestZsxLatest.hx", "ZSX3_FORMAT.md", "ZSX_LATEST_FORMAT.md",
        "Zsd.hx", "ZsxLatest.hx", "buildzsd.hxml", "outputs/Zsd.NOTICES.txt",
        "outputs/Zsd.cs", "outputs/Zsd.source-map.txt", "reference/csdownload-0.3.cs",
        "reference/zsx-latest.c", "test/zsx_latest_c_cross.sh", "test_latest.hxml",
        "tools/zsd/THIRD-PARTY-NOTICES.txt", "tools/zsd/build-zsd.sh",
        "tools/zsd/combine-cs.sh", "tools/zsd/build-private-zsd.sh",
        "tools/zsd/verify-private-zsd.py", "zsd/README.md", "zsd/ZsdArchive.hx",
        "zsd/ZsdBinary.hx", "zsd/ZsdContainer.hx", "zsd/ZsdFs.hx",
        "zsd/ZsdHttp.hx", "zsd/ZsdInputs.hx", "zsd/ZsdProtocol.hx",
        "zsd/ZsdShortIo.hx", "zsd/test/ZsdArchiveTest.hx",
        "zsd/test/ZsdShortIoTest.hx", "zsd/test/cli_regression.py",
        "zsd/test/format_fixtures.py", "zsd/test/mock_e2e.py",
        "zsd/test/shortio_e2e.py",
    ]
    public_zip = work / "zsd-net40-source.zip"
    with zipfile.ZipFile(public_zip, "w", zipfile.ZIP_DEFLATED) as bundle:
        for name in public_files:
            data = (root / name).read_bytes()
            check(not any(needle in data for needle in needles))
            # Keep executable bits on the existing public build/test scripts.
            bundle.write(root / name, name)
    with zipfile.ZipFile(public_zip) as bundle:
        check(bundle.namelist() == public_files)
        check(bundle.testzip() is None)
        for name in bundle.namelist():
            check(not any(needle in bundle.read(name) for needle in needles))

    stage = "private Haxe generation"
    # Derive from the authoritative hxml; Haxe rejects duplicate target flags.
    # Replace only its output path and add the explicit private compile flag.
    generated = work / "generated"
    hxml = (root / "buildzsd.hxml").read_text()
    check(hxml.count("-cs tools/zsd/generated") == 1)
    private_hxml = work / "private.hxml"
    private_hxml.write_text(
        hxml.replace("-cs tools/zsd/generated", "-cs " + str(generated)) +
        "\n-D zsd_embedded_key\n")
    run(["haxe", str(private_hxml)])
    combined = work / "combined.cs"
    run(["bash", str(root / "tools/zsd/combine-cs.sh"), str(generated / "src"),
         str(combined), str(work / "source-map.txt")])
    stage = "private credential generation"
    private.mkdir(mode=0o700)
    source = private / "privateZsd.cs"
    # Move the uncredentialed source before appending: no duplicate plaintext
    # private source in generated trees, workspace caches, or temporary files.
    shutil.move(str(combined), str(source))
    with source.open("a", encoding="ascii") as output:
        output.write("\n// PRIVATE: generated build-time credential; never share.\n")
        output.write("internal static class PrivateCredential {\n")
        output.write("    internal const string ApiKey = " + literal + ";\n}\n")
    os.chmod(source, 0o600)
    stage = "private .NET 4 C# 4 compilation"
    executable = private / "Zsd.exe"
    run(["mcs", "-sdk:4", "-langversion:4", "-target:exe",
         "-out:" + str(executable), str(source)])
    refs = run(["monodis", "--assemblyref", str(executable)]).decode("utf-8")
    check(set(re.findall(r"Name=(\S+)", refs)) == {"mscorlib", "System"})
    check(set(re.findall(r"Version=([0-9.]+)", refs)) == {"4.0.0.0"})
    stage = "private artifact validation"
    code = source.read_bytes()
    check(("ApiKey = " + literal + ";").encode("ascii") in code)
    check(b'getEnv("SHORT_IO_API_KEY")' not in code)
    check(b'GetEnvironmentVariable("SHORT_IO_API_KEY")' not in code)
    check(b"global::PrivateCredential.ApiKey" in code)
    check(key.encode("utf-16le") in executable.read_bytes())
    (private / "README.txt").write_text(
        "PRIVATE ZSD - .NET Framework 4.0 / C# 4\n\n"
        "Both privateZsd.cs and Zsd.exe contain a literal Short.io API key.\n"
        "Anyone receiving either can extract and use it. Do not share publicly,\n"
        "commit, or upload this package through Zsd.\n"
        "Runtime SHORT_IO_API_KEY is not read; there is no credential fallback.\n"
        "Domain: --domain > SHORT_IO_DOMAIN > compiled pex.s.gy.\n"
        "Carrier: https://pex.wf/# plus standard Base64; no format changes.\n\n"
        "Windows: Zsd.exe upload file1 folder2\n"
        "         Zsd.exe get SHORT_ID\n"
        "Mono:    mono Zsd.exe get SHORT_ID\n"
        "get extracts into the current directory; --output DIR selects another.\n"
        "Uploads are public and non-durable. Never upload secrets.\n"
        "Recompile: mcs -sdk:4 -langversion:4 -out:Zsd.exe privateZsd.cs\n"
        "Only .NET mscorlib and System 4.0 are required. HTTPS needs TLS 1.2.\n\n"
        "Changing environment variables or rebuilding does not revoke older\n"
        "copies. Revoke/rotate the old key at Short.io, then rebuild and replace\n"
        "private copies. Keep the separate shareable source ZIP key-free.\n",
        encoding="ascii")
    shutil.copyfile(root / "tools/zsd/THIRD-PARTY-NOTICES.txt",
                    private / "NOTICES.txt")
    names = ["privateZsd.cs", "Zsd.exe", "README.txt", "NOTICES.txt"]
    package = private / "zsd-private-net40.zip"
    with zipfile.ZipFile(package, "w", zipfile.ZIP_DEFLATED) as bundle:
        for name in names:
            bundle.write(private / name, name)
    with zipfile.ZipFile(package) as bundle:
        check(bundle.namelist() == names and bundle.testzip() is None)
        check(bundle.read("privateZsd.cs") == code)
        check(bundle.read("Zsd.exe") == executable.read_bytes())
        check(("ApiKey = " + literal + ";").encode("ascii") in bundle.read("privateZsd.cs"))
        check(key.encode("utf-16le") in bundle.read("Zsd.exe"))
    for path in private.iterdir():
        os.chmod(path, 0o600)
        run(["git", "check-ignore", "-q", str(path.relative_to(root))])
    # Atomic public replacement, preserving the previous ZIP on any prior failure.
    stage = "public ZIP publication"
    public_target = root / "outputs/zsd-net40-source.zip"
    public_stage = root / "outputs/.zsd-net40-source.zip.new"
    try:
        shutil.copyfile(public_zip, public_stage)
        os.chmod(public_stage, 0o600)
        os.replace(public_stage, public_target)
    finally:
        public_stage.unlink(missing_ok=True)
    complete = True
    print("Public ZIP all 34 entries credential-free: true")
    print("Private source/executable/ZIP match build credential: true")
    print("Private .NET 4/C# 4 compilation; only mscorlib/System 4.0: true")
    print("Private outputs git-ignored; directory 0700, files 0600: true")
    for path in sorted(private.iterdir()):
        print(str(path.relative_to(root)) + " (" + str(path.stat().st_size) + " bytes)")
    print("outputs/zsd-net40-source.zip (" + str(public_target.stat().st_size) + " bytes)")
except BaseException:
    # Do not emit exception text, source, compiler lines, or secret-containing logs.
    print("Private build failed during " + stage + "; diagnostics suppressed for credential safety.",
          file=sys.stderr)
    sys.exit(1)
finally:
    if not complete and private.is_dir() and not private.is_symlink():
        shutil.rmtree(private)
PY