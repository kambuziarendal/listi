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


def validate_link_receipts(case, lines):
    if case == "archive-copy-inputs":
        required = {"PASS mounted fixture contains both real hard-link pairs": 1,
                    "PASS protected both mounted archive aliases through replacement and retry": 1}
    elif case == "file-operation-faults":
        required = {
            "PASS Tracker original-path strings are present and distinct on the native Trash volume": 1,
            "PASS three fixture names refer to one inode before Trash": 1,
            "PASS Trash preview and worker refuse a shared original-path attribute": 2,
            "PASS both selected names and the outside link retain bytes and metadata": 1,
        }
        # A real three-link inode is mandatory. Attribute support is measured,
        # not inferred from the volume type: only one verified result is valid.
        if sum(lines.count("PASS three-link volume: Tracker original-path attribute verified " + state)
               for state in ("present", "absent")) != 1:
            raise RuntimeError("Missing or ambiguous Tracker attribute observation on three-link volume")
    else:
        return
    for receipt, count in required.items():
        if lines.count(receipt) != count:
            raise RuntimeError(f"{case}: expected {count} receipt(s): {receipt}")


def validate_full_audit(lines, required):
    reject_incomplete(lines)
    starts = [line for line in lines if line.startswith("TEST ")]
    expected = ["TEST " + name for name in required] + ["TEST Listi --self-test"]
    if starts != expected:
        raise RuntimeError("Incomplete or repeated native case selection")
    positions = [i for i, line in enumerate(lines) if line.startswith("TEST ")]
    for name, start, end in zip(required, positions, positions[1:]):
        if lines[end - 1] != "PASS " + name:
            raise RuntimeError("Missing completion receipt for " + name)
        validate_link_receipts(name, lines[start + 1:end - 1])
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
