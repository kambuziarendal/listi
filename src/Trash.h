// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include "FileOperation.h"
namespace Listi {
// Haiku backend. These functions perform I/O and must run off the UI thread.
std::string TrashDirectory(const std::string &path, bool create);
void CheckTrashSource(const std::string &path);
std::string MoveToTrash(const std::string &path,
                        const NodeIdentity *expected = nullptr);
std::string OriginalTrashPath(const std::string &path, const NodeIdentity &expected);
std::string RestoreFromTrash(const std::string &path, std::string *warning = nullptr,
                             const NodeIdentity *expected = nullptr,
                             const std::string *confirmedTarget = nullptr);
} // namespace Listi
