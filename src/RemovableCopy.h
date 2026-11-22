// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <stdexcept>
#include <sys/stat.h>

namespace Listi {
class RemovableCopyInterrupted : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Takes ownership of both descriptors. The I/O worker only sees an already
// opened source and a private staging inode. Only the caller can publish it.
void WriteRemovableStagingFile(int source, int staging, const struct stat &before,
                               const std::function<void(size_t)> &checkpoint);
}
