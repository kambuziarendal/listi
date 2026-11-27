// SPDX-License-Identifier: MIT
#pragma once
#include <stdexcept>
#include <string>
#include "FileOperation.h"
namespace Listi {
// Haiku backend. These functions perform I/O and must run off the UI thread.
std::string TrashDirectory(const std::string &path, bool create);
class TrashUnsupported : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
void CheckTrashSource(const std::string &path, const NodeIdentity *expected = nullptr,
                      bool create = true);
std::string MoveToTrash(const std::string &path,
                        const NodeIdentity *expected = nullptr);
std::string OriginalTrashPath(const std::string &path, const NodeIdentity &expected);
RestoreTarget CaptureRestoreTarget(const std::string &path, const NodeIdentity &expected);
std::string RestoreFromTrash(const std::string &path, std::string *warning = nullptr,
                             const NodeIdentity *expected = nullptr,
                             const RestoreTarget *confirmedTarget = nullptr);
} // namespace Listi
