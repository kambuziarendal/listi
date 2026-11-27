#!/usr/bin/env python3
"""Exercise the real runner's exit contract with a tiny POSIX-only build tree."""
import os
import runpy
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_receipt_contract():
    receipts = runpy.run_path(str(ROOT / "ci/qualification_receipts.py"))
    validate = receipts["validate_full_audit"]
    multiwindow = receipts["validate_installed_multiwindow"]
    installed = ["TEST installed multiwindow", "PASS closing the last browser exits Listi",
                 "PASS installed multiwindow"]
    multiwindow(installed)
    for lines in ([], installed[1:], installed[:-1], installed[::2]):
        try:
            multiwindow(lines)
        except RuntimeError:
            pass
        else:
            raise RuntimeError("Incomplete installed multiwindow receipts accepted")
    print("PASS gate/installed-multiwindow: required receipts")
    valid = ["TEST demo", "PASS demo", "TEST Listi --self-test",
             *(f"PASS check{i}" for i in range(58)), "58 checks, 0 failures",
             "PASS Listi --self-test; 1 selected native cases exited successfully"]
    validate(valid, ["demo"])
    broken = {
        "missing-test": valid[2:],
        "missing-completion": valid[:1] + valid[2:],
        "skipped": ["  SKIP unavailable", *valid],
        "failed": ["FAIL assertion", *valid],
        "empty-self-test": valid[:3] + ["0 checks, 0 failures", valid[-1]],
        "missing-self-check": valid[:3] + valid[4:],
        "repeated-case": valid[:2] + valid,
        "truncated": valid[:-1],
    }
    for reason, lines in broken.items():
        try:
            validate(lines, ["demo"])
        except RuntimeError:
            print(f"PASS invalid-log/{reason}: rejected")
        else:
            raise RuntimeError(f"Invalid native receipts accepted: {reason}")

    cases = ["archive-copy-inputs", "file-operation-faults"]
    alias = "PASS mounted fixture contains both real hard-link pairs"
    protected = "PASS protected both mounted archive aliases through replacement and retry"
    tracker = "PASS Tracker original-path strings are present and distinct on the native Trash volume"
    triple = "PASS three fixture names refer to one inode before Trash"
    attribute = "PASS three-link volume: Tracker original-path attribute verified "
    refusal = "PASS Trash preview and worker refuse a shared original-path attribute"
    unchanged = "PASS both selected names and the outside link retain bytes and metadata"
    link_log = ["TEST archive-copy-inputs", alias, protected, "PASS archive-copy-inputs",
                "TEST file-operation-faults", tracker, triple, attribute + "absent", refusal, refusal,
                unchanged, "PASS file-operation-faults", *valid[2:-1],
                "PASS Listi --self-test; 2 selected native cases exited successfully"]
    validate(link_log, cases)
    present = link_log.copy()
    present[present.index(attribute + "absent")] = attribute + "present"
    validate(present, cases)
    invalid = {}
    for line in (alias, protected, tracker, triple, attribute + "absent", refusal, unchanged):
        missing = link_log.copy()
        missing.remove(line)
        invalid["absent " + line] = missing
    invalid["wrong case"] = [alias, *invalid["absent " + alias]]
    invalid["contradictory attribute"] = [*link_log[:8], attribute + "present", *link_log[8:]]
    for reason, lines in invalid.items():
        try:
            validate(lines, cases)
        except RuntimeError:
            continue
        raise RuntimeError("Missing/ambiguous hard-link coverage accepted: " + reason)
    print("PASS gate/hardlinks: both fixture receipts and explicit Tracker state required in their own cases")


def main():
    check_receipt_contract()
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
