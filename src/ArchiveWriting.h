// SPDX-License-Identifier: MIT
#pragma once
#include "ArchivePasswords.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Listi {
struct Hooks;
struct TarOwner {
  int64_t uid, gid;
  std::string user, group;
  bool operator==(const TarOwner &other) const {
    return uid == other.uid && gid == other.gid &&
           user == other.user && group == other.group;
  }
};
using TarOwners = std::map<std::string, TarOwner>;
void CreateArchiveOutput(int output, const std::string &format,
                         const std::vector<std::string> &sources, int level,
                         bool encryptNames, const ArchiveSecretPtr &secret,
                         const Hooks &, const TarOwners &owners = {});
std::string DetectArchiveEditFormat(const std::string &path,
                                   const Hooks &, TarOwners *owners = nullptr);
}
