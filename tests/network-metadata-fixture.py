#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Loopback FTP and SFTP fixtures for network-metadata.cpp.

Run with pyftpdlib and paramiko installed, passing an empty fixture directory.
For Haiku guests, forward ports 2127, 2227, 31270 to the host loopback.
The generated known_hosts file is staging data, not a real server's identity.
"""

import argparse
import os
from pathlib import Path
import re
import socketserver
import stat
import threading

import paramiko
from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.filesystems import AbstractedFS
from pyftpdlib.handlers import DTPHandler, FTPHandler
from pyftpdlib.servers import FTPServer

ADDRESS = "127.0.0.1"
USER = "listitest"
PASSWORD = "fixture-only"
MODIFIED = 1709209800
FACTS = {
    "epoch": "modify=19700101000000;",
    "unknown": "",
    "wrong-day": "modify=20230229123000;",
    "wrong-hour": "modify=20240229243000;",
    "short": "modify=202402291230;",
    "suffix": "modify=20240229123000Z;",
    "bare-dot": "modify=20240229123000.;",
    "bad-fraction": "modify=20240229123000.1x;",
    "duplicates": "modify=20240229123000;modify=20240229123001;",
    "stable": "MoDiFy=20240229123000.123456789000;",
}
BEFORE = "modify=20240229123000.1234567891;"
AFTER = "modify=20240229123000.1234567892;"


LEGACY = {"list-only", "hidden-only", "mixed-nested", "guarded-rename"}
UPLOADS = {"upload-uncertain", "upload-cancel", "upload-mlsd", "cancel-mlsd", "upload-cleanup"}
FILTERED = {"mlsd-hidden", "mlsd-mixed", "upload-mlsd"}


def count_probe(operation, path):
    # Count requests, not wall time. Keep the comparison and download trees
    # separate so their traffic is identifiable from the host-side log.
    folder = next((part for part in Path(path).parts if part.startswith("scaling-")), None)
    if folder:
        print(f"PROBE {folder} {operation}", flush=True)


def make_scaling_trees(root):
    for n in (4, 8, 16, 32, 64):
        for action in ("compare", "download"):
            folder = root / f"scaling-{action}-{n}"
            folder.mkdir()
            for i in range(n):
                (folder / str(i)).write_text(str(i))


class MetadataFS(AbstractedFS):
    def ftp2fs(self, path):
        resolved = Path(super().ftp2fs(path))
        if resolved.parent.name == "rename-insensitive" and resolved.name == "report":
            return str(resolved.with_name("Report"))
        return str(resolved)

    def format_list(self, basedir, listing, *args, **kwargs):
        if Path(basedir).name in LEGACY | UPLOADS:
            listing = [name for name in listing if not name.startswith(".")]
        return super().format_list(basedir, listing, *args, **kwargs)

    def format_mlsx(self, basedir, listing, perms, facts, ignore_err=True):
        if ignore_err and Path(basedir).name in FILTERED:
            listing = [name for name in listing if not name.startswith(".")]
        for row in super().format_mlsx(basedir, listing, perms, facts, ignore_err):
            attrs, name = row.split(b" ", 1)
            leaf = name.rstrip().decode()
            if leaf in {"missing-mode", "private-memo"}:
                attrs = re.sub(rb"unix\.mode=[^;]*;", b"", attrs, flags=re.I)
            if leaf == "reported-mode":
                attrs = re.sub(rb"unix\.mode=[^;]*;", b"", attrs, flags=re.I) + b"unix.mode=0640;"
            replacement = FACTS.get(leaf)
            if leaf == "changed":
                mtime = (Path(basedir) / leaf).stat().st_mtime_ns
                replacement = AFTER if mtime // 1_000_000_000 > MODIFIED else BEFORE
            if replacement is not None:
                attrs = re.sub(rb"modify=[^;]*;", b"", attrs) + replacement.encode()
            yield attrs + b" " + name


class MetadataDTP(DTPHandler):
    def send(self, data):
        count = super().send(data)
        if not count or self.cmd != "RETR":
            return count
        path = Path(self.file_obj.name)
        if path.name != "changed":
            return count
        if path.stat().st_mtime_ns // 1_000_000_000 == MODIFIED:
            with path.open("r+b") as file:
                file.seek(-1, os.SEEK_END)
                file.write(b"x")
            os.utime(path, (MODIFIED + 1, MODIFIED + 1))
            print("Changed one source byte after FTP sent data; reported Modify differs only below 1 ns.", flush=True)
        return count


class MetadataFTP(FTPHandler):
    abstracted_fs = MetadataFS
    dtp_handler = MetadataDTP
    passive_ports = [31270]
    use_sendfile = False

    def pre_process_command(self, line, cmd, arg):
        if cmd in {"RNFR", "RNTO"} and self.authenticated:
            # Log even an attempted rename, including paths relative to login.
            (Path(self.fs.root) / "rename-command-seen").touch()
            self.respond("550 Rename disabled by the test fixture.")
            return
        # This server shows dotfiles in list-all only for an explicit LIST -a.
        if cmd == "LIST" and self.fs.cwd == "/list-all" and arg != "-a":
            self.respond("501 Use LIST -a.")
            return
        return super().pre_process_command(line, cmd, arg)

    def ftp_MLSD(self, path):
        count_probe("FTP-MLSD", path)
        folder = Path(path)
        if (folder.name in LEGACY | {"list-all"} or
                (folder.name == "upload-uncertain" and any(folder.glob("*.part"))) or
                (folder.name == "upload-cancel" and (self.cancel_signal.parent / folder.name).exists())):
            self.respond("502 MLSD unavailable.")
            return
        return super().ftp_MLSD(path)

    def ftp_MLST(self, path):
        count_probe("FTP-MLST", path)
        if Path(path).parent.name == "scaling-compare-4":
            self.respond("502 MLST unavailable.")
            return
        file = Path(path)
        if (file.name == ".settings" and file.parent.name in LEGACY or
                file.suffix == ".part" and file.exists() and
                file.parent.name in {"upload-uncertain", "upload-mlsd"}):
            self.respond("550 Entry unavailable.")
            return
        return super().ftp_MLST(path)

    def on_file_sent(self, filename):
        # Small transfers can finish before pyftpdlib assigns DTPHandler.cmd.
        # This callback runs before servicing the client's next stat/list.
        path = Path(filename)
        if path.suffix == ".part" and path.parent.name == "upload-cleanup":
            (path.parent / "upload.txt").write_bytes(b"other client")
        if path.name == "sample" and path.parent.name in {"identity-compare", "identity-copy"}:
            replacement = path.with_suffix(".next")
            if path.read_bytes() == b"aaaa":
                parent = path.parent.stat()
                replacement.write_bytes(b"bbbb")
                os.utime(replacement, (MODIFIED, MODIFIED))
                replacement.replace(path)  # New inode, same size and Modify.
                os.utime(path.parent, ns=(parent.st_atime_ns, parent.st_mtime_ns))

    def ftp_RETR(self, path):
        file = Path(path)
        if file.parent.name in {"upload-cancel", "cancel-mlsd"} and file.suffix == ".part":
            # Only reached when the client verifies a STOR acknowledged earlier.
            (self.cancel_signal.parent / file.parent.name).touch()
            if file.parent.name == "cancel-mlsd":
                FILTERED.add(file.parent.name)
        return super().ftp_RETR(path)


class Auth(paramiko.ServerInterface):
    def __init__(self, client_key):
        self.client_key = client_key

    def get_allowed_auths(self, username):
        if username == "agent-password":
            return "publickey,password"
        return "publickey" if username == "key-test" else "password"

    def check_auth_publickey(self, username, key):
        return (paramiko.AUTH_SUCCESSFUL if username == "key-test" and key == self.client_key
                else paramiko.AUTH_FAILED)

    def check_auth_password(self, username, password):
        return (paramiko.AUTH_SUCCESSFUL if username in (USER, "agent-password") and password == PASSWORD
                else paramiko.AUTH_FAILED)

    def check_channel_request(self, kind, ident):
        return (paramiko.OPEN_SUCCEEDED if kind == "session"
                else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED)


class ListAttributes(paramiko.SFTPAttributes):
    def __str__(self):
        # Use an OpenSSH-style long name, with the date deliberately unrelated
        # to the precise timestamp returned by STAT.
        mode = stat.filemode(self.st_mode)
        return f"{mode} 1 {self.st_uid} {self.st_gid} {self.st_size} Jan 2 2000 {self.filename}"


class MetadataSFTP(paramiko.SFTPServerInterface):
    def __init__(self, server, root):
        super().__init__(server)
        self.root = root
        self.size_listed = set()

    def resolve(self, path, *, follow=True):
        candidate = self.root / path.lstrip("/")
        if follow or candidate == self.root:
            candidate = candidate.resolve()
        else:
            candidate = candidate.parent.resolve() / candidate.name
        if candidate.parent.name == "aliases" and candidate.name == "report":
            candidate = candidate.with_name("Report")
        if candidate.name == ".." or not candidate.is_relative_to(self.root):
            raise PermissionError(13, "Outside fixture")
        return candidate

    def list_folder(self, path):
        count_probe("SFTP-READDIR", path)
        try:
            if path in {"/compare-size-quick", "/compare-size-hash"}:
                sample = self.resolve(path) / "sample"
                final = b"abcde" if path.endswith("-quick") else b"abc"
                if path in self.size_listed and sample.read_bytes() != final:
                    sample.write_bytes(final)
                self.size_listed.add(path)
            entries = []
            for child in self.resolve(path).iterdir():
                entry = ListAttributes.from_stat(child.lstat(), child.name)
                if path in {"/compare", "/compare-changing", "/compare-short"}:
                    entry.st_size = None
                if path == "/compare-late":
                    entry.st_size = entry.st_atime = entry.st_mtime = None
                if path == "/compare-stale":
                    entry.st_mtime -= 1
                entries.append(entry)
            return entries
        except OSError as error:
            return paramiko.SFTPServer.convert_errno(error.errno)

    def stat(self, path):
        return self.attributes(path, follow=True)

    def lstat(self, path):
        return self.attributes(path, follow=False)

    def attributes(self, path, *, follow):
        count_probe("SFTP-STAT" if follow else "SFTP-LSTAT", path)
        try:
            local = self.resolve(path, follow=follow)
            item = paramiko.SFTPAttributes.from_stat(local.stat(follow_symlinks=follow))
            if path == "/compare-short/known":
                item.st_size += 1
            if local.name == "unknown":
                item.st_mtime = None
                item.st_atime = None
            return item
        except OSError as error:
            return paramiko.SFTPServer.convert_errno(error.errno)

    def rename(self, oldpath, newpath):
        old, new = self.resolve(oldpath), self.resolve(newpath)
        control = self.root / "rename-control"
        if old.parent == new.parent == control and {old.name, new.name} == {"original", "renamed"}:
            try:
                if new.exists():
                    return paramiko.SFTP_FAILURE
                old.rename(new)
                return paramiko.SFTP_OK
            except OSError as error:
                return paramiko.SFTPServer.convert_errno(error.errno)
        (self.root / "sftp-rename-seen").touch()
        return paramiko.SFTP_PERMISSION_DENIED

    def posix_rename(self, oldpath, newpath):
        return self.rename(oldpath, newpath)

    def open(self, path, flags, attr):
        if flags != os.O_RDONLY:
            return paramiko.SFTP_PERMISSION_DENIED
        try:
            if path == "/compare-late/1":
                first = self.resolve("/compare-late/0")
                if first.read_bytes() == b"unchanged":
                    first.write_bytes(b"grown after hashing")
            if path == "/signal-conflict":
                with self.resolve("/conflict").open("ab") as conflict:
                    conflict.write(b"!")
            if path in {"/signal-access-existing", "/signal-access-new"}:
                self.resolve(path.removeprefix("/signal-")).chmod(0o700)
            handle = paramiko.SFTPHandle(flags)
            handle.readfile = self.resolve(path).open("rb")
            # curl may use FSTAT for size on the open handle.
            handle.stat = lambda: self.stat(path)
            if path in {"/tightens", "/tightens-installed"}:
                def read_private(offset, length):
                    handle.readfile.seek(offset)
                    data = handle.readfile.read(length)
                    if data:
                        self.resolve(path).chmod(0o600)
                    return data
                handle.read = read_private
            if path in {"/recursive/changed", "/compare-changing/known"}:
                def read(offset, length):
                    handle.readfile.seek(offset)
                    data = handle.readfile.read(length)
                    local = self.resolve(path)
                    if data and local.stat().st_mtime == MODIFIED:
                        with local.open("r+b") as file:
                            file.seek(-1, os.SEEK_END)
                            file.write(b"x")
                        os.utime(local, (MODIFIED + 1, MODIFIED + 1))
                    return data
                handle.read = read
            return handle
        except OSError as error:
            return paramiko.SFTPServer.convert_errno(error.errno)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    staging = args.directory.resolve()
    staging.mkdir()
    ftp_root = staging / "ftp"
    ftp_root.mkdir()
    (ftp_root / "missing-mode").mkdir()
    (ftp_root / "missing-mode" / "private-memo").write_bytes(b"private")
    (ftp_root / "reported-mode").write_bytes(b"explicit")
    for name in FACTS:
        (ftp_root / name).write_bytes(b"fixture contents\n")
    for directory in ("mlsd", "list-only", "list-all"):
        (ftp_root / directory).mkdir()
        (ftp_root / directory / "name").write_bytes(b"fixture contents\n")
        (ftp_root / directory / ".settings").write_bytes(b"hidden\n")
    for directory in ("identity-stable", "identity-compare", "identity-copy"):
        (ftp_root / directory).mkdir()
        sample = ftp_root / directory / "sample"
        sample.write_bytes(b"aaaa")
        os.utime(sample, (MODIFIED, MODIFIED))
    for directory in ("rename-sensitive", "rename-insensitive"):
        folder = ftp_root / directory
        folder.mkdir()
        (folder / "Report").write_bytes(b"original")
        (folder / "third").write_bytes(b"third")
    for directory in ("hidden-only", "guarded-rename", "outer/mixed-nested",
                      "mlsd-hidden", "mlsd-outer/mlsd-mixed"):
        folder = ftp_root / directory
        folder.mkdir(parents=True)
        (folder / ".settings").write_bytes(b"hidden\n")
        if directory not in {"hidden-only", "mlsd-hidden"}:
            (folder / "visible").write_bytes(b"visible\n")
    for directory in UPLOADS:
        (ftp_root / directory).mkdir()
    changed = ftp_root / "changed"
    changed.write_bytes(b"d" * (2 * 1024 * 1024))
    os.utime(changed, (MODIFIED, MODIFIED))
    (ftp_root / "recursive").mkdir()
    recursive_ftp = ftp_root / "recursive" / "changed"
    recursive_ftp.write_bytes(changed.read_bytes())
    os.utime(recursive_ftp, (MODIFIED, MODIFIED))

    sftp_root = staging / "sftp"
    (sftp_root / "signals").mkdir(parents=True)
    (sftp_root / "conflict").write_bytes(b"staged")
    (sftp_root / "signal-conflict").touch()
    scope = sftp_root / "scope"
    selected = scope / "top"
    selected.mkdir(parents=True)
    for i in range(8):
        name = f"entry-{i}"
        (selected / name).write_text(name)
    os.mkfifo(scope / "unselected-pipe")
    (scope / "link").symlink_to("top/entry-0")
    MetadataFTP.cancel_signal = sftp_root / "signals" / "checking-upload"
    private = sftp_root / "Private"
    private.mkdir(mode=0o700)
    inner = private / "inner"
    inner.mkdir(mode=0o700)
    memo = inner / "memo"
    memo.write_bytes(b"private payload")
    memo.chmod(0o644)
    (sftp_root / "rename-control").mkdir()
    (sftp_root / "rename-control" / "original").write_bytes(b"control")
    (sftp_root / "aliases").mkdir()
    (sftp_root / "aliases" / "Report").write_bytes(b"original")
    (sftp_root / "aliases" / "third").write_bytes(b"third")
    for name, initial in (("compare-size-quick", b"abc"), ("compare-size-hash", b"abcde")):
        folder = sftp_root / name
        folder.mkdir()
        (folder / "sample").write_bytes(initial)
        (folder / "steady").write_bytes(b"stable")
    late = sftp_root / "compare-late"
    late.mkdir()
    for i in range(8):
        (late / str(i)).write_bytes(b"unchanged")
    for name in ("compare", "compare-changing", "compare-stale", "compare-short"):
        (sftp_root / name).mkdir()
    for name in ("known", "unknown", "compare/known", "compare-changing/known",
                 "compare-stale/known", "compare-short/known"):
        file = sftp_root / name
        file.write_bytes(b"fixture contents\n")
        os.utime(file, (MODIFIED, MODIFIED))
    for name in ("access-existing", "access-new"):
        folder = sftp_root / name
        folder.mkdir()
        folder.chmod(0o755)
        for leaf in ("a", "b"):
            (folder / leaf).write_text(leaf)
            (folder / leaf).chmod(0o644)
        (sftp_root / ("signal-" + name)).write_text("advance")
    for name in ("tightens", "tightens-installed"):
        tightens = sftp_root / name
        tightens.write_bytes(b"private")
        tightens.chmod(0o644)
    (sftp_root / "recursive").mkdir()
    recursive_sftp = sftp_root / "recursive" / "changed"
    recursive_sftp.write_bytes(changed.read_bytes())
    os.utime(recursive_sftp, (MODIFIED, MODIFIED))
    key = paramiko.RSAKey.generate(2048)
    (staging / "known_hosts").write_text(f"[{ADDRESS}]:2227 {key.get_name()} {key.get_base64()}\n")

    wrong_key = paramiko.RSAKey.generate(2048)
    (staging / "wrong_known_hosts").write_text(
        f"[{ADDRESS}]:2227 {wrong_key.get_name()} {wrong_key.get_base64()}\n")

    client_key = paramiko.RSAKey.generate(2048)
    client_key.write_private_key_file(str(staging / "client.pem"), password="fixture-passphrase")
    (staging / "client.pem.pub").write_text(f"{client_key.get_name()} {client_key.get_base64()}\n")

    class SFTPConnection(socketserver.BaseRequestHandler):
        def handle(self):
            print("SFTP-CONNECT", flush=True)
            with paramiko.Transport(self.request) as transport:
                transport.add_server_key(key)
                transport.set_subsystem_handler("sftp", paramiko.SFTPServer,
                                                MetadataSFTP, root=sftp_root)
                transport.start_server(server=Auth(client_key))
                while transport.is_active():
                    transport.join(timeout=0.1)

    auth = DummyAuthorizer()
    auth.add_user(USER, PASSWORD, str(ftp_root), perm="elr")
    for directory in UPLOADS:
        auth.override_perm(USER, str(ftp_root / directory), perm="elrwd")
    MetadataFTP.authorizer = auth
    for volume in (ftp_root, sftp_root):
        make_scaling_trees(volume)

    class SFTPServer(socketserver.ThreadingTCPServer):
        allow_reuse_address = True
        daemon_threads = True

    with SFTPServer((ADDRESS, 2227), SFTPConnection) as sftp:
        ftp = FTPServer((ADDRESS, 2127), MetadataFTP)
        threading.Thread(target=sftp.serve_forever, daemon=True).start()
        print("READY loopback FTP/SFTP metadata fixtures", flush=True)
        ftp.serve_forever()


if __name__ == "__main__":
    main()
