// SPDX-License-Identifier: MIT
#pragma once
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace Listi {
struct Job;
namespace ArchiveJobChecks {
std::string LocalSource(const std::string &path);
Job PublishedArchiveGuard(const Job &edit, const std::string &receipt);
void VerifyArchiveSets(const Job &);
void RequireArchiveVersions(const Job &shown, const Job &prepared);
void Guards(const Job &);
std::string MemberPath(const std::string &path);

inline void Require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct FD {
  int n;
  explicit FD(int value) : n(value) {}
  ~FD() {
    if (n >= 0)
      close(n);
  }
};
inline bool Unchanged(const struct stat &a, const struct stat &b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
         a.st_mode == b.st_mode && a.st_size == b.st_size &&
         a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
         a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
         a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
         a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
}
}
