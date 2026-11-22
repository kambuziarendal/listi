#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run Listi's Haiku audit in a local throwaway overlay; never modify the base."""
import argparse
import hashlib
import re
from pathlib import Path
import shlex
import socket
import subprocess
import tarfile
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True,
                        help="quiescent Haiku image with native build tools and SSH enabled")
    parser.add_argument("--base-format", choices=("qcow2", "raw"), default="qcow2")
    parser.add_argument("--key", type=Path, required=True, help="private guest SSH identity")
    parser.add_argument("--user", default="user")
    parser.add_argument("--output", type=Path, required=True, help="new directory for audit evidence")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--qemu-img", default="qemu-img")
    parser.add_argument("--bios-dir", type=Path)
    parser.add_argument("--accel", choices=("kvm", "tcg", "hvf"), default="tcg")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--preflight", action="store_true", help="boot and verify an exported tree only")
    parser.add_argument("--qualify-package", action="store_true",
                        help="require the complete suite and test the exact newly installed HPKG")
    parser.add_argument("--extended-fixture", type=Path, metavar="DIRECTORY",
                        help="run opt-in local storage and network qualification; see docs/CI.md")
    parser.add_argument("tests", nargs="*", help="leave empty for the full native suite")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if (args.qualify_package or args.extended_fixture) and (args.preflight or args.tests):
        parser.error("Package qualification requires the full suite.")
    if args.extended_fixture:
        args.qualify_package = True
    base = args.base.resolve(strict=True)
    key = args.key.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir()
    with tempfile.TemporaryDirectory(prefix="listi-native-") as temp:
        run(args, base, key, output, Path(temp))


def run(args, base, key, output, scratch):
    overlay = scratch / "native.qcow2"
    archive = scratch / "source.tar"
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    subprocess.run(["git", "archive", "--output", archive, revision], check=True)
    (output / "source.txt").write_text(f"Exported commit: {revision}\n")
    subprocess.run([args.qemu_img, "create", "-f", "qcow2", "-F", args.base_format,
                    "-b", base, overlay], check=True)
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    command = [args.qemu, "-machine", f"pc,accel={args.accel}", "-m", "2048",
               "-smp", str(args.jobs), "-display", "none", "-monitor", "none",
               "-drive", f"file={overlay},format=qcow2,if=ide",
               "-netdev", f"user,id=n0,restrict=on,hostfwd=tcp:127.0.0.1:{port}-:22",
               "-device", "e1000,netdev=n0,romfile=",
               "-serial", f"file:{output / 'serial.log'}"]
    if args.bios_dir:
        command.extend(["-L", str(args.bios_dir.resolve(strict=True))])
    ssh = ["ssh", "-F", "/dev/null", "-o", "ForwardAgent=no", "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes",
           "-o", "StrictHostKeyChecking=accept-new", "-o", "ConnectTimeout=3",
           "-o", f"UserKnownHostsFile={scratch / 'known-hosts'}",
           "-i", str(key), "-p", str(port), f"{args.user}@127.0.0.1"]
    with (output / "qemu.log").open("wb") as qemu_log:
        guest = subprocess.Popen(command, stdout=qemu_log, stderr=qemu_log)
        try:
            ready(guest, ssh)
            audit(args, output, ssh, archive)
        finally:
            if guest.poll() is None:
                guest.terminate()
                try:
                    guest.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    guest.kill()
                    guest.wait()


def ready(guest, ssh):
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        if guest.poll() is not None:
            raise RuntimeError(f"QEMU exited during boot ({guest.returncode}); see qemu.log")
        try:
            status = subprocess.run([*ssh, "uname -s"], capture_output=True,
                                    text=True, timeout=5)
            if status.returncode == 0 and status.stdout.strip() == "Haiku":
                return
        except subprocess.TimeoutExpired:
            pass
        time.sleep(2)
    raise RuntimeError("Guest did not answer SSH as Haiku within two minutes.")


def audit(args, output, ssh, archive):
    # Quote user-supplied test names for the remote shell. Makefile TESTS is
    # interpolated in a recipe, so admit only native fixture names here.
    if any(not re.fullmatch(r"[a-z0-9][a-z0-9-]*", name) for name in args.tests):
        raise ValueError("Invalid native test name.")
    unpack = "set -e; mkdir ~/listi-ephemeral; cd ~/listi-ephemeral; tar xf -"
    with archive.open("rb") as source, (output / "export.log").open("wb") as log:
        subprocess.run([*ssh, unpack], stdin=source, stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    manifest = output / "source.sha256"
    with tarfile.open(archive) as tree, manifest.open("w") as hashes:
        for member in tree:
            if not member.isfile():
                continue
            digest = hashlib.sha256()
            with tree.extractfile(member) as blob:
                for block in iter(lambda: blob.read(1024 * 1024), b""):
                    digest.update(block)
            hashes.write(f"{digest.hexdigest()}  {member.name}\n")
    with manifest.open("rb") as hashes, (output / "verify.log").open("wb") as log:
        subprocess.run([*ssh, "cd ~/listi-ephemeral && sha256sum -c -"],
                       stdin=hashes, stdout=log, stderr=subprocess.STDOUT, check=True)
    remote = "set -e; cd ~/listi-ephemeral; uname -a"
    if not args.preflight:
        remote += "; " + shlex.join(["make", f"-j{args.jobs}", "audit",
                                     "TESTS=" + " ".join(args.tests)])
    with (output / "audit.log").open("wb") as log:
        subprocess.run([*ssh, remote], stdout=log, stderr=subprocess.STDOUT, check=True)


    if args.qualify_package:
        lines = (output / "audit.log").read_text(errors="replace").splitlines()
        skips = [line for line in lines if line.startswith("SKIP ")]
        if skips:
            raise RuntimeError("Incomplete native qualification: " + "; ".join(skips))
        if not lines or not re.fullmatch(
            r"PASS Listi --self-test; [1-9][0-9]* selected native cases exited successfully",
            lines[-1]
        ):
            raise RuntimeError("Missing final native suite result")
        package = """set -eu
cd ~/listi-ephemeral
make package
set -- dist/*.hpkg
test "$#" -eq 1
pkgman install -R -y "$PWD/$1"
metadata=$(awk '$1 == "version" {print $2}' PackageInfo)
active=/boot/system/packages/listi-${metadata}-x86_64.hpkg
cmp "$1" "$active"
cmp src/objects.*-release/Listi /boot/system/apps/Listi
sha256sum "$1" "$active" /boot/system/apps/Listi
/boot/system/apps/Listi --self-test
"""
        with (output / "installed.log").open("wb") as log:
            subprocess.run([*ssh, package], stdout=log, stderr=subprocess.STDOUT, check=True)
    if args.extended_fixture:
        # Host dependencies are opt-in. Nothing here uses a production service.
        helper = Path(__file__).resolve().parents[1] / "ci/run-extended.py"
        subprocess.run([sys.executable, helper, args.extended_fixture, output, *ssh], check=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        raise SystemExit(f"Ephemeral audit failed: {error}") from error
