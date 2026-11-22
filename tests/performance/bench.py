"""Run on Haiku after native release builds. Writes one JSON object per measured run."""
import argparse
import json
import platform
import runpy
import shlex
import subprocess
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", nargs=2, action="append", required=True, metavar=("COMMIT", "SOURCE"))
parser.add_argument("--machine", required=True)
parser.add_argument("--volume", type=Path, required=True, help="disposable fixture directory")
parser.add_argument("--repetitions", type=int, default=10)
parser.add_argument("--dataset", action="append", choices=["one", "small", "tiny", "nested", "medium"])
parser.add_argument("--mode", action="append", choices=["fresh", "overwrite", "cancel"])
args = parser.parse_args()
if args.repetitions < 1:
    parser.error("repetitions must be positive")
with tempfile.TemporaryDirectory() as temp:
    builds = []
    for index, (commit, source) in enumerate(args.build):
        root = Path(source).resolve()
        config = runpy.run_path(str(root / "tests/run-native-audit.py"))["build_configuration"]()
        objects = [p for p in shlex.split(config["objects"]) if Path(p).name not in
                   {"App.o", "SelfTest.o", "VirtualTest.o"}]
        binary = Path(temp) / f"bench-{index}"
        wrappers = ["mkstemp", "close", "write", "fsync", "renameat"]
        subprocess.run([*shlex.split(config["compiler"]), *shlex.split(config["flags"]),
                        Path(__file__).resolve().with_name("copy.cpp"), *objects,
                        *(f"-Wl,--wrap={fn}" for fn in wrappers),
                        *shlex.split(config["libraries"]), "-o", binary], cwd=root / "src", check=True)
        builds.append((binary, {"commit": commit, "target": config["target"], "flags": config["flags"]}))
    metadata = {"machine": args.machine, "os": platform.platform(), "uname": list(platform.uname()),
                "volume": subprocess.check_output(["df", str(args.volume)], text=True),
                "cache": "warm source, fresh fixture per run, no cache eviction"}
    for dataset in args.dataset or ["one", "small", "tiny", "nested", "medium"]:
        for mode in args.mode or ["fresh", "overwrite", "cancel"]:
            for rep in range(args.repetitions):
                for binary, build in (builds if rep % 2 == 0 else reversed(builds)):
                    output = subprocess.check_output([binary, args.volume, dataset, mode], text=True)
                    print(json.dumps(dict(metadata, **build, dataset=dataset, mode=mode, repetition=rep,
                                          **json.loads(output))), flush=True)
