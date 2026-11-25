// SPDX-License-Identifier: MIT
#pragma once

#include "VirtualFile.h"
#include <cstdint>
#include <limits.h>
#include <unistd.h>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace Listi {
inline constexpr char kUnavailableVolumeText[] =
    "Disk I/O was interrupted. Choose another folder. After safely reconnecting "
    "the disk, restart Listi to use it again.";

// Process-wide: a queued job and every window must make the same decision.
// Never probe the blocked mount to decide whether it is safe to reopen.
class UnavailableVolumes {
public:
  void Block(const std::string &path, int64_t device = -1) {
    const auto parts = Components(path);
    if (parts.empty())
      return;
    std::lock_guard<std::mutex> lock(mutex);
    roots.insert(parts);
    if (device >= 0)
      devices.insert(device);
  }

  bool Contains(const std::string &path) const {
    std::lock_guard<std::mutex> lock(mutex);
    if (roots.empty())
      return false;
    try {
      const auto location = ParseLocation(path);
      auto matching = [&](const auto &prefix) { return roots.count(prefix) != 0; };
      // Check each prefix before processing "..": the kernel can still
      // traverse a volume in a path such as /disk/../boot.
      return !Components(location.Archive() ? location.archive :
                         location.Local() ? location.path : "", matching).empty();
    } catch (const std::runtime_error &) {
      return false; // Malformed locations are rejected by their callers.
    }
  }

  // For workers about to dereference a path: detect a local symlink to a
  // blocked volume without calling realpath() through that volume.
  bool ContainsResolved(const std::string &path) const {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (roots.empty())
        return false;
    }
    if (Contains(path))
      return true;
    Location location;
    try {
      location = ParseLocation(path);
    } catch (const std::runtime_error &) {
      return false;
    }
    std::string remaining = location.Archive() ? location.archive :
                            location.Local() ? location.path : "";
    if (remaining.empty() || remaining.front() != '/')
      return false;
    std::string prefix;
    for (int followed = 0; !remaining.empty() && followed < 40;) {
      const auto slash = remaining.find('/');
      const auto component = remaining.substr(0, slash);
      remaining.erase(0, slash == std::string::npos ? remaining.size() : slash + 1);
      if (component.empty() || component == ".")
        continue;
      if (component == "..") {
        if (!prefix.empty())
          prefix.resize(prefix.find_last_of('/'));
        continue;
      }
      const auto candidate = prefix + "/" + component;
      if (Contains(candidate))
        return true;
      char link[PATH_MAX];
      const auto bytes = readlink(candidate.c_str(), link, sizeof(link));
      if (bytes <= 0) {
        prefix = candidate;
        continue;
      }
      if (link[0] == '/')
        prefix.clear();
      remaining = std::string(link, bytes) + "/" + remaining;
      ++followed;
    }
    return false;
  }

  bool ContainsDevice(int64_t device) const {
    std::lock_guard<std::mutex> lock(mutex);
    return devices.count(device) != 0;
  }

private:
  using Parts = std::vector<std::string>;

  static Parts Components(const std::string &path) {
    return Components(path, [](const Parts &) { return false; }, false);
  }

  template<class Match>
  static Parts Components(const std::string &path, Match match, bool matchOnly = true) {
    Parts parts;
    if (path.empty() || path.front() != '/')
      return parts;
    for (size_t start = 1; start < path.size();) {
      const auto slash = path.find('/', start);
      const auto component = path.substr(start, slash - start);
      if (component == "..") {
        if (!parts.empty())
          parts.pop_back();
      } else if (!component.empty() && component != ".") {
        parts.push_back(component);
        if (match(parts))
          return parts;
      }
      if (slash == std::string::npos)
        break;
      start = slash + 1;
    }
    return matchOnly ? Parts{} : parts;
  }

  mutable std::mutex mutex;
  std::set<Parts> roots;
  std::set<int64_t> devices;
};

inline UnavailableVolumes &InterruptedVolumes() {
  static UnavailableVolumes registry;
  return registry;
}
} // namespace Listi
