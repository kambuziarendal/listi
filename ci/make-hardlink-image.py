#!/usr/bin/env python3
"""Build a disposable ext2 test image without root or a host mount."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def create(image):
    with tempfile.TemporaryDirectory(prefix="listi-link-seed-") as directory:
        seed = Path(directory)
        aliases = seed / "archive-aliases"
        aliases.mkdir()
        for original, alias in [("original.zip", "other.zip"), ("plain.7z.002", "payload.bin")]:
            (aliases / original).touch()
            os.link(aliases / original, aliases / alias)
        for scenario in ["complete", "cancel", "replace", "edit"]:
            base = seed / "deletion" / scenario
            (base / "selected").mkdir(parents=True)
            outside = base / "outside"
            outside.write_bytes(b"shared bytes")
            for leaf in ["a", "z"]:
                os.link(outside, base / "selected" / leaf)
        move = seed / "move-pair"
        move.mkdir()
        (move / "entry").write_bytes(b"source bytes")
        os.link(move / "entry", move / "sibling")
        with image.open("xb") as disk:
            disk.truncate(32 * 1024 * 1024)
        tool = shutil.which("mke2fs") or "/usr/sbin/mke2fs"
        subprocess.run([tool, "-q", "-F", "-t", "ext2", "-L", "ListiHardlinks",
                        "-d", seed, image], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="new image path")
    create(parser.parse_args().image)
