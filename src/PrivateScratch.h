// SPDX-License-Identifier: MIT
#pragma once
#include "RecoveryInventory.h"
#include <Directory.h>
#include <Entry.h>
#include <filesystem>
#include <stdexcept>

namespace Listi {
inline bool RemoveStageContents(BDirectory &dir, dev_t device, unsigned depth = 0) {
  if (depth > 64 || dir.SetPermissions(0700) != B_OK)
    return false;
  bool clean = true;
  BEntry entry;
  status_t next;
  while ((next = dir.GetNextEntry(&entry, false)) == B_OK) {
    struct stat st;
    if (entry.GetStat(&st) != B_OK || st.st_dev != device) {
      clean = false;
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      BDirectory child(&entry);
      node_ref opened;
      if (child.GetNodeRef(&opened) != B_OK || opened.device != st.st_dev ||
          opened.node != st.st_ino || !RemoveStageContents(child, device, depth + 1)) {
        clean = false;
        continue;
      }
    }
    struct stat current;
    if (entry.GetStat(&current) != B_OK || current.st_dev != st.st_dev ||
        current.st_ino != st.st_ino || entry.Remove() != B_OK)
      clean = false;
  }
  return clean && next == B_ENTRY_NOT_FOUND;
}

// Only traverse the opened private directory; never descend through a
// replacement of its root name. Destructor failures leave a review record.
class PrivateScratch {
public:
  explicit PrivateScratch(const char *prefix)
      : record("codec", std::filesystem::canonical("/tmp"), prefix, "", ""),
        path(record.Stage()), parent(std::filesystem::path(path).parent_path().c_str()) {
    if (parent.InitCheck() != B_OK || mkdir(path.c_str(), 0700))
      throw std::runtime_error("Cannot create private codec directory: " + path);
    BEntry entry(&parent, std::filesystem::path(path).filename().c_str(), false);
    if (dir.SetTo(&entry) != B_OK || dir.GetStat(&identity) != B_OK ||
        parent.Sync() != B_OK)
      throw std::runtime_error("Cannot identify or sync private codec directory: " + path);
    record.Advance(RecoveryPhase::Writing, &identity);
  }
  ~PrivateScratch() {
    try {
      BEntry entry(&parent, std::filesystem::path(path).filename().c_str(), false);
      struct stat now;
      const auto matches = [&] {
        return entry.GetStat(&now) == B_OK && now.st_dev == identity.st_dev &&
               now.st_ino == identity.st_ino;
      };
      if (matches() && RemoveStageContents(dir, identity.st_dev) &&
          matches() && entry.Remove() == B_OK && parent.Sync() == B_OK)
        record.Retire();
    } catch (...) {
      // The inventory remains available for a read-only review.
    }
  }
private:
  RecoveryRecord record;
public:
  const std::string path;
private:
  BDirectory parent, dir;
  struct stat identity;
};
} // namespace Listi
