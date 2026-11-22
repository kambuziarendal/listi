// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>
#include <sys/stat.h>

namespace Listi {
// A durable advisory record. Destruction never removes a record implicitly:
// only the owner of a completed operation may retire it.
class RecoveryRecord {
public:
  RecoveryRecord(const std::string &kind, const std::string &path,
                 const struct stat &identity, const std::string &inventory = {});
  void Retire();
private:
  std::string path;
  struct stat identity;
};

// Read only. A matching entry is not permission to resume or delete anything.
std::string ReviewRecovery(const std::string &inventory = {});
} // namespace Listi
