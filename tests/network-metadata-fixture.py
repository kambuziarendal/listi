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
UPLOADS = {"upload-uncertain", "upload-cancel"}


class MetadataFS(AbstractedFS):
    def format_list(self, basedir, listing, *args, **kwargs):
        if Path(basedir).name in LEGACY | UPLOADS:
            listing = [name for name in listing if not name.startswith(".")]
        return super().format_list(basedir, listing, *args, **kwargs)

    def format_mlsx(self, basedir, listing, perms, facts, ignore_err=True):
        for row in super().format_mlsx(basedir, listing, perms, facts, ignore_err):
            attrs, name = row.split(b" ", 1)
            leaf = name.rstrip().decode()
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
        folder = Path(path)
        if (folder.name in LEGACY | {"list-all"} or
                (folder.name == "upload-uncertain" and any(folder.glob("*.part"))) or
                (folder.name == "upload-cancel" and self.cancel_signal.exists())):
            self.respond("502 MLSD unavailable.")
            return
        return super().ftp_MLSD(path)

    def on_file_sent(self, filename):
        # Small transfers can finish before pyftpdlib assigns DTPHandler.cmd.
        # This callback runs before servicing the client's next stat/list.
        path = Path(filename)
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
        if file.parent.name == "upload-cancel" and file.suffix == ".part":
            # Only reached when the client verifies a STOR acknowledged earlier.
            self.cancel_signal.touch()
        return super().ftp_RETR(path)


class Auth(paramiko.ServerInterface):
    def check_auth_password(self, username, password):
        return (paramiko.AUTH_SUCCESSFUL if (username, password) == (USER, PASSWORD)
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

    def resolve(self, path):
        candidate = (self.root / path.lstrip("/")).resolve()
        if not candidate.is_relative_to(self.root):
            raise PermissionError(13, "Outside fixture")
        return candidate

    def list_folder(self, path):
        try:
            entries = []
            for child in self.resolve(path).iterdir():
                entries.append(ListAttributes.from_stat(child.stat(), child.name))
            return entries
        except OSError as error:
            return paramiko.SFTPServer.convert_errno(error.errno)

    def stat(self, path):
        try:
            local = self.resolve(path)
            item = paramiko.SFTPAttributes.from_stat(local.stat())
            if local.name == "unknown":
                item.st_mtime = None
                item.st_atime = None
            return item
        except OSError as error:
            return paramiko.SFTPServer.convert_errno(error.errno)

    def open(self, path, flags, attr):
        if flags != os.O_RDONLY:
            return paramiko.SFTP_PERMISSION_DENIED
        try:
            handle = paramiko.SFTPHandle(flags)
            handle.readfile = self.resolve(path).open("rb")
            # curl may use FSTAT for size on the open handle.
            handle.stat = lambda: self.stat(path)
            if path == "/recursive/changed":
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
    for directory in ("hidden-only", "guarded-rename", "outer/mixed-nested"):
        folder = ftp_root / directory
        folder.mkdir(parents=True)
        (folder / ".settings").write_bytes(b"hidden\n")
        if directory != "hidden-only":
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
    MetadataFTP.cancel_signal = sftp_root / "signals" / "checking-upload"
    (sftp_root / "compare").mkdir()
    for name in ("known", "unknown", "compare/known"):
        file = sftp_root / name
        file.write_bytes(b"fixture contents\n")
        os.utime(file, (MODIFIED, MODIFIED))
    (sftp_root / "recursive").mkdir()
    recursive_sftp = sftp_root / "recursive" / "changed"
    recursive_sftp.write_bytes(changed.read_bytes())
    os.utime(recursive_sftp, (MODIFIED, MODIFIED))
    key = paramiko.RSAKey.generate(2048)
    (staging / "known_hosts").write_text(f"[{ADDRESS}]:2227 {key.get_name()} {key.get_base64()}\n")

    class SFTPConnection(socketserver.BaseRequestHandler):
        def handle(self):
            with paramiko.Transport(self.request) as transport:
                transport.add_server_key(key)
                transport.set_subsystem_handler("sftp", paramiko.SFTPServer,
                                                MetadataSFTP, root=sftp_root)
                transport.start_server(server=Auth())
                while transport.is_active():
                    transport.join(timeout=0.1)

    auth = DummyAuthorizer()
    auth.add_user(USER, PASSWORD, str(ftp_root), perm="elr")
    for directory in UPLOADS:
        auth.override_perm(USER, str(ftp_root / directory), perm="elrw")
    MetadataFTP.authorizer = auth
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
