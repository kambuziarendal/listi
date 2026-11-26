"""Internal host driver for run-ephemeral-audit.py --extended-fixture.

Use an isolated host with the loopback fixture ports exclusively available.
"""
import contextlib
import json
import os
import runpy
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@contextlib.contextmanager
def service(command, log):
    with log.open("wb") as stream:
        proc = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
        try:
            yield proc
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def wait_listening(port, process):
    deadline = time.monotonic() + 20
    while process.poll() is None and time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return
        except OSError:
            time.sleep(0.2)
    raise RuntimeError(f"Fixture failed to listen on {port}")


def extended(config, output, ssh, scratch):
    def guest(command, log, **kwargs):
        with (output / log).open("wb") as stream:
            subprocess.run([*ssh, command], stdout=stream, stderr=subprocess.STDOUT,
                           timeout=600, check=True, **kwargs)

    cert, key = config / "ftps.pem", config / "ftps.key"
    if not cert.is_file() or not key.is_file():
        raise ValueError("Missing guest-test FTPS server certificate/key")
    guest("set -e; cd ~/listi-ephemeral; LISTI_TEST_VOLUME=/ListiTest "
          "python3 tests/run-native-audit.py staging-budget cross-volume-move; "
          "python3 ci/link-network-case.py; python3 ci/virtual-fixtures.py build/virtual",
          "extended-storage.log")

    receipts = runpy.run_path(str(ROOT / "ci/qualification_receipts.py"))
    receipts["validate_full_audit"]((output / "extended-storage.log").read_text().splitlines(),
                                    ["staging-budget", "cross-volume-move"])
    network = scratch / "network"
    tls = scratch / "tls-share"
    tls.mkdir()
    with contextlib.ExitStack() as stack:
        server = stack.enter_context(service(
            [sys.executable, ROOT / "tests/network-failure-fixture.py", "--directory", network],
            output / "network-fixture.log"))
        ftps = stack.enter_context(service(
            [sys.executable, ROOT / "ci/ftps-fixture.py", tls, cert, key],
            output / "ftps-fixture.log"))
        for port in (1445, 2121, 2222, 23990):
            wait_listening(port, server)
        wait_listening(2143, ftps)
        for share in (network / "share", tls):
            (share / "hello world.txt").write_bytes(b"Listi network fixture\n")
            (share / "Årsrapport.txt").write_bytes(b"UTF-8 test\n")

        compare = network / "share" / "comparison"
        (compare / "folder").mkdir(parents=True)
        (compare / "hello world.txt").write_bytes(b"Listi network fixture\n")
        (compare / "Årsrapport.txt").write_bytes(b"UTF-8 test\n")
        (compare / "folder" / "nested.txt").write_bytes(b"nested\n")

        with (network / "known_hosts").open("rb") as known:
            guest("cat > ~/listi-ephemeral/build/known-hosts", "known-host.log", stdin=known)
        forwards = []
        for port in (1445, 2121, 2222, 2143, *range(31000, 31020), *range(31300, 31310)):
            forwards.extend(["-R", f"127.0.0.1:{port}:127.0.0.1:{port}"])
        # Keep command completion independent of sockets left open by a test.
        # -f returns only after ssh confirms all remote forward requests.
        control = str(scratch / "forward.sock")
        master = [*ssh[:-1], "-S", control]
        @contextlib.contextmanager
        def forwarding(log):
            with (output / log).open("ab") as stream:
                subprocess.run([*master, "-M", "-fN", "-o", "ExitOnForwardFailure=yes",
                                *forwards, ssh[-1]], check=True, timeout=20,
                               stdout=stream, stderr=subprocess.STDOUT)
                try:
                    yield
                finally:
                    subprocess.run([*master, "-O", "exit", ssh[-1]], check=True, timeout=10,
                                   stdout=stream, stderr=subprocess.STDOUT)

        with forwarding("forwarding.log"):
            guest("cd ~/listi-ephemeral && build/folder-compare-network build/known-hosts "
                  "smb://127.0.0.1:1445/files/comparison ftp://127.0.0.1:2121/comparison "
                  "sftp://127.0.0.1:2222/comparison", "folder-compare-network.log")
            guest("set -e; cd ~/listi-ephemeral; "
                  'LISTI_TEST_NETWORK_HOST=127.0.0.1 LISTI_TEST_FTPS=1 /boot/system/apps/Listi '
                  '--virtual-test "$PWD/build/virtual" "$PWD/build/known-hosts"',
                  "installed-protocols.log")
        receipts["validate_checks"]((output / "installed-protocols.log").read_text().splitlines(),
                                    110, "virtual checks")
        guest_root = subprocess.check_output([*ssh, "cd ~/listi-ephemeral && pwd"],
                                             text=True, timeout=20).strip()
        (scratch / "ssh").write_text("#!/bin/sh\nexec " + shlex.join(ssh) + ' "$@"\n')
        (scratch / "ssh").chmod(0o700)
        env = os.environ.copy()
        env.pop("NET_CASES", None)
        env.pop("NET_RUN", None)
        env.update(LISTI_NETWORK_TEST_BINARY=guest_root + "/build/network-interruptions",
                   LISTI_NETWORK_KNOWN_HOSTS=guest_root + "/build/known-hosts")
        matrix = [sys.executable, ROOT / "tests/run-network-interruptions.py"]
        cases = subprocess.check_output([*matrix, "--list"], text=True).splitlines()
        if len(cases) != 27 or len(set(cases)) != 27:
            raise RuntimeError("Invalid interruption case manifest")
        results = []
        try:
            with (output / "interruptions.log").open("wb") as stream:
                for case in cases:
                    suffix = "-" + case.replace(":", "-")
                    with forwarding("forwarding.log"):
                        subprocess.run([*matrix, scratch], env=dict(env, NET_CASES=case, NET_RUN=suffix),
                                       stdout=stream, stderr=subprocess.STDOUT, timeout=100, check=True)
                    row, = json.loads((scratch / ("network-results" + suffix + ".json")).read_text())
                    if not row["ok"] or row["protocol"] + ":" + row["mode"] != case:
                        raise RuntimeError(f"Missing passing interruption receipt for {case}")
                    results.append(row)
                    (output / "network-results.json").write_text(json.dumps(results, indent=2))
        finally:
            for entry in (*scratch.glob("*.log"), *scratch.glob("network-results-*.json")):
                shutil.copyfile(entry, output / entry.name)
        results = json.loads((output / "network-results.json").read_text())
        if len(results) != 27 or len({(r["protocol"], r["mode"]) for r in results}) != 27:
            raise RuntimeError("Incomplete interruption matrix")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="listi-extended-") as directory:
        extended(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3:], Path(directory))
