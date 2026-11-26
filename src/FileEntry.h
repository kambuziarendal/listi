// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>

namespace Listi {
struct FileEntry {
  std::string name, path;
  bool directory = false, link = false;
  bool sizeKnown = true;
  int64_t size = 0, modified = 0;
  unsigned permissions = 0644;
  bool modifiedKnown = false; // Distinguishes an explicit epoch from an absent timestamp.
  // Decimal digits, without insignificant trailing zeros. Do not round
  // the source revision to the precision of time_t or of the local filesystem.
  std::string modifiedFraction;

  // Opaque provider identity when available (e.g. the FTP MLSD unique fact).
  std::string providerIdentity;

  bool SameModification(const FileEntry &other) const {
    return modifiedKnown == other.modifiedKnown && modified == other.modified &&
           modifiedFraction == other.modifiedFraction;
  }

  bool SameRevision(const FileEntry &other) const {
    return directory == other.directory && link == other.link &&
           sizeKnown == other.sizeKnown && (!sizeKnown || size == other.size) &&
           SameModification(other) && providerIdentity == other.providerIdentity;
  }

  void SetModification(int64_t seconds, long nanoseconds) {
    modified = seconds;
    modifiedKnown = true;
    modifiedFraction.clear();
    if (nanoseconds > 0 && nanoseconds < 1000000000) {
      modifiedFraction = std::to_string(1000000000 + nanoseconds).substr(1);
      while (modifiedFraction.back() == '0')
        modifiedFraction.pop_back();
    }
  }

  long ModifiedNanoseconds() const {
    long ns = 0;
    for (std::size_t i = 0; i < 9; ++i)
      ns = ns * 10 +
           (i < modifiedFraction.size() ? modifiedFraction[i] - '0' : 0);
    return ns;
  }
};
} // namespace Listi
