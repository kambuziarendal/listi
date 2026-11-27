// SPDX-License-Identifier: MIT
#pragma once
#include "SftpSession.h"

namespace Listi {
bool AuthenticateWithAgent(SftpSession &, LIBSSH2_SESSION *, const char *user,
                           const TransferControl &);
}
