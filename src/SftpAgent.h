// SPDX-License-Identifier: MIT
#pragma once
#include "SftpSession.h"

namespace Listi {
struct SftpAgentTimeout : std::runtime_error {
  SftpAgentTimeout() : std::runtime_error("SSH agent authentication timed out.") {}
};
bool AuthenticateWithAgent(SftpSession &, LIBSSH2_SESSION *, const char *user,
                           const TransferControl &);
}
