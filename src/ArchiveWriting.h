// SPDX-License-Identifier: MIT
#pragma once
#include "ArchivePasswords.h"
#include <cstdint>
#include <map>
#include <tuple>
#include <string>
#include <vector>

namespace Listi {
struct Hooks;
struct TarMemberMetadata {
  int64_t uid, gid;
  std::string user, group;
  bool directory;
  unsigned mode;
  int64_t modified;
  long nanoseconds;
  bool operator==(const TarMemberMetadata &other) const {
    return std::tie(uid, gid, user, group, directory, mode, modified, nanoseconds) ==
           std::tie(other.uid, other.gid, other.user, other.group, other.directory,
                    other.mode, other.modified, other.nanoseconds);
  }
};
using TarMemberMap = std::map<std::string, TarMemberMetadata>;
bool IsTarFormat(const std::string &format);
void CreateArchiveOutput(int output, const std::string &format,
                         const std::vector<std::string> &sources, int level,
                         bool encryptNames, const ArchiveSecretPtr &secret,
                         const Hooks &, const TarMemberMap &metadata = {});
std::string DetectArchiveEditFormat(const std::string &path,
                                   const Hooks &, TarMemberMap *metadata = nullptr);
}
