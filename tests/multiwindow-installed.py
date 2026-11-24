#!/usr/bin/env python3
"""Exercise the installed Haiku app. Run in a private guest with Listi closed."""
from collections import Counter
from contextlib import ExitStack
from pathlib import Path
import subprocess
import tempfile
import time

APP = "/boot/system/apps/Listi"
SIGNATURE = "application/x-vnd.Kambuzi-Lister"


def hey(*args):
    return subprocess.check_output(
        ["hey", "-o", SIGNATURE, *map(str, args)], text=True,
        timeout=4, stderr=subprocess.DEVNULL).strip()


def windows():
    result = {}
    # hey can report a successful but empty count during roster startup/shutdown.
    for index in range(int(hey("count", "Window") or 0)):
        try:
            result[index] = hey("get", "SourcePath", "of", "Window", index)
        except subprocess.CalledProcessError:
            pass  # Auxiliary windows do not implement SourcePath.
    return result


def wait_for(f, label, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if f():
                print("PASS", label, flush=True)
                return
        except subprocess.CalledProcessError:
            pass
        time.sleep(0.1)
    raise RuntimeError(label)


def menu(index, name, *parents):
    args = ["do", "MenuItem", name]
    for parent in parents or ("Files",):
        args += ["of", "Menu", parent]
    hey(*args, "of", "MenuBar", "of", "Window", index)


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def run(root, cleanup):
    def launch(*args, cwd=None):
        process = subprocess.Popen([APP, *map(str, args)], cwd=cwd, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cleanup.callback(stop, process)
        return process

    first, second = map(str, (root / "alpha", root / "beta"))
    Path(first).mkdir()
    Path(second).mkdir()
    app = launch("--new-window", "alpha", cwd=root)

    def paths(*expected):
        wait_for(lambda: Counter(windows().values()) == Counter(expected),
                 "browser folders: " + ", ".join(expected))

    def index(path):
        return next(i for i, value in windows().items() if value == path)

    paths(first)
    menu(index(first), "New window")
    paths(first, first)
    menu(index(first), "New window")
    paths(first, first, first)
    hey("set", "SourcePath", "of", "Window", index(first), "to", second)
    paths(first, first, second)
    menu(index(second), "Detach to Einn window", "Folder tabs", "Window")
    paths(first, first, second, "/boot/home")
    menu(index("/boot/home"), "Close window")
    paths(first, first, second)
    launch("--new-window", "beta", cwd=root)
    paths(first, first, second, second)
    subprocess.run(["open", APP], check=True, timeout=10)
    wait_for(lambda: len(windows()) == 5, "Tracker roster relaunch adds one browser")
    hey("set", "Active", "of", "Window", index(first), "to", "true")
    assert len(windows()) == 5
    print("PASS activation did not open another browser")
    menu(index(first), "Quit Listi")
    assert app.wait(timeout=10) == 0

    app = launch()
    wait_for(lambda: len(windows()) == 1, "cold default opens only one browser")
    menu(next(iter(windows())), "Close window")
    assert app.wait(timeout=10) == 0
    print("PASS closing the last browser exits Listi")


if __name__ == "__main__":
    print("TEST installed multiwindow", flush=True)
    with tempfile.TemporaryDirectory(prefix="listi-multi-", dir="/boot/home") as folder:
        with ExitStack() as cleanup:
            run(Path(folder), cleanup)
    print("PASS installed multiwindow", flush=True)
