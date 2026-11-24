"""Manual, whole-file version history. See README.md for format and limits."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import stat
import subprocess
import sys
import tempfile
import time


def attrs(path, payload=None):
    if not sys.platform.startswith("haiku"):
        if payload:
            raise ValueError("restoring typed attributes requires Haiku")
        return ""
    helper = Path(__file__).resolve().with_name("bfs-attrs")
    result = subprocess.run([helper, "save" if payload is None else "load", path],
                            input=bytes.fromhex(payload or ""), stdout=subprocess.PIPE,
                            check=True)
    return result.stdout.hex()


def record(db, path, root, entries):
    before = path.lstat()
    mode = before.st_mode
    row = dict(path=str(path.relative_to(root)), mode=mode, mtime=before.st_mtime_ns,
               attrs=attrs(path))
    entries.append(row)
    if stat.S_ISLNK(mode):
        row["target"] = os.readlink(path)
    elif stat.S_ISREG(mode):
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if not db.execute("SELECT 1 FROM objects WHERE hash=?", (digest,)).fetchone():
            db.execute("INSERT INTO objects VALUES (?,?)", (digest, data))
        row["hash"] = digest
    elif stat.S_ISDIR(mode):
        for child in sorted(path.iterdir()):
            record(db, child, root, entries)
    else:
        raise ValueError(f"unsupported entry: {path}")
    after = path.lstat()
    if (before.st_ino, before.st_ctime_ns, before.st_size, before.st_mtime_ns) != (
            after.st_ino, after.st_ctime_ns, after.st_size, after.st_mtime_ns):
        raise ValueError(f"source changed: {path}")


def separate(a, b):
    if a == b or a in b.parents or b in a.parents:
        raise ValueError("source, store and destination must be separate")


def save(store, root):
    separate(store, root)
    if not root.is_dir():
        raise ValueError("source must be a directory")
    with sqlite3.connect(store) as db:
        db.execute("BEGIN IMMEDIATE")
        db.execute("CREATE TABLE IF NOT EXISTS config(root TEXT)")
        db.execute("CREATE TABLE IF NOT EXISTS objects(hash TEXT PRIMARY KEY, data BLOB NOT NULL)")
        db.execute("CREATE TABLE IF NOT EXISTS points(id INTEGER PRIMARY KEY, created INTEGER, manifest TEXT)")
        configured = db.execute("SELECT root FROM config").fetchone()
        if configured is None:
            db.execute("INSERT INTO config VALUES (?)", (str(root),))
        elif configured != (str(root),):
            raise ValueError("store belongs to another source")
        entries = []
        record(db, root, root, entries)
        point = db.execute("INSERT INTO points(created,manifest) VALUES (?,?)",
                           (int(time.time()), json.dumps(entries))).lastrowid
    print(point)


def restore(db, rows, destination):
    # A fresh private directory, never an existing tree. Validate ancestors
    # against directories already created, not paths resolved through links.
    if os.path.lexists(destination):
        raise FileExistsError(destination)
    with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
        root = Path(temporary) / "copy"
        directories = set()
        for row in rows:
            rel = Path(row["path"])
            if rel.is_absolute() or ".." in rel.parts or (rel.parts and rel.parent not in directories):
                raise ValueError("invalid manifest hierarchy")
            path = root / rel
            mode = row["mode"]
            if stat.S_ISDIR(mode):
                path.mkdir()
                directories.add(rel)
            elif stat.S_ISLNK(mode):
                path.symlink_to(row["target"])
            elif stat.S_ISREG(mode):
                data, = db.execute("SELECT data FROM objects WHERE hash=?", (row["hash"],)).fetchone()
                if hashlib.sha256(data).hexdigest() != row["hash"]:
                    raise ValueError("damaged content object")
                with path.open("xb") as file:
                    file.write(data)
            else:
                raise ValueError("unknown entry type")
        for row in reversed(rows):
            path = root / row["path"]
            attrs(path, row["attrs"])
            if not path.is_symlink():
                os.chmod(path, stat.S_IMODE(row["mode"]) & 0o777)
                os.utime(path, ns=(row["mtime"], row["mtime"]))
        # Reserve the name exclusively; rename replaces only our empty directory.
        destination.mkdir(mode=0o700)
        try:
            root.rename(destination)
        except OSError:
            destination.rmdir()
            raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("store", type=Path)
    commands = parser.add_subparsers(dest="action", required=True)
    commands.add_parser("save").add_argument("source", type=Path)
    commands.add_parser("points")
    for name in ("browse", "restore"):
        command = commands.add_parser(name)
        command.add_argument("point", type=int)
        if name == "restore":
            command.add_argument("destination", type=Path)
    args = parser.parse_args()
    store = args.store.resolve()
    if args.action == "save":
        save(store, args.source.resolve())
        return
    with sqlite3.connect(store.as_uri() + "?mode=ro", uri=True) as db:
        if args.action == "points":
            for row in db.execute("SELECT id,created FROM points ORDER BY id"):
                print(*row)
            return
        point = db.execute("SELECT manifest FROM points WHERE id=?", (args.point,)).fetchone()
        if not point:
            raise ValueError("point not found")
        rows = json.loads(point[0])
        if args.action == "browse":
            print(json.dumps(rows, indent=2))
        else:
            destination = args.destination.absolute()
            separate(destination.resolve(), store)
            separate(destination.resolve(), Path(db.execute("SELECT root FROM config").fetchone()[0]))
            restore(db, rows, destination)


if __name__ == "__main__":
    main()
