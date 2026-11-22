"""Link the opt-in case against the exact native objects used by the main audit."""
import runpy
import shlex
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
audit = runpy.run_path(str(ROOT / "tests/run-native-audit.py"))
config = audit["build_configuration"]()
objects = [obj for obj in shlex.split(config["objects"])
           if Path(obj).name not in {"SelfTest.o", "App.o", "VirtualTest.o"}]
subprocess.run([
    *shlex.split(config["compiler"]), *shlex.split(config["flags"]),
    ROOT / "tests/network-interruptions.cpp", *objects, *shlex.split(config["libraries"]),
    "-o", ROOT / "build/network-interruptions",
], cwd=ROOT / "src", check=True)
