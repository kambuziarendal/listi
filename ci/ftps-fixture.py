"""Disposable loopback implicit-TLS server; never disable certificate checking."""
import sys
from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import TLS_FTPHandler
from pyftpdlib.servers import FTPServer


class ImplicitTLS(TLS_FTPHandler):
    def handle(self):
        self.secure_connection(self.ssl_context)

    def handle_ssl_established(self):
        super().handle()


share, cert, key = sys.argv[1:]
ImplicitTLS.certfile = cert
ImplicitTLS.keyfile = key
ImplicitTLS.tls_control_required = True
ImplicitTLS.tls_data_required = True
ImplicitTLS.passive_ports = range(31300, 31310)
ImplicitTLS.authorizer = DummyAuthorizer()
ImplicitTLS.authorizer.add_user("listitest", "fixture-only", share, perm="elradfmwMT")
FTPServer(("127.0.0.1", 2143), ImplicitTLS).serve_forever()
