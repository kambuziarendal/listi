// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

namespace Listi {
class RemovableCopyInterrupted : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
  RemovableCopyInterrupted(const std::string &message,
                           const std::string &folder)
      : std::runtime_error(message), folder(folder) {}
  std::string folder;
};

bool IsRemovableIOError(int error);

enum class StagingPermissions { Preserve, PreserveIfSupported, KeepCreationMode };

// Takes ownership of both descriptors. Returns false if chmod was unsupported
// and the caller explicitly allowed keeping the destination mode. The I/O worker only sees an already
// opened source and a private staging inode. Only the caller can publish it.
bool WriteRemovableStagingFile(int source, int staging,
                               const struct stat &before,
                               const std::function<void(size_t)> &checkpoint,
                               bool privateStaging = false,
                               StagingPermissions permissions = StagingPermissions::Preserve,
                               bool flushBeforeTimes = false);
// Takes ownership. After cancellation the worker can only flush and close.
int FlushRemovableDirectory(int descriptor,
                            const std::function<void()> &checkpoint,
                            int &closeError);
} // namespace Listi
