// SPDX-License-Identifier: MIT
#include "ArchiveJobChecks.h"
#include "ArchiveBackend.h"
#include "ArchiveVolumes.h"
#include "FileOperation.h"
#include "VirtualFile.h"

namespace Listi::ArchiveJobChecks {
std::string LocalSource(const std::string &path) {
  auto loc = ParseLocation(path);
  Require(loc.Local() || loc.Archive(),
          "Archive jobs currently require local files or a local archive.");
  return loc.Archive() ? loc.archive : path;
}
void VerifyArchiveSets(const Job &job) {
  for (const auto &item : job.expectedArchiveSets)
    Require(FindArchiveVolumes(item.first).Fingerprint() == item.second,
            "Archive parts changed after selection. Reload and select again.");
}
void RequireArchiveVersions(const Job &shown, const Job &prepared) {
  for (const auto &[path, version] : shown.expectedArchiveSets) {
    const auto found = prepared.expectedArchiveSets.find(path);
    Require(found != prepared.expectedArchiveSets.end() && found->second == version,
            "Archive changed since selection. Reload and select again.");
  }
}
void Guards(const Job &job) {
  for (const auto &item : job.expectedSources) {
    struct stat s;
    Require(!lstat(item.first.c_str(), &s) && s.st_dev == item.second.device &&
                s.st_ino == item.second.inode,
            "A selected source was replaced. Select it again.");
  }
  if (!job.destinationFolder.empty()) {
    struct stat s;
    Require(!stat(job.destinationFolder.c_str(), &s) && S_ISDIR(s.st_mode) &&
                s.st_dev == job.expectedDestination.device &&
                s.st_ino == job.expectedDestination.inode,
            "The destination folder was replaced. Choose it again.");
  }
}
std::string MemberPath(const std::string &path) {
  auto value = path;
  while (!value.empty() && value.front() == '/')
    value.erase(value.begin());
  if (value.empty())
    return "";
  return SafeArchivePath(value.c_str());
}
}
