#!/usr/bin/env python3
"""Link native regression cases against the current Haiku build and run them."""
from pathlib import Path
import hashlib
import io
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
OUTPUT = ROOT / "build/native-audit-tests"
REQUIRED = """
file-operation-faults directory-finalization copy-crash-recovery worker-lifecycle virtual-operation-faults archive-read-scope
operation-queue removable-copy disk-actions archive-move-scaling directory-delivery terminal-delivery search-filters
archive-local-move-faults archive-local-move tar-edit-ownership same-file-type folder-compare
archive-editor-faults archive-editor archive-move-faults archive-move
archive-folder-operations transfer-races archive-ambiguity archive-metadata archive-volumes
sevenzip-backend archive-passwords archive-jobs ar-writer arj-writer rar-store
cab-writer lha-writer preferences-state message-store-faults recovery-inventory main-window-routing working-windows folder-tabs application-services settings-window
workspace-layout workspace-editor miller-columns navigation-history selection-model type-to-select
pane-filter pane-search-list file-list-export selection-summary selection-sort saved-places saved-searches saved-navigation-controller file-search
live-search search-results-view flat-view file-clipboard clipboard-text-file file-drop image-convert
inspector-content inspector-document inspector-editor inspector-photo
terminal-session terminal-dock bulk-rename bulk-rename-window shelf shelf-collections
shelf-collections-window named-entry file-properties attribute-columns
attribute-columns-ui adaptive-scroll properties-faults attribute-copy job-snapshot icon-cache
audit-io theme-text connection-defaults ssh-key-format folder-history
""".split()

WRAPS = {
    "folder-history": ["write", "fstat", "real_time_clock_usecs"],
    "working-windows": ["stat", "_ZN10BPopUpMenu2GoE6BPointbbb"],
    "removable-copy": ["fsync", "write", "read"],
    "directory-finalization": ["fsync", "fs_stat_dev"],
    "archive-move-faults": ['renameat'],
    "archive-editor-faults": ['rename', 'renameat', 'fsync', 'fs_stat_dev'],
    "archive-local-move-faults": ['renameat', 'unlink', 'openat'],
    "file-operation-faults": ['lstat', 'fs_stat_dev', 'openat', 'write', 'unlinkat', 'stat', 'close', 'fsync', '_ZN5BNode10RemoveAttrEPKc'],
    "recovery-inventory": ["lstat", "access"],
    "copy-crash-recovery": ["openat", "renameat", "unlinkat", "fsync"],
    "message-store-faults": ["mkstemp", "write", "close", "fsync", "renameat"],
    "inspector-document": ['fsync'],
    "bulk-rename": ['renameat', 'fstatat'],
    "clipboard-text-file": ['write', 'close', 'fsync'],
    "archive-move-scaling": ['lstat'],
    "terminal-delivery": ["_ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl",
                          "_ZN5BView10DrawBitmapEPK7BBitmap5BRectS3_"],
    "directory-delivery": ['opendir', 'readdir', '_ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl'],
    "operation-queue": ['_ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl', 'opendir'],
    "flat-view": ['_ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl', 'opendir'],
    "folder-compare": ['opendir'],
    "properties-faults": ['_ZN5BNode9WriteAttrEPKcjlPKvm', '_ZN5BNode4SyncEv', '_ZN8BMessage9AddStringEPKcS1_', 'fsync'],
}


def run_disk_actions(binary):
    # Never format a supplied device. When no fixture was supplied, use only
    # the new file in our private directory and unregister that same file.
    folder = Path(tempfile.mkdtemp(prefix="listi-disk-audit-"))
    image = folder / "volume.bfs"
    try:
        with image.open("xb") as stream:
            stream.truncate(32 * 1024 * 1024)
        subprocess.run(["mkfs", "-q", "-t", "bfs", image, "ListiMountQA"], check=True)
        registration = subprocess.check_output(["diskimage", "register", image], text=True)
        try:
            device = re.search(r'"(/dev/disk/virtual/files/[0-9]+/raw)"', registration)
            if device is None:
                raise RuntimeError(f"Cannot identify file device for {image}: {registration}")
            subprocess.run([binary], cwd=ROOT, check=True, timeout=90,
                           env={**os.environ, "LISTI_TEST_DISK_DEVICE": device.group(1)})
        finally:
            # On cleanup failure leave the backing file available for inspection.
            subprocess.run(["diskimage", "unregister", image], check=True)
    except BaseException:
        print(f"Disk audit fixture retained at {folder}", file=sys.stderr)
        raise
    image.unlink()
    folder.rmdir()


def run_tar_edit_ownership(binary):
    # Use an independent TAR writer/reader, not the production libarchive adapter.
    with tempfile.TemporaryDirectory(prefix="listi-tar-owner-") as folder:
        path = Path(folder) / "owners.tar.gz"
        with tarfile.open(path, "w:gz") as archive:
            for name in ("keep", "remove"):
                header = tarfile.TarInfo(name)
                header.uid, header.gid = 1001, 2002
                header.uname, header.gname = "original-owner", "original-group"
                header.size = len(name)
                archive.addfile(header, io.BytesIO(name.encode()))
        subprocess.run([binary, path], cwd=ROOT, check=True, timeout=90)
        with tarfile.open(path, "r:gz") as archive:
            assert archive.getnames() == ["keep"]
            member = archive.getmember("keep")
            assert (member.uid, member.gid) == (1001, 2002)
            assert (member.uname, member.gname) == ("original-owner", "original-group")
            assert archive.extractfile(member).read() == b"keep"


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
            inputs = [SRC / "JobReportStore.cpp", SRC / "DirectoryFinalization.cpp",
                      SRC / "RemovableCopy.cpp",
                      SRC / "VirtualOperation.cpp", SRC / "VirtualFile.cpp",
                      SRC / "VirtualLocation.cpp", SRC / "Sha256.cpp",
                      SRC / "RecoveryInventory.cpp", SRC / "MessageStore.cpp"]
        binary = OUTPUT / name
        subprocess.run([
            *compiler, *shlex.split(config["flags"]), ROOT / "tests" / f"{name}.cpp",
            *inputs, *shlex.split(config["libraries"]),
            *(f"-Wl,--wrap={symbol}" for symbol in WRAPS.get(name, [])),
            "-o", binary,
        ], cwd=SRC, check=True)
        if name == "disk-actions" and not os.environ.get("LISTI_TEST_DISK_DEVICE"):
            run_disk_actions(binary)
        elif name == "tar-edit-ownership":
            run_tar_edit_ownership(binary)
        else:
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
