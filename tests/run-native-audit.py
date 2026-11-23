#!/usr/bin/env python3
"""Link native regression cases against the current Haiku build and run them."""
from pathlib import Path
import hashlib
import platform
import re
import shlex
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
OUTPUT = ROOT / "build/native-audit-tests"
REQUIRED = """
file-operation-faults copy-crash-recovery worker-lifecycle virtual-operation-faults archive-read-scope
operation-queue removable-copy archive-move-scaling directory-delivery search-filters
archive-local-move-faults archive-local-move same-file-type folder-compare
archive-editor-faults archive-editor archive-move-faults archive-move
archive-folder-operations transfer-races archive-ambiguity archive-volumes
sevenzip-backend archive-passwords archive-jobs ar-writer arj-writer rar-store
cab-writer lha-writer preferences-state message-store-faults recovery-inventory main-window-routing application-services settings-window
workspace-layout workspace-editor miller-columns navigation-history selection-model type-to-select
pane-filter selection-summary selection-sort saved-places saved-searches saved-navigation-controller file-search
live-search search-results-view flat-view file-clipboard clipboard-text-file file-drop image-convert
inspector-content inspector-document inspector-editor inspector-photo
terminal-session terminal-dock bulk-rename bulk-rename-window shelf shelf-collections
shelf-collections-window named-entry file-properties attribute-columns
attribute-columns-ui adaptive-scroll properties-faults attribute-copy job-snapshot icon-cache
audit-io theme-text connection-defaults ssh-key-format
""".split()

WRAPS = {
    "removable-copy": ["fsync", "write", "read"],
    "archive-move-faults": ['renameat'],
    "archive-editor-faults": ['rename', 'renameat', 'fsync', 'fs_stat_dev'],
    "archive-local-move-faults": ['renameat', 'unlink'],
    "file-operation-faults": ['openat', 'write', 'unlinkat', 'stat', 'close', 'fsync', '_ZN5BNode10RemoveAttrEPKc'],
    "recovery-inventory": ["lstat", "access"],
    "copy-crash-recovery": ["openat", "renameat", "unlinkat", "fsync"],
    "message-store-faults": ["mkstemp", "write", "close", "fsync", "renameat"],
    "inspector-document": ['fsync'],
    "bulk-rename": ['renameat', 'fstatat'],
    "clipboard-text-file": ['write'],
    "archive-move-scaling": ['lstat'],
    "directory-delivery": ['opendir', '_ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl'],
    "operation-queue": ['_ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl', 'opendir'],
    "flat-view": ['_ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl', 'opendir'],
    "folder-compare": ['opendir'],
    "properties-faults": ['_ZN5BNode9WriteAttrEPKcjlPKvm', '_ZN5BNode4SyncEv', 'fsync'],
}


def build_configuration():
    result = subprocess.run(
        ["make", "-s", "--no-print-directory", "listi-audit-config"],
        cwd=SRC, check=True, stdout=subprocess.PIPE, text=True,
    )
    config = {}
    for line in result.stdout.splitlines():
        if line.startswith("listi:"):
            field, value = line[6:].split("=", 1)
            config[field] = value
    for field in ("target", "compiler", "objects", "flags", "libraries"):
        if not config.get(field):
            raise ValueError(f"Native build configuration lacks {field}")
    return config


def git_version():
    if not shutil.which("git") or not (ROOT / ".git").exists():
        return "unknown (not a Git checkout or git unavailable)"
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    changes = subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True)
    return revision + (" (dirty)" if changes else " (clean)")


def file_digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run():
    names = sys.argv[1:] or REQUIRED
    for name in names:
        if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", name) or not (ROOT / "tests" / f"{name}.cpp").is_file():
            raise ValueError(f"Missing or invalid native test: {name}")
    config = build_configuration()
    compiler = shlex.split(config["compiler"])
    version = subprocess.check_output([*compiler, "--version"], text=True).splitlines()[0]
    print("Commit:", git_version())
    print("System:", platform.platform(), platform.machine())
    print("Compiler:", version)
    app = SRC / config["target"]
    print("Executable:", app)
    print("Executable SHA-256:", file_digest(app))
    print("Selected:", " ".join(names), "+ Listi --self-test")
    print("Case status is process exit status; fixture SKIP messages are not executed coverage.")
    packages = Path("/boot/system/packages")
    # Versioned package filenames record the installed environment, including optional backends.
    print("System packages:", " ".join(sorted(p.name for p in packages.glob("*.hpkg"))) or "unknown",
          flush=True)

    all_objects = [SRC / item for item in shlex.split(config["objects"])]
    for path in [app, *all_objects]:
        if not path.is_file():
            raise ValueError(f"Missing native build product: {path}; run make audit to build first")
    objects = [p for p in all_objects if p.name not in {"App.o", "SelfTest.o", "VirtualTest.o"}]
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for name in names:
        print("TEST", name, flush=True)
        inputs = objects
        if name == "virtual-operation-faults":
            inputs = [SRC / "VirtualOperation.cpp", SRC / "VirtualFile.cpp",
                      SRC / "VirtualLocation.cpp", SRC / "Sha256.cpp",
                      SRC / "RecoveryInventory.cpp", SRC / "MessageStore.cpp"]
        binary = OUTPUT / name
        subprocess.run([
            *compiler, *shlex.split(config["flags"]), ROOT / "tests" / f"{name}.cpp",
            *inputs, *shlex.split(config["libraries"]),
            *(f"-Wl,--wrap={symbol}" for symbol in WRAPS.get(name, [])),
            "-o", binary,
        ], cwd=SRC, check=True)
        subprocess.run([binary], cwd=ROOT, check=True, timeout=90)
        print("PASS", name, flush=True)
    print("TEST Listi --self-test", flush=True)
    subprocess.run([app, "--self-test"], cwd=ROOT, check=True, timeout=120)
    print(f"PASS Listi --self-test; {len(names)} selected native cases exited successfully", flush=True)


if __name__ == "__main__":
    try:
        run()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        sys.exit(f"Native audit failed: {error}")
