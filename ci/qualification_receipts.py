"""Validate native and installed receipts independently of process exit status."""
import re


def reject_incomplete(lines):
    if any(re.match(r"^\s*(?:FAIL|SKIP)\b", line) for line in lines):
        raise RuntimeError("Failed or skipped required check")


def validate_checks(lines, minimum=58, label="checks"):
    reject_incomplete(lines)
    if not lines or not re.fullmatch(rf"[0-9]+ {re.escape(label)}, 0 failures", lines[-1]):
        raise RuntimeError("Missing successful self-test summary")
    count = int(lines[-1].split()[0])
    if count < minimum or sum(line.startswith("PASS ") for line in lines) != count:
        raise RuntimeError("Incomplete self-test receipts")


def validate_full_audit(lines, required):
    reject_incomplete(lines)
    starts = [line for line in lines if line.startswith("TEST ")]
    expected = ["TEST " + name for name in required] + ["TEST Listi --self-test"]
    if starts != expected:
        raise RuntimeError("Incomplete or repeated native case selection")
    positions = [i for i, line in enumerate(lines) if line.startswith("TEST ")]
    for name, end in zip(required, positions[1:]):
        if lines[end - 1] != "PASS " + name:
            raise RuntimeError("Missing completion receipt for " + name)
    if lines[-1] != f"PASS Listi --self-test; {len(required)} selected native cases exited successfully":
        raise RuntimeError("Invalid final native suite result")
    validate_checks(lines[positions[-1] + 1:-1])


def validate_installed_multiwindow(lines):
    start = "TEST installed multiwindow"
    done = "PASS installed multiwindow"
    if (not lines or lines[0] != start or lines[-1] != done or
            lines.count(start) != 1 or lines.count(done) != 1 or
            "PASS closing the last browser exits Listi" not in lines):
        raise RuntimeError("Missing or incomplete installed multiwindow receipts")
