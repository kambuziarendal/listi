#!/usr/bin/env python3
"""Read-only loopback SFTP fixture for the installed --sftp-copy-test.

Pass a new private directory. Forward guest port 28657 to host 127.0.0.1:28657,
and copy the generated client key and known_hosts to the guest. Events count
accepted SSH transports and directory lists/rows, not individual packets.

Run each case with the installed app, using the two guest paths:
    /boot/system/apps/Listi --sftp-copy-test 8 known_hosts benchmark-key
Replace 8 with 64, fifo (an unrelated FIFO outside the selection) or wide
(one selected file among 1,024 siblings). Measure the events during each
separate process. For an OpenSSH reference, use get -r /8/top or /64/top
with the same account, private key and pinned known_hosts.
"""
import errno
import os
from pathlib import Path
import socketserver
import stat
import sys

import paramiko


PORT = 28657
state = Path(sys.argv[1]).resolve()
state.mkdir(mode=0o700)
root = state / "volume"
for label, width in (("8", 8), ("64", 64), ("fifo", 8), ("wide", 1024)):
    folder = root / label
    if label != "wide":
        folder = folder / "top" / "inner"
    folder.mkdir(parents=True)
    for i in range(width):
        name = f"item-{i:04d}"
        (folder / name).write_bytes(name.encode())
os.mkfifo(root / "fifo" / "outside-tree")
host_key = paramiko.RSAKey.generate(2048)
client_key = paramiko.RSAKey.generate(2048)
client_key.write_private_key_file(str(state / "benchmark-key"))
(state / "known_hosts").write_text(
    f"[127.0.0.1]:{PORT} {host_key.get_name()} {host_key.get_base64()}\n")


class Account(paramiko.ServerInterface):
    def check_channel_request(self, kind, ident):
        return paramiko.OPEN_SUCCEEDED if kind == "session" else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

    def get_allowed_auths(self, username):
        return "publickey"

    def check_auth_publickey(self, username, key):
        return paramiko.AUTH_SUCCESSFUL if username == "benchmark" and key == client_key else paramiko.AUTH_FAILED


class Fixture(paramiko.SFTPServerInterface):
    def local(self, path):
        target = root / path.lstrip("/")
        if not target.resolve().is_relative_to(root):
            raise PermissionError(errno.EACCES, "Outside fixture")
        return target

    def list_folder(self, path):
        try:
            entries = [paramiko.SFTPAttributes.from_stat(child.lstat(), child.name)
                       for child in self.local(path).iterdir()]
            print("LIST", len(entries), path, flush=True)
            return entries
        except OSError as exc:
            return paramiko.SFTPServer.convert_errno(exc.errno)

    def lstat(self, path):
        try:
            print("LSTAT", path, flush=True)
            return paramiko.SFTPAttributes.from_stat(self.local(path).lstat())
        except OSError as exc:
            return paramiko.SFTPServer.convert_errno(exc.errno)

    stat = lstat

    def open(self, path, flags, attr):
        try:
            local = self.local(path)
            if flags != os.O_RDONLY or not stat.S_ISREG(local.lstat().st_mode):
                return paramiko.SFTP_PERMISSION_DENIED
            handle = paramiko.SFTPHandle(flags)
            handle.readfile = local.open("rb")
            handle.stat = lambda: paramiko.SFTPAttributes.from_stat(os.fstat(handle.readfile.fileno()))
            return handle
        except OSError as exc:
            return paramiko.SFTPServer.convert_errno(exc.errno)


class Connection(socketserver.BaseRequestHandler):
    def handle(self):
        print("CONNECT", flush=True)
        with paramiko.Transport(self.request) as transport:
            transport.add_server_key(host_key)
            transport.set_subsystem_handler("sftp", paramiko.SFTPServer, Fixture)
            transport.start_server(server=Account())
            while transport.is_active():
                transport.join(0.1)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


with Server(("127.0.0.1", PORT), Connection) as server:
    print("READY", flush=True)
    server.serve_forever()
