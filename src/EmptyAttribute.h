// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <fcntl.h>
#include <fs_attr.h>
#include <stdexcept>
#include <string>

namespace Listi {
// A zero-byte write need not create an attribute on BFS. On the pinned output,
// materialize an inode-backed attribute, truncate it, and check its typed header.
inline void RestoreEmptyAttribute(int fd, const char *name, uint32_t type) {
  static constexpr char seed[16 * 1024] = {};
  if (fs_write_attr(fd, name, type, 0, seed, sizeof(seed)) != sizeof(seed))
    throw std::runtime_error(std::string("Cannot create empty BFS attribute: ") + name);
  const int attribute = fs_fopen_attr(fd, name, type, O_WRONLY | O_TRUNC);
  if (attribute < 0 || fs_close_attr(attribute) != 0)
    throw std::runtime_error(std::string("Cannot truncate BFS attribute: ") + name);
  attr_info restored{};
  if (fs_stat_attr(fd, name, &restored) != 0 || restored.type != type || restored.size != 0)
    throw std::runtime_error(std::string("Cannot verify empty BFS attribute: ") + name);
}
}
