#!/usr/bin/env python3
"""Emit a stable build identity, including for source archives without .git."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
source = root / "src"
digest = hashlib.sha256()
inputs = [root / "PackageInfo", root / "ci/build-identity.py"]
for path in source.rglob("*"):
    if (path.is_file() and not any(part.startswith("objects.") for part in path.parts)
            and (path.suffix in {".cpp", ".c", ".h", ".inc", ".rdef"} or path.name == "Makefile")):
        inputs.append(path)
for path in sorted(inputs):
    digest.update(str(path.relative_to(root)).encode() + b"\0")
    digest.update(hashlib.sha256(path.read_bytes()).digest())

identity = "source " + digest.hexdigest()[:16]
if (root / ".git").exists():
    def git(*args):
        return subprocess.check_output(["git", *args], cwd=root, stderr=subprocess.DEVNULL).decode().strip()

    try:
        commit = git("rev-parse", "--short=12", "HEAD")
        state = "modified" if git("status", "--porcelain") else "clean"
        identity += f", {commit} ({state})"
    except (OSError, subprocess.CalledProcessError):
        identity += ", Git state unknown"
else:
    identity += ", Git state unknown"

version = next((line.split()[1] for line in (root / "PackageInfo").read_text().splitlines()
                if line.startswith("version ")), "unknown")
header = f"#define LISTI_BUILD_ID {json.dumps(identity)}\n#define LISTI_PACKAGE_VERSION {json.dumps(version)}\n"
output = Path(sys.argv[1])
if not output.exists() or output.read_text() != header:
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(header)
