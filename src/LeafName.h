// SPDX-License-Identifier: MIT
#pragma once
#include <StorageDefs.h>
#include <algorithm>
#include <stdexcept>
#include <string>

namespace Listi {
// Reserve room for a generated suffix without splitting a UTF-8 character.
inline std::string SuffixedLeafName(const std::string &stem,
                                    const std::string &suffix,
                                    size_t limit = B_FILE_NAME_LENGTH - 1) {
  if (suffix.size() > limit)
    throw std::length_error("The filename suffix is too long.");
  size_t keep = std::min(stem.size(), limit - suffix.size());
  while (keep < stem.size() && keep > 0 &&
         (static_cast<unsigned char>(stem[keep]) & 0xc0) == 0x80)
    --keep;
  return stem.substr(0, keep) + suffix;
}
}
