// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <sys/types.h>
#include <string>
#include <vector>
namespace Listi {
struct ArchiveVolume {
  std::string path, identity;
  int64_t size = 0;
  dev_t device;
  ino_t inode;
};
struct ArchiveVolumes {
  enum Kind { Single, Joined7z, Rar } kind = Single;
  std::vector<ArchiveVolume> files;
  std::string Fingerprint() const;
  void Verify() const;
};
// Only numbered siblings in one directory are eligible. No decoder-supplied
// path can expand this captured set. Missing middle/first parts fail here;
// the native reader must additionally verify that the last part is complete.
ArchiveVolumes FindArchiveVolumes(const std::string &path);
} // namespace Listi
