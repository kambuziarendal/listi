"""A disappearing test must not silently turn into a smaller passing native suite."""
from pathlib import Path
import runpy

ROOT = Path(__file__).resolve().parents[1]
required = runpy.run_path(str(ROOT / "tests/run-native-audit.py"))["REQUIRED"]
# Independent native programs requiring special fixtures, other build recipes or
# a second volume. Some are run in the extended or portable tiers instead.
other = set("""
app-icon archive-backends archive-extraction archive-job-fixtures archive-module-retry
cross-volume-move directory-scaling folder-compare-network native-preview network-disconnect network-interruptions network-metadata
open-with open-with-receiver parsing-fuzz staging-budget theme
""".split())
sources = {p.stem for p in (ROOT / "tests").glob("*.cpp")}
expected = set(required)
if (len(required) < 80 or len(required) != len(expected) or expected & other
        or sources != expected | other):
    raise SystemExit(f"Invalid test inventory. Missing: {sorted((expected | other) - sources)}; "
                     f"unclassified: {sorted(sources - (expected | other))}; "
                     f"double-classified: {sorted(expected & other)}")
print(f"Source inventory: {len(required)} required, {len(other)} explicitly non-default")
