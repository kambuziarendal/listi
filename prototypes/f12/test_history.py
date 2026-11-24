import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest

import history


class HistoryTest(unittest.TestCase):
    def test_points(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            src = base / "src"
            store = base / "store"
            src.mkdir()
            (src / "file").write_bytes(b"original")
            (src / "link").symlink_to("file")
            history.save(store, src)
            (src / "file").rename(src / "renamed")
            os.chmod(src / "renamed", 0o600)
            history.save(store, src)
            with sqlite3.connect(store) as db:
                self.assertEqual(db.execute("select count(*) from objects").fetchone(), (1,))
            (src / "renamed").write_bytes(b"modified")
            history.save(store, src)
            (src / "renamed").unlink()
            history.save(store, src)
            # Kill the writer after it inserted an object but before manifest commit.
            (src / "extra").write_bytes(b"uncommitted")
            code = """import history,os,sys
from pathlib import Path
record = history.record
def interrupted(db,path,root,entries):
    record(db,path,root,entries)
    if path.name == 'extra': os._exit(77)
history.record = interrupted
history.save(Path(sys.argv[1]), Path(sys.argv[2]))
"""
            result = subprocess.run([sys.executable, "-c", code, store, src],
                                    cwd=Path(__file__).parent)
            self.assertEqual(result.returncode, 77)
            with sqlite3.connect(store) as db:
                self.assertEqual(db.execute("select count(*) from objects").fetchone(), (2,))
                self.assertEqual(db.execute("select count(*) from points").fetchone(), (4,))
            cli = [sys.executable, Path(history.__file__), store]
            subprocess.run(cli + ["restore", "1", base / "out"], check=True)
            self.assertEqual((base / "out/file").read_bytes(), b"original")
            self.assertEqual(os.readlink(base / "out/link"), "file")
            self.assertNotEqual(subprocess.run(cli + ["restore", "1", base / "out"],
                                               stderr=subprocess.PIPE).returncode, 0)


if __name__ == "__main__":
    unittest.main()
