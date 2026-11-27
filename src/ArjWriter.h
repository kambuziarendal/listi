// SPDX-License-Identifier: MIT
#pragma once

#include "FileOperation.h"

namespace Listi {
bool ArjCanCreate();
bool ArjCanCompress();
// Without the optional arj tool, or at level zero, write Store members.
void ArjWrite(int output, const std::vector<std::string> &sources,
              const Hooks &hooks, int level = 0);
// Only the metadata subset emitted by ArjWrite can be safely rebuilt.
void ArjValidateEdit(int input, const Hooks &hooks = {});
} // namespace Listi
