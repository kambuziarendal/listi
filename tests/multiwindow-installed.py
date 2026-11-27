#!/usr/bin/env python3
"""Exercise the installed Haiku app. Run in a private guest with Listi closed."""
from collections import Counter
from contextlib import ExitStack
from pathlib import Path
import subprocess
import tempfile
import time
import zipfile

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
    helper = Path(__file__).resolve().parents[1] / "build/native-audit-tests/file-clipboard"
    for route in ("Cut", "mixed Tracker", "archive"):
        base = root / route
        inputs = base / "inputs"
        inputs.mkdir(parents=True)
        source = inputs / "source.txt"
        source.write_bytes(b"installed Paste fixture")
        extra = inputs / "copied.txt"
        archive = base / "selection.zip"
        shown, held = base / "mount/shown", base / "held/shown"
        shown.mkdir(parents=True)
        browser = index(first)
        if route == "archive":
            with zipfile.ZipFile(archive, "w") as container:
                container.writestr(source.name, source.read_bytes())
            initial = "archive://" + str(archive) + "!/"
        else:
            initial = str(inputs)
        hey("set", "SourcePath", "of", "Window", browser, "to", initial)
        paths(initial)

        def ready():
            return hey("get", "PathReady", "of", "Window", browser).lower() == "true"

        wait_for(ready, "installed " + route + " source loaded")
        if route == "mixed Tracker":
            extra.write_bytes(b"retained Tracker copy")
            subprocess.run([helper, "mixed", source, extra], check=True, timeout=10)
        else:
            menu(browser, "All", "Select")
            menu(browser, "Copy" if route == "archive" else "Cut")
        hey("set", "SourcePath", "of", "Window", browser, "to", shown)
        paths(str(shown))
        wait_for(ready, "installed " + route + " destination loaded")
        # Rename an ancestor, not the watched directory itself. Its node still
        # exists unchanged; the visible path now traverses a different parent.
        shown.parent.rename(held.parent)
        shown.mkdir(parents=True)
        menu(browser, "Paste into active folder")

        def refusal():
            for i in range(int(hey("count", "Window") or 0)):
                try:
                    label = hey("get", "Text", "of", "View", "operation error", "of", "Window", i)
                    if "destination folder was replaced" in label and str(shown) in label:
                        return True
                except subprocess.CalledProcessError:
                    pass
            return False

        wait_for(refusal, "installed " + route + " Paste reports replaced destination")
        assert source.read_bytes() == b"installed Paste fixture"
        assert not any(shown.iterdir()) and not any(held.iterdir())
        if route == "mixed Tracker":
            assert extra.read_bytes() == b"retained Tracker copy"
        if route == "archive":
            with zipfile.ZipFile(archive) as container:
                assert container.read(source.name) == b"installed Paste fixture"
        browser = index(str(shown))
        menu(browser, "Refresh", "Listi")
        wait_for(ready, "installed replacement folder reloaded")
        menu(browser, "Paste into active folder")

        def delivered():
            if not (shown / source.name).exists():
                return False
            if route != "archive" and source.exists():
                return False
            return route != "mixed Tracker" or (shown / extra.name).exists()

        wait_for(delivered, "installed " + route + " Paste retries into refreshed folder")
        assert (shown / source.name).read_bytes() == b"installed Paste fixture" and not any(held.iterdir())
        if route == "mixed Tracker":
            assert extra.read_bytes() == (shown / extra.name).read_bytes() == b"retained Tracker copy"
        if route == "archive":
            with zipfile.ZipFile(archive) as container:
                assert container.read(source.name) == b"installed Paste fixture"
        # Drop earlier operation messages and unread errors before the next case.
        wait_for(lambda: hey("get", "Busy", "of", "Window", index(str(shown))) == "false",
                 "installed " + route + " Paste finished")
        menu(index(str(shown)), "Quit Listi")
        assert app.wait(timeout=10) == 0
        app = launch("--new-window", first)
        paths(first)
    menu(index(first), "New window")
    paths(first, first)
    menu(index(first), "New window")
    paths(first, first, first)
    hey("set", "SourcePath", "of", "Window", index(first), "to", second)
    paths(first, first, second)
    menu(index(second), "Detach tab to Einn window", "Window")
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
