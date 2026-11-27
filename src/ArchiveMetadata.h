// SPDX-License-Identifier: MIT
#pragma once
#include "VirtualFile.h"
#include <map>
#include <string_view>
#include <utility>

namespace Listi {
// Per scan, separate from decoded data and from the completed listing cache.
// Fixed costs cover record storage and conservative map-node overhead. String
// costs allow capacity growth and include terminators. This cannot account for
// allocations inside a system codec or bound simultaneous independent scans.
class ArchiveMetadataBudget {
public:
  static constexpr size_t DefaultLimit = 64 * 1024 * 1024;
  static constexpr size_t NodeOverhead = 64;

  explicit ArchiveMetadataBudget(size_t limit = DefaultLimit)
      : remaining(limit) {}

  void Charge(size_t bytes) {
    if (bytes > remaining)
      throw std::runtime_error("Archive metadata exceeds the safe listing limit");
    remaining -= bytes;
  }

private:
  size_t remaining;
};

class ArchiveListingChildren {
public:
  ArchiveListingChildren(const Location &location, ArchiveMetadataBudget &budget)
      : budget(budget), url(location.URL()), folder(location.path.substr(1)) {}

  std::string_view RelativeMember(std::string_view name, bool directory) {
    if (folder.empty())
      return name;
    if (name.compare(0, folder.size(), folder) != 0)
      return {};
    if (name.size() == folder.size()) {
      if (!directory)
        throw std::runtime_error("Archive member is not a folder.");
      explicitDirectory = true;
      return {};
    }
    if (name[folder.size()] != '/')
      return {};
    return name.substr(folder.size() + 1);
  }

  FileEntry &Get(std::string_view leaf, bool *inserted = nullptr) {
    auto found = children.find(leaf);
    if (inserted)
      *inserted = found == children.end();
    if (found != children.end())
      return found->second;

    // One key, one display name and one URL. Escaping takes at most three
    // bytes per leaf byte. Charge before building either owning string.
    budget.Charge(sizeof(decltype(children)::value_type) +
                  ArchiveMetadataBudget::NodeOverhead);
    budget.Charge(2 * (url.size() + 5 * leaf.size() + 4));
    std::string name(leaf);
    FileEntry item;
    item.name = name;
    item.path = ChildLocation(url, name);
    return children.emplace(std::move(name), std::move(item)).first->second;
  }

  std::vector<FileEntry> Finish() {
    if (!folder.empty() && !explicitDirectory && children.empty())
      throw std::runtime_error("Archive folder does not exist.");
    budget.Charge(children.size() * sizeof(FileEntry));
    std::vector<FileEntry> out;
    out.reserve(children.size());
    for (auto &child : children)
      out.push_back(std::move(child.second));
    return out;
  }

private:
  ArchiveMetadataBudget &budget;
  std::string url;
  std::string folder;
  bool explicitDirectory = false;
  std::map<std::string, FileEntry, std::less<>> children;
};
} // namespace Listi
