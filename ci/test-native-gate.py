#!/usr/bin/env python3
"""Exercise the real runner's exit contract with a tiny POSIX-only build tree."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="listi-gate-") as directory:
        root = Path(directory)
        src = root / "src"
        tests = root / "tests"
        src.mkdir()
        tests.mkdir()
        runner = tests / "run-native-audit.py"
        shutil.copyfile(ROOT / "tests" / runner.name, runner)
        (src / "App.o").touch()  # Deliberately excluded by the native linker.
        (src / "Listi").write_text("#!/bin/sh\nexit 0\n")
        (src / "Listi").chmod(0o700)
        (src / "Makefile").write_text(
            "listi-audit-config:\n"
            "\t@echo listi:target=Listi; echo listi:compiler=c++; "
            "echo listi:objects=App.o; echo listi:flags=-std=c++17; echo listi:libraries=-lm\n"
        )
        cases = [
            ("ok", "int main() { return 0; }", True, "PASS ok"),
            ("bad-build", "#error intentional compile failure", False, "intentional compile failure"),
            ("bad-assertion", '#include <cassert>\nint main() { assert(false); }',
             False, "Native audit failed:"),
            ("missing", None, False, "Missing or invalid native test: missing"),
        ]
        for name, source, success, evidence in cases:
            if source is not None:
                (tests / (name + ".cpp")).write_text(source)
            result = subprocess.run([sys.executable, runner, name], cwd=root,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=30)
            final = "PASS Listi --self-test;" in result.stdout
            if (result.returncode == 0) != success or final != success or evidence not in result.stdout:
                raise RuntimeError(f"{name}: unexpected exit or log:\n{result.stdout}")
            print(f"PASS gate/{name}: runner exit {result.returncode}", flush=True)

        network = subprocess.run(
            [sys.executable, ROOT / "tests/run-network-interruptions.py", directory],
            env={**os.environ, "NET_CASES": "ftp:missing"}, capture_output=True,
            text=True, timeout=5
        )
        if not network.returncode or "Unknown interruption cases: ftp:missing" not in network.stderr:
            raise RuntimeError("Unknown interruption selector did not fail preflight")
        print("PASS gate/missing-interruption: no empty matrix can qualify")


if __name__ == "__main__":
    main()
