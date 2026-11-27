// SPDX-License-Identifier: MIT
#pragma once

#include "VirtualFile.h"
#include <curl/curl.h>
#include <libssh2.h>
#include <libssh2_sftp.h>

namespace Listi {
// curl owns the cancellable DNS/TCP connection. libssh2 owns the single
// authenticated SSH session and SFTP channel carried by that socket.
class SftpSession {
public:
  SftpSession() = default;
  SftpSession(const SftpSession &) = delete;
  SftpSession &operator=(const SftpSession &) = delete;
  ~SftpSession();
  void Connect(const Location &, const TransferControl &);
  LIBSSH2_SFTP *Channel() const { return sftp; }

  template<typename F>
  auto Request(const TransferControl &control, F action) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
      control.Check();
      const auto result = action();
      if (result != LIBSSH2_ERROR_EAGAIN)
        return result;
      Wait(control, deadline);
    }
  }
  void Require(long result, const char *operation) const;
  LIBSSH2_SFTP_HANDLE *Open(const std::string &, unsigned long flags, bool directory,
                            const TransferControl &);
  void Close(const TransferControl &);

private:
  void Wait(const TransferControl &, std::chrono::steady_clock::time_point);
  void Authenticate(Connection &, const TransferControl &);
  CURL *transport = nullptr;
  curl_socket_t socket = CURL_SOCKET_BAD;
  LIBSSH2_SESSION *ssh = nullptr;
  LIBSSH2_SFTP *sftp = nullptr;
  LIBSSH2_SFTP_HANDLE *handle = nullptr;
};
}
