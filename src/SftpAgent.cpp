// SPDX-License-Identifier: MIT
#include "SftpAgent.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace Listi {
namespace {
// libssh2's Unix agent backend performs blocking connect/read/write calls.
// Use the two standard SSH-agent requests with a nonblocking local socket;
// libssh2 still negotiates and authenticates the SSH session.
using Clock = std::chrono::steady_clock;
struct AgentFailure {};
constexpr unsigned char RequestIdentities = 11, IdentitiesAnswer = 12,
                        SignRequest = 13, SignResponse = 14;
constexpr size_t MaxPacket = 256 * 1024; // OpenSSH agent message limit

void AppendNumber(std::string &s, uint32_t n) {
  n = htonl(n);
  s.append(reinterpret_cast<const char *>(&n), sizeof(n));
}
void AppendField(std::string &s, std::string_view field) {
  if (field.size() > MaxPacket)
    throw AgentFailure();
  AppendNumber(s, field.size());
  s.append(field.data(), field.size());
}
std::string_view Take(std::string_view &s, size_t n) {
  if (n > s.size())
    throw AgentFailure();
  auto field = s.substr(0, n);
  s.remove_prefix(n);
  return field;
}
uint32_t Number(std::string_view &s) {
  uint32_t n;
  memcpy(&n, Take(s, sizeof(n)).data(), sizeof(n));
  return ntohl(n);
}
std::string_view Field(std::string_view &s) { return Take(s, Number(s)); }

class Agent {
public:
  explicit Agent(const TransferControl &c) : control(c) {}
  ~Agent() { if (fd >= 0) close(fd); }
  void Connect(const char *path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (!*path || strlen(path) >= sizeof(address.sun_path))
      throw AgentFailure();
    strcpy(address.sun_path, path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
      throw AgentFailure();
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
      return;
    if (errno != EINPROGRESS)
      throw AgentFailure();
    Wait(POLLOUT);
    int err = 0;
    socklen_t length = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &length) || err)
      throw AgentFailure();
  }
  std::string Exchange(const std::string &request) {
    if (request.size() > MaxPacket)
      throw AgentFailure();
    std::string framed;
    AppendField(framed, request);
    Transfer(framed.data(), framed.size(), true);
    uint32_t size;
    Transfer(reinterpret_cast<char *>(&size), sizeof(size), false);
    size = ntohl(size);
    if (!size || size > MaxPacket)
      throw AgentFailure();
    std::string reply(size, '\0');
    Transfer(reply.data(), reply.size(), false);
    return reply;
  }
private:
  void Check() {
    control.Check();
    if (Clock::now() >= deadline)
      throw std::runtime_error("SSH agent authentication timed out.");
  }
  void Wait(short events) {
    pollfd p{fd, events, 0};
    for (;;) {
      Check();
      const int result = poll(&p, 1, 100);
      if (result > 0)
        return; // read/write/SO_ERROR supplies the terminal status
      if (result < 0 && errno != EINTR)
        throw AgentFailure();
    }
  }
  void Transfer(char *data, size_t remaining, bool writing) {
    while (remaining) {
      Wait(writing ? POLLOUT : POLLIN);
      const auto n = writing ? send(fd, data, remaining, MSG_NOSIGNAL) : recv(fd, data, remaining, 0);
      if (n < 0 && (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN))
        continue;
      if (n <= 0)
        throw AgentFailure();
      data += n;
      remaining -= n;
    }
  }
  const TransferControl &control;
  Clock::time_point deadline = Clock::now() + std::chrono::seconds(20);
  int fd = -1;
};

struct Signer {
  Agent &agent;
  std::string_view key;
  std::exception_ptr failure;
  static LIBSSH2_USERAUTH_PUBLICKEY_SIGN_FUNC(Sign) {
    (void)session;
    auto &signer = *static_cast<Signer *>(*abstract);
    try {
      const std::string_view payload(reinterpret_cast<const char *>(data), data_len);
      auto fields = payload;
      Field(fields); // session id
      if (Take(fields, 1).front() != 50)
        throw AgentFailure();
      Field(fields); // user
      Field(fields); // service
      if (Field(fields) != "publickey" || Take(fields, 1).front() != 1)
        throw AgentFailure();
      const auto algorithm = Field(fields);
      if (Field(fields) != signer.key || !fields.empty())
        throw AgentFailure();
      std::string request(1, SignRequest);
      AppendField(request, signer.key);
      AppendField(request, payload);
      // RFC 8332 algorithms use the same public key blob as legacy ssh-rsa.
      const auto plain = algorithm.substr(0, algorithm.find("-cert-v01@openssh.com"));
      const uint32_t flags = plain == "rsa-sha2-512" ? 4 : plain == "rsa-sha2-256" ? 2 : 0;
      AppendNumber(request, flags);
      const auto reply = signer.agent.Exchange(request);
      std::string_view frame(reply);
      if (Take(frame, 1).front() != SignResponse)
        return LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED; // agent declined this key
      auto signature = Field(frame);
      const auto method = Field(signature);
      // OpenSSH certificate keys return signatures of the underlying type.
      const bool securityKey = plain.substr(0, 3) == "sk-";
      const auto expected = securityKey && plain != algorithm
          ? std::string(plain) + "@openssh.com" : std::string(plain);
      if (method != expected && method != algorithm)
        return LIBSSH2_ERROR_ALGO_UNSUPPORTED;
      // libssh2 takes the complete inner signature (including flags and
      // counter) for security keys, and just the signature bytes otherwise.
      const auto encoded = signature;
      const auto raw = Field(signature);
      const auto bytes = securityKey ? encoded : raw;
      if (signature.size() != (securityKey ? 5u : 0u) || !frame.empty() || raw.empty())
        throw AgentFailure();
      *sig = static_cast<unsigned char *>(malloc(bytes.size())); // default libssh2 allocator
      if (!*sig)
        return LIBSSH2_ERROR_ALLOC;
      memcpy(*sig, bytes.data(), bytes.size());
      *sig_len = bytes.size();
      return 0;
    } catch (...) {
      signer.failure = std::current_exception();
      return LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED;
    }
  }
};
}

bool AuthenticateWithAgent(SftpSession &session, LIBSSH2_SESSION *ssh, const char *user,
                           const TransferControl &control) {
  const char *path = getenv("SSH_AUTH_SOCK");
  if (!path)
    return false;
  try {
    Agent agent(control);
    agent.Connect(path);
    const auto reply = agent.Exchange(std::string(1, RequestIdentities));
    std::string_view keys(reply);
    if (Take(keys, 1).front() != IdentitiesAnswer)
      return false;
    const uint32_t count = Number(keys);
    for (uint32_t i = 0; i < count; ++i) {
      const auto key = Field(keys);
      Field(keys); // human-readable comment
      Signer signer{agent, key, {}};
      void *context = &signer;
      const auto status = session.Request(control, [&] {
        return libssh2_userauth_publickey(ssh, user,
            reinterpret_cast<const unsigned char *>(key.data()), key.size(), Signer::Sign, &context);
      });
      if (signer.failure)
        std::rethrow_exception(signer.failure);
      if (!status)
        return true;
    }
  } catch (const AgentFailure &) {
    // Unavailable agent or invalid agent response: try the next login method.
  }
  return false;
}
}
