// SPDX-License-Identifier: MIT
#pragma once
#include <functional>
#include <string>
#include <sys/types.h>

namespace Listi {
void WriteOutputBytes(int fd, const char *data, size_t size);

// Reserve a new leaf in the pinned folder. Before success, flush both the
// output and the folder, and verify that the destination still names them.
void SaveExclusiveFile(int folder, const std::string &name, mode_t mode,
                       const std::function<void(int)> &write,
                       const std::function<void()> &verifyFolder);
}
