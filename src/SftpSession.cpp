// SPDX-License-Identifier: MIT
#include "SftpSession.h"
#include "CurlLibrary.h"
#include "SshPaths.h"
#include "SftpAgent.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace Listi {
namespace {
struct DialControl {
  const TransferControl &control;
  std::exception_ptr failure;
  static int Progress(void *context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto &dial = *static_cast<DialControl *>(context);
    try {
      dial.control.Check();
      return 0;
    } catch (...) {
      dial.failure = std::current_exception();
      return 1;
    }
  }
};

void KeyboardPassword(const char *, int, const char *, int, int count,
                      const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
                      LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, void **abstract) {
  if (count != 1 || prompts[0].echo || !abstract || !*abstract)
    return;
  const auto &password = static_cast<const Connection *>(*abstract)->password;
  responses[0].text = strdup(password.c_str());
  if (responses[0].text)
    responses[0].length = password.size();
}
}

void SftpSession::Handshake(const Location &location, const Connection &account,
                            const TransferControl &control) {
  const auto path = account.knownHosts.empty() ? DefaultSshPath(SshFile::KnownHosts) : account.knownHosts;
  std::unique_ptr<LIBSSH2_KNOWNHOSTS, decltype(&libssh2_knownhost_free)> hosts(
      libssh2_knownhost_init(ssh), libssh2_knownhost_free);
  auto hostname = location.host;
  if (hostname.size() > 2 && hostname.front() == '[' && hostname.back() == ']')
    hostname = hostname.substr(1, hostname.size() - 2);
  const int port = location.port ? location.port : 22;
  // checkp(host, port) also searches a bare host pin. That pin belongs to
  // port 22, not to other services on the same machine. Match the literal
  // OpenSSH route (or its hash) for both negotiation and final verification.
  const auto route = port == 22 ? hostname : "[" + hostname + "]:" + std::to_string(port);
  const char *untrusted = "SSH host key is unknown or changed. Verify the server and its known_hosts file.";
  if (!hosts || !ReadableSshFile(path) ||
      libssh2_knownhost_readfile(hosts.get(), path.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) <= 0)
    throw std::runtime_error(untrusted);

  // A server may offer several keys while this route pins only one. Use the
  // library's hostname/port (including hashed hostname) matching to find
  // compatible key types before negotiation. A deliberately empty candidate
  // produces MISMATCH only when the route has a key of that type.
  const struct { int type; const char *methods; } keys[] = {
    {LIBSSH2_KNOWNHOST_KEY_ED25519, "ssh-ed25519"},
    {LIBSSH2_KNOWNHOST_KEY_ECDSA_256, "ecdsa-sha2-nistp256"},
    {LIBSSH2_KNOWNHOST_KEY_ECDSA_384, "ecdsa-sha2-nistp384"},
    {LIBSSH2_KNOWNHOST_KEY_ECDSA_521, "ecdsa-sha2-nistp521"},
    {LIBSSH2_KNOWNHOST_KEY_SSHRSA, "rsa-sha2-512,rsa-sha2-256,ssh-rsa"},
    {LIBSSH2_KNOWNHOST_KEY_SSHDSS, "ssh-dss"}
  };
  std::string methods;
  for (const auto &key : keys) {
    if (libssh2_knownhost_checkp(hosts.get(), route.c_str(), port == 22 ? 22 : -1, "", 0,
        LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_BASE64 | key.type,
        nullptr) == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
      if (!methods.empty())
        methods += ',';
      methods += key.methods;
    }
  }
  if (methods.empty())
    throw std::runtime_error(untrusted);
  Require(libssh2_session_method_pref(ssh, LIBSSH2_METHOD_HOSTKEY, methods.c_str()),
          "Select pinned SSH host key type");
  Require(Request(control, [&] { return libssh2_session_handshake(ssh, socket); }), "SSH handshake");
  size_t length = 0;
  int type = LIBSSH2_HOSTKEY_TYPE_UNKNOWN;
  const auto *key = libssh2_session_hostkey(ssh, &length, &type);
  // The raw SSH key blob contains its type. Only a full key match authorizes login.
  if (!key || !length || type == LIBSSH2_HOSTKEY_TYPE_UNKNOWN ||
      libssh2_knownhost_checkp(hosts.get(), route.c_str(), port == 22 ? 22 : -1, key, length,
          LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW, nullptr)
          != LIBSSH2_KNOWNHOST_CHECK_MATCH)
    throw std::runtime_error(untrusted);
}

SftpSession::~SftpSession() {
  // No blocking protocol shutdown on a lost connection or a cancelled job.
  // Normal operations close their own handle and check the server's reply.
  if (socket != CURL_SOCKET_BAD)
    shutdown(socket, SHUT_RDWR);
  if (ssh) {
    libssh2_session_set_blocking(ssh, 1);
    libssh2_session_set_timeout(ssh, 250);
    if (handle)
      libssh2_sftp_close_handle(handle);
    if (sftp)
      libssh2_sftp_shutdown(sftp);
    libssh2_session_free(ssh);
  }
  if (transport)
    LIBCALL(Curl(), curl_easy_cleanup)(transport);
}

void SftpSession::Connect(const Location &location, const TransferControl &control) {
  control.Check();
  auto account = GetConnection(location.URL());
  transport = NewCurlHandle();
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    if (libssh2_init(0))
      throw std::runtime_error("Could not initialize SSH.");
  });
  // CONNECT_ONLY establishes TCP; no HTTP request or credentials are sent.
  // Keep the easy handle alive until SSH releases the borrowed socket.
  auto &curl = Curl();
  auto set = [&](CURLoption option, auto value) {
    if (LIBCALL(curl, curl_easy_setopt)(transport, option, value) != CURLE_OK)
      throw std::runtime_error("Could not configure the SSH connection.");
  };
  const auto *version = LIBCALL(curl, curl_version_info)(CURLVERSION_NOW);
  if (control.cleanup && (!version || !(version->features & CURL_VERSION_ASYNCHDNS)))
    throw std::runtime_error("Remote cleanup deferred: resolver has no deadline.");
  const auto address = "http://" + location.host + ":" + std::to_string(location.port ? location.port : 22) + "/";
  DialControl dial{control, {}};
  set(CURLOPT_URL, address.c_str());
  set(CURLOPT_PROTOCOLS_STR, "http");
  set(CURLOPT_PROXY, "");
  set(CURLOPT_NOSIGNAL, 1L);
  set(CURLOPT_CONNECT_ONLY, 1L);
  set(CURLOPT_TIMEOUT_MS, control.cleanup ? std::min(10000L, control.cleanup->TimeoutMs()) : 10000L);
  set(CURLOPT_XFERINFOFUNCTION, &DialControl::Progress);
  set(CURLOPT_XFERINFODATA, &dial);
  set(CURLOPT_NOPROGRESS, 0L);
  const auto connected = LIBCALL(curl, curl_easy_perform)(transport);
  set(CURLOPT_NOPROGRESS, 1L);
  set(CURLOPT_XFERINFODATA, static_cast<void *>(nullptr));
  if (dial.failure)
    std::rethrow_exception(dial.failure);
  if (connected != CURLE_OK)
    throw std::runtime_error(std::string("SSH connection: ") + LIBCALL(curl, curl_easy_strerror)(connected));
  if (LIBCALL(curl, curl_easy_getinfo)(transport, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK ||
      socket == CURL_SOCKET_BAD)
    throw std::runtime_error("No socket for SSH.");
  const int flags = fcntl(socket, F_GETFL);
  if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK))
    throw std::runtime_error("Cannot make SSH cancellable.");
  ssh = libssh2_session_init();
  if (!ssh)
    throw std::runtime_error("Cannot allocate SSH session.");
  libssh2_session_set_blocking(ssh, 0);
  Handshake(location, account, control);
  Authenticate(account, control);
  Require(Request(control, [&] {
    sftp = libssh2_sftp_init(ssh);
    return sftp ? 0 : libssh2_session_last_errno(ssh);
  }), "Open SFTP subsystem");
}

void SftpSession::Wait(const TransferControl &control, std::chrono::steady_clock::time_point deadline) {
  using namespace std::chrono;
  const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
  if (remaining <= 0)
    throw std::runtime_error("SFTP request timed out.");
  int timeout = std::min<int64_t>(remaining, 100);
  if (control.cleanup)
    timeout = std::min<long>(timeout, control.cleanup->TimeoutMs());
  const int direction = libssh2_session_block_directions(ssh);
  short events = 0;
  if (direction & LIBSSH2_SESSION_BLOCK_INBOUND)
    events |= POLLIN;
  if (direction & LIBSSH2_SESSION_BLOCK_OUTBOUND)
    events |= POLLOUT;
  pollfd wait{socket, events, 0};
  if (poll(&wait, 1, timeout) < 0 && errno != EINTR)
    throw std::runtime_error("Cannot wait for SFTP response.");
}

void SftpSession::Require(long result, const char *operation) const {
  if (result >= 0)
    return;
  const auto reason = result == LIBSSH2_ERROR_SFTP_PROTOCOL && sftp
      ? "server status " + std::to_string(libssh2_sftp_last_error(sftp))
      : "SSH error " + std::to_string(result);
  throw std::runtime_error(std::string(operation) + ": " + reason);
}

void SftpSession::Authenticate(Connection &account, const TransferControl &control) {
  const char *user = account.username.c_str();
  char *methods = nullptr;
  const int queried = Request(control, [&] {
    methods = libssh2_userauth_list(ssh, user, account.username.size());
    return methods || libssh2_userauth_authenticated(ssh) ? 0 : libssh2_session_last_errno(ssh);
  });
  Require(queried, "Query SSH authentication");
  if (libssh2_userauth_authenticated(ssh))
    return;
  const std::string offered = "," + std::string(methods ? methods : "") + ",";
  if (offered.find(",publickey,") != std::string::npos) {
    const auto key = account.privateKey.empty() ? DefaultSshPath(SshFile::Identity) : account.privateKey;
    if (!key.empty() && Request(control, [&] {
      return libssh2_userauth_publickey_fromfile(ssh, user, nullptr, key.c_str(),
                                                 account.keyPassphrase.c_str());
    }) == 0)
      return;
    try {
      if (!control.cleanup && AuthenticateWithAgent(*this, ssh, user, control))
        return;
    } catch (const SftpAgentTimeout &) {
      control.Check(); // Cancellation must not turn into another login attempt.
      if (account.password.empty() || (offered.find(",password,") == std::string::npos &&
                                       offered.find(",keyboard-interactive,") == std::string::npos))
        throw;
    }
  }
  if (offered.find(",password,") != std::string::npos && !Request(control, [&] {
    return libssh2_userauth_password(ssh, user, account.password.c_str());
  }))
    return;
  if (offered.find(",keyboard-interactive,") != std::string::npos) {
    *libssh2_session_abstract(ssh) = &account;
    const auto status = Request(control, [&] {
      return libssh2_userauth_keyboard_interactive(ssh, user, KeyboardPassword);
    });
    *libssh2_session_abstract(ssh) = nullptr;
    if (!status)
      return;
  }
  throw std::runtime_error("SSH authentication failed. Check the selected username, password or private key.");
}

LIBSSH2_SFTP_HANDLE *SftpSession::Open(const std::string &path, unsigned long flags,
                                        bool directory, const TransferControl &control) {
  Require(Request(control, [&] {
    handle = libssh2_sftp_open_ex(sftp, path.c_str(), path.size(), flags, 0600,
                                  directory ? LIBSSH2_SFTP_OPENDIR : LIBSSH2_SFTP_OPENFILE);
    return handle ? 0 : libssh2_session_last_errno(ssh);
  }), directory ? "Open SFTP folder" : "Open SFTP file");
  return handle;
}
void SftpSession::Close(const TransferControl &control) {
  const auto status = Request(control, [&] { return libssh2_sftp_close_handle(handle); });
  // libssh2 releases the handle on every terminal response, even an error.
  handle = nullptr;
  Require(status, "Close SFTP handle");
}
}
