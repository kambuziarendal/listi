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
file-operation-faults trash-metadata directory-finalization copy-crash-recovery worker-lifecycle virtual-operation-faults archive-read-scope
operation-queue removable-copy disk-actions archive-move-scaling directory-delivery terminal-delivery search-filters
archive-local-move-faults archive-local-move archive-copy-inputs archive-panel-version tar-edit-ownership tar-move-ownership same-file-type folder-compare
archive-editor-faults archive-editor archive-move-faults archive-move
archive-folder-operations transfer-races transfer-preflight archive-ambiguity archive-metadata archive-volumes
sevenzip-backend archive-passwords archive-jobs ar-writer arj-writer rar-store
cab-writer lha-writer preferences-state stored-collection-faults message-store-faults recovery-inventory main-window-routing working-windows folder-tabs application-services settings-window
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
    "archive-editor": ["_ZNK7BRoster6LaunchEPK9entry_refPK8BMessagePi"],
    "archive-panel-version": ["_ZNK8BMessage10WasDroppedEv", "opendir", "_Z20PostMainWindowNoticePKc"],
    "archive-copy-inputs": ["openat", "write", "close"],
    "virtual-operation-faults": ["openat", "write", "close"],
    "archive-jobs": ["_ZNK8BMessage7FindRefEPKciP9entry_ref", "pread"],
    "transfer-preflight": ["opendir", "readdir", "closedir"],
    "disk-actions": ["_ZN8BMessage8AddInt64EPKcl"],
    "pane-search-list": ["lstat"],
    "stored-collection-faults": ["_ZN8BMessage10AddMessageEPKcPKS_"],
    "folder-tabs": ["_ZN8BMessage10AddMessageEPKcPKS_"],
    "job-snapshot": ["_ZN8BMessage9AddStringEPKcS1_"],
    "folder-history": ["write", "fstat", "real_time_clock_usecs", "_ZNK8BMessage11FindMessageEPKciPS_"],
    "working-windows": ["stat", "_ZN10BPopUpMenu2GoE6BPointbbb"],
    "removable-copy": ["fsync", "write", "read"],
    "directory-finalization": ["fsync", "fs_stat_dev"],
    "tar-move-ownership": ["renameat"],
    "archive-move-faults": ["renameat", "fstat"],
    "archive-editor-faults": ['rename', 'renameat', 'fsync', 'fs_stat_dev'],
    "archive-local-move-faults": ['renameat', 'unlink', 'openat'],
    "file-operation-faults": ['lstat', 'fs_stat_dev', 'openat', 'write', 'unlinkat', 'stat', 'close', 'fsync', '_ZN5BNode10RemoveAttrEPKc'],
    "trash-metadata": ["_ZNK5BNode8ReadAttrEPKcjlPvm", "_ZN5BNode9WriteAttrEPKcjlPKvm",
                       "_ZN6BEntry6MoveToEP10BDirectoryPKcb"],
    "recovery-inventory": ["lstat", "access"],
    "copy-crash-recovery": ["openat", "renameat", "unlinkat", "fsync"],
    "message-store-faults": ["mkstemp", "write", "close", "fsync", "renameat"],
    "inspector-document": ['fsync'],
    "bulk-rename": ['renameat', 'fstatat'],
    "clipboard-text-file": ['write', 'close', 'fsync'],
    "archive-move-scaling": ['lstat', 'realpath'],
    "terminal-delivery": ["_ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl",
                          "_ZN5BView10DrawBitmapEPK7BBitmap5BRectS3_"],
    "application-services": ['fsync', '_Z14find_directory15directory_whichP5BPathbP7BVolume'],
    "directory-delivery": ['_ZN8BMessage10AddMessageEPKcPKS_', '_ZNK8BMessage11FindMessageEPKciPS_', 'fstatat', 'opendir', 'readdir', '_ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl'],
    "operation-queue": ['_ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl', 'opendir'],
    "flat-view": ['_ZN8BMessage10AddMessageEPKcPKS_', '_ZNK8BMessage11FindMessageEPKciPS_', '_ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl', 'opendir'],
    "folder-compare": ['opendir'],
    "properties-faults": ['_ZN5BNode9WriteAttrEPKcjlPKvm', '_ZN5BNode4SyncEv', '_ZN8BMessage9AddStringEPKcS1_', 'fsync'],
}


def run_hardlink_case(binary, name, seed):
    # Only mount a private copy; these tests deliberately rename/delete links.
    # Never recursively remove a directory if unmount or unregister failed.
    scratch = Path(tempfile.mkdtemp(prefix="listi-hardlinks-"))
    mountpoint, image = scratch / "volume", scratch / "hardlinks.img"
    mountpoint.mkdir()
    shutil.copyfile(seed, image)
    registration = subprocess.check_output(["diskimage", "register", image], text=True)
    match = re.search(r'"(/dev/disk/virtual/files/[0-9]+/raw)"', registration)
    try:
        if not match:
            raise RuntimeError(f"Cannot identify hard-link image device: {registration}")
        subprocess.run(["mount", "-t", "ext2", match[1], mountpoint], check=True)
        try:
            env = os.environ.copy()
            if name == "archive-copy-inputs":
                env["LISTI_ARCHIVE_ALIAS_DIR"] = str(mountpoint / "archive-aliases")
            else:
                env["LISTI_DELETE_HARDLINK_FIXTURE"] = str(mountpoint / "deletion")
                env["LISTI_HARDLINK_TEST_PAIR"] = str(mountpoint / "move-pair")
            subprocess.run([binary], cwd=ROOT, check=True, timeout=90, env=env)
        finally:
            subprocess.run(["unmount", mountpoint], check=True)
    finally:
        if os.path.ismount(mountpoint):
            raise RuntimeError(f"Hard-link image still mounted at {mountpoint}")
        subprocess.run(["diskimage", "unregister", image], check=True)
        mountpoint.rmdir()
        image.unlink()
        scratch.rmdir()


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
            directory = tarfile.TarInfo("shared")
            directory.type = tarfile.DIRTYPE
            directory.mode = 0o1777
            directory.mtime = 1700000000.125
            directory.pax_headers["ctime"] = "1710000000.123456789"
            archive.addfile(directory)
            for name in ("keep", "remove", "old-name"):
                header = tarfile.TarInfo(name)
                if name == "keep":
                    header.pax_headers["LIBARCHIVE.creationtime"] = "1690000000.987654321"
                    header.pax_headers["atime"] = "1710000001.987654321789"
                    header.pax_headers["ctime"] = "1710000002.123456789987"
                    header.pax_headers["mtime"] = "1700000001.375123456789"
                    header.mode = 0o4750
                    header.mtime = 1700000001.375
                header.uid, header.gid = 1001, 2002
                header.uname, header.gname = "original-owner", "original-group"
                header.size = len(name)
                archive.addfile(header, io.BytesIO(name.encode()))
        subprocess.run([binary, path], cwd=ROOT, check=True, timeout=90)
        with tarfile.open(path, "r:gz") as archive:
            assert sorted(archive.getnames()) == ["keep", "new-name", "shared"]
            directory = archive.getmember("shared")
            assert directory.isdir() and directory.mode == 0o1777
            assert directory.pax_headers["ctime"] == "1710000000.123456789"
            assert "atime" not in directory.pax_headers
            renamed = archive.getmember("new-name")
            assert all(key not in renamed.pax_headers for key in ("atime", "ctime", "LIBARCHIVE.creationtime"))
            assert directory.mtime == 1700000000.125
            assert archive.extractfile("new-name").read() == b"old-name"
            member = archive.getmember("keep")
            assert (member.uid, member.gid) == (1001, 2002)
            assert (member.uname, member.gname) == ("original-owner", "original-group")
            assert member.pax_headers["LIBARCHIVE.creationtime"] == "1690000000.987654321"
            assert member.pax_headers["atime"] == "1710000001.987654321"
            assert member.pax_headers["ctime"] == "1710000002.123456789"
            assert member.mode == 0o4750 and member.pax_headers["mtime"] == "1700000001.375123456"
            assert archive.extractfile(member).read() == b"keep"

        with tarfile.open(path, "w:gz") as archive:
            header = tarfile.TarInfo("old-name")
            header.pax_headers["SCHILY.xattr.user.provenance"] = "preserve-me"
            archive.addfile(header)
        original = path.read_bytes()
        subprocess.run([binary, path, "unsupported"], cwd=ROOT, check=True, timeout=90)
        assert path.read_bytes() == original


def run_tar_move_ownership(binary):
    # Python supplies and verifies headers independently of Listi/libarchive.
    # Directories are explicit; each entry has a distinct uid/gid and names.
    source = {"box": (1, None), "box/tree": (2, None), "box/tree/file": (3, b"data"),
              "loose": (4, b"loose"), "keep": (5, b"keep")}
    target = {"recv": (6, None), "keep": (7, b"target")}
    moved = {**target, "recv/tree": source["box/tree"],
             "recv/tree/file": source["box/tree/file"], "recv/loose": source["loose"]}

    def write(path, entries, *, local_mtime=None, sticky=None):
        with tarfile.open(path, "w:gz" if path.suffix == ".gz" else "w") as archive:
            for name, (owner, data) in entries.items():
                header = tarfile.TarInfo(name)
                header.uid, header.gid = 1000 + owner, 2000 + owner
                header.uname, header.gname = f"owner{owner}", f"group{owner}"
                header.type = tarfile.DIRTYPE if data is None else tarfile.REGTYPE
                header.mode = 0o755 if data is None else 0o644
                if name == "recv/local-item":
                    # Damage only numeric ownership, not content, names, mode or time.
                    header.uname = header.gname = ""
                    header.mode = 0o640
                    if local_mtime is not None:
                        header.pax_headers["mtime"] = local_mtime
                if name == sticky:
                    header.mode = 0o1777
                header.size = len(data) if data is not None else 0
                archive.addfile(header, io.BytesIO(data) if data is not None else None)

    def verify(path, expected):
        with tarfile.open(path) as archive:
            assert sorted(archive.getnames()) == sorted(expected)
            for name, (owner, data) in expected.items():
                member = archive.getmember(name)
                assert (member.uid, member.gid) == (1000 + owner, 2000 + owner)
                assert (member.uname, member.gname) == (f"owner{owner}", f"group{owner}")
                assert member.isdir() if data is None else archive.extractfile(member).read() == data

    for damage in (False, True):
        with tempfile.TemporaryDirectory(prefix="listi-tar-move-") as folder:
            src = Path(folder) / "source.tar.gz"
            dst = Path(folder) / "target.tar"
            write(src, source)
            write(dst, target)
            command = [binary, src, dst]
            original = src.read_bytes()
            if damage:
                # Same paths and content; only one published ownership record differs.
                different = {**moved, "recv/tree/file": (9, b"data")}
                substitute = Path(folder) / "changed.tar"
                write(substitute, different)
                command.append(substitute)
            subprocess.run(command, cwd=ROOT, check=True, timeout=90)
            if damage:
                assert src.read_bytes() == original
                verify(dst, different)
            else:
                verify(dst, moved)
                verify(src, {"box": source["box"], "keep": source["keep"]})

    for damage in (False, True):
        with tempfile.TemporaryDirectory(prefix="listi-local-tar-owner-") as folder:
            src = Path(folder) / "local-item"
            src.write_bytes(b"original bytes")
            os.chown(src, 1003, 2003)
            os.chmod(src, 0o640)
            timestamp = 1700000000125000000
            os.utime(src, ns=(timestamp, timestamp))
            # BFS can quantize the requested time. Match the actual source, not the request.
            seconds, nanos = divmod(src.stat().st_mtime_ns, 10**9)
            observed = f"{seconds}.{nanos:09d}"
            assert (src.stat().st_uid, src.stat().st_gid) == (1003, 2003)
            dst = Path(folder) / "target.tar"
            write(dst, target)
            command = [binary, src, dst]
            if damage:
                substitute = Path(folder) / "changed.tar"
                write(substitute, {**target, "recv/local-item": (9, b"original bytes")}, local_mtime=observed)
                command.append(substitute)
            subprocess.run(command, cwd=ROOT, check=True, timeout=90)
            with tarfile.open(dst) as archive:
                header = archive.getmember("recv/local-item")
                assert (header.uid, header.gid) == ((1009, 2009) if damage else (1003, 2003))
                assert archive.extractfile(header).read() == b"original bytes"
                from decimal import Decimal
                assert header.mode == 0o640
                assert Decimal(header.pax_headers.get("mtime", str(header.mtime))) == Decimal(observed)
            if damage:
                assert src.read_bytes() == b"original bytes"
                assert (src.stat().st_uid, src.stat().st_gid) == (1003, 2003)
            else:
                assert not src.exists()

    for local_source in (False, True):
        with tempfile.TemporaryDirectory(prefix="listi-tar-directory-conflict-") as folder:
            src, dst = Path(folder) / "source.tar", Path(folder) / "target.tar"
            write(src, source)
            original = src.read_bytes()
            dest_tree = {**target, "recv/tree": (8, None), "recv/tree/old": (8, b"untouched")}
            write(dst, dest_tree, sticky="recv/tree")
            prior_target = dst.read_bytes()
            local_dir = Path(folder) / "tree"
            local_dir.mkdir()
            (local_dir / "new").write_bytes(b"new")
            subprocess.run([binary, local_dir if local_source else src, dst, "directory-conflict"],
                           cwd=ROOT, check=True, timeout=90)
            assert src.read_bytes() == original and (local_dir / "new").read_bytes() == b"new"
            assert dst.read_bytes() == prior_target

    with tempfile.TemporaryDirectory(prefix="listi-tar-directory-compatible-") as folder:
        src, dst = Path(folder) / "source.tar", Path(folder) / "target.tar"
        write(src, source)
        old_child = {"recv/tree/old": (8, b"untouched")}
        write(dst, {**target, "recv/tree": source["box/tree"], **old_child})
        subprocess.run([binary, src, dst], cwd=ROOT, check=True, timeout=90)
        verify(dst, {**moved, **old_child})
        verify(src, {"box": source["box"], "keep": source["keep"]})

    with tempfile.TemporaryDirectory(prefix="listi-tar-portable-") as folder:
        from zipfile import ZipFile
        src, dst, local = (Path(folder) / leaf for leaf in ("source.tar", "target.zip", "local"))
        write(src, source)
        original = src.read_bytes()
        local.mkdir()
        with ZipFile(dst, "w") as archive:
            archive.writestr("recv/", "")
        prior_zip = dst.read_bytes()
        for target_path in (local, dst):
            subprocess.run([binary, src, target_path, "unsupported"], cwd=ROOT, check=True, timeout=90)
            assert src.read_bytes() == original and dst.read_bytes() == prior_zip
            assert list(local.iterdir()) == []
        # An otherwise portable tree with just a recorded PAX access time
        # cannot be destructively converted using the generic local/ZIP path.
        with tarfile.open(src, "w") as archive:
            for name in ("box/tree", "loose"):
                header = tarfile.TarInfo(name)
                if name == "box/tree":
                    header.type, header.mode = tarfile.DIRTYPE, 0o755
                else:
                    header.pax_headers["atime"] = "1710000001.123456789"
                archive.addfile(header)
        original = src.read_bytes()
        for target_path in (local, dst):
            subprocess.run([binary, src, target_path, "extra-time"], cwd=ROOT, check=True, timeout=90)
            assert src.read_bytes() == original and dst.read_bytes() == prior_zip
            assert list(local.iterdir()) == []
        # ZIP stores full seconds here; keep the source TAR when a selected file
        # has higher precision, even though names, permissions and bytes all match.
        with tarfile.open(src, "w") as archive:
            for name, data in (("box/tree", None), ("box/tree/file", b"data"), ("loose", b"loose")):
                header = tarfile.TarInfo(name)
                header.type = tarfile.DIRTYPE if data is None else tarfile.REGTYPE
                header.mode = 0o755 if data is None else 0o644
                header.mtime = 1700000000.125 if name == "loose" else 1700000000
                header.size = len(data) if data else 0
                archive.addfile(header, io.BytesIO(data) if data else None)
        original = src.read_bytes()
        subprocess.run([binary, src, dst, "time-mismatch"], cwd=ROOT, check=True, timeout=90)
        assert src.read_bytes() == original
        prior_zip = dst.read_bytes()  # The verified content copy may remain after refusal.
        local_file = Path(folder) / "local-item"
        local_file.write_bytes(b"original bytes")
        os.chown(local_file, 1003, 2003)
        subprocess.run([binary, local_file, dst, "unsupported"], cwd=ROOT, check=True, timeout=90)
        assert local_file.read_bytes() == b"original bytes" and dst.read_bytes() == prior_zip


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
        if name in ("archive-copy-inputs", "file-operation-faults") and os.getenv("LISTI_QUALIFICATION_LINK_IMAGE"):
            run_hardlink_case(binary, name, os.environ["LISTI_QUALIFICATION_LINK_IMAGE"])
        elif name == "disk-actions" and not os.environ.get("LISTI_TEST_DISK_DEVICE"):
            run_disk_actions(binary)
        elif name == "tar-edit-ownership":
            run_tar_edit_ownership(binary)
        elif name == "tar-move-ownership":
            run_tar_move_ownership(binary)
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
