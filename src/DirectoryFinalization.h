// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <string>
#include <sys/stat.h>

namespace Listi {
// A directory and its name are checked independently. Hold the descriptor
// across recursive copy/metadata updates, before permissions can be tightened.
class PinnedDirectory {
public:
  PinnedDirectory(const std::string &path, dev_t device, ino_t inode,
                  bool follow = false);
  ~PinnedDirectory();
  PinnedDirectory(const PinnedDirectory &) = delete;
  PinnedDirectory &operator=(const PinnedDirectory &) = delete;

  int Descriptor() const { return fd; }
  void Verify() const;
  // False only for the known Haiku NTFS directory-fsync limitation.
  bool Flush(const std::function<void()> &checkpoint = {},
             bool allowUnsupportedNtfs = false) const;
  void FlushAfterCleanup(const std::function<void()> &checkpoint = {}) const;

private:
  std::string path;
  dev_t device;
  ino_t inode;
  bool follow;
  int fd = -1;
};

void FinalizeDirectory(const PinnedDirectory &directory,
                       const PinnedDirectory &parent,
                       const std::function<void()> &checkpoint = {});
} // namespace Listi
