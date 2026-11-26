"""Link the opt-in cases against the exact native objects used by the main audit."""
import runpy
import shlex
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
audit = runpy.run_path(str(ROOT / "tests/run-native-audit.py"))
config = audit["build_configuration"]()
objects = [obj for obj in shlex.split(config["objects"])
           if Path(obj).name not in {"SelfTest.o", "App.o", "VirtualTest.o"}]
for case in ("network-interruptions", "folder-compare-network"):
    subprocess.run([
        *shlex.split(config["compiler"]), *shlex.split(config["flags"]),
        ROOT / "tests" / (case + ".cpp"), *objects, *shlex.split(config["libraries"]),
        "-o", ROOT / "build" / case,
    ], cwd=ROOT / "src", check=True)
