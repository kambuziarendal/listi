"""Create disposable inputs matching the installed VirtualTest contract."""
import bz2
import gzip
import io
import lzma
import stat
import sys
import tarfile
import zipfile
from pathlib import Path

root = Path(sys.argv[1])
root.mkdir()  # Never overwrite an existing fixture root.
hello = b"archive content\n"
with zipfile.ZipFile(root / "sample.zip", "w") as archive:
    archive.writestr("folder/hello world.txt", hello)
    archive.writestr("large.bin", bytes(512 * 1024))
for suffix in ("gz", "xz", "bz2"):
    with tarfile.open(root / ("sample.tar." + suffix), "w:" + suffix) as archive:
        entry = tarfile.TarInfo("folder/hello world.txt")
        entry.size = len(hello)
        archive.addfile(entry, io.BytesIO(hello))
for suffix, compress in (("gz", gzip.compress), ("xz", lzma.compress), ("bz2", bz2.compress)):
    (root / ("single.txt." + suffix)).write_bytes(compress(b"single content\n"))
with tarfile.open(root / "metadata.tar", "w") as archive:
    directory = tarfile.TarInfo("explicit")
    directory.type, directory.mode, directory.mtime = tarfile.DIRTYPE, 0o750, 1600000000
    archive.addfile(directory)
    file = tarfile.TarInfo("explicit/run.sh")
    file.mode, file.mtime = stat.S_ISUID | 0o751, 1600000020
    file.size = 1
    archive.addfile(file, io.BytesIO(b"x"))
with zipfile.ZipFile(root / "unsafe.zip", "w") as archive:
    archive.writestr("../outside", b"invalid archive member")
with tarfile.open(root / "links.tar", "w") as archive:
    link = tarfile.TarInfo("escape")
    link.type, link.linkname = tarfile.SYMTYPE, "../outside"
    archive.addfile(link)
