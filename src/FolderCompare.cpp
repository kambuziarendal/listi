// SPDX-License-Identifier: MIT
#include "FolderCompare.h"
#include "ArchiveVolumes.h"
#include "Sha256.h"
#include "UnavailableVolume.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
namespace Listi {
namespace {
void Need(bool ok, const std::string &s) {
  if (!ok)
    throw std::runtime_error(s);
}
struct Item {
  FileEntry entry;
  struct stat native {};
  bool local = false;
  std::string link;
};
using Tree = std::map<std::string, Item>;
bool Stable(const struct stat &a, const struct stat &b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
         a.st_mode == b.st_mode && a.st_size == b.st_size &&
         a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
         a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
         a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
         a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
Item Inspect(FileEntry e) {
  Need(!e.sizeKnown || e.size >= 0,
       "Invalid file size reported for: " + e.path);
  Item i;
  i.entry = e;
  i.local = ParseLocation(e.path).Local();
  if (i.local) {
    Need(!lstat(e.path.c_str(), &i.native),
         "Could not inspect " + e.path + ": " + strerror(errno));
    Need(e.directory == bool(S_ISDIR(i.native.st_mode)) &&
             e.link == bool(S_ISLNK(i.native.st_mode)) &&
             (!e.sizeKnown || e.size == i.native.st_size),
         "Entry changed while listing: " + e.path);
    if (e.link) {
      std::vector<char> target(65536);
      auto n = readlink(e.path.c_str(), target.data(), target.size());
      Need(n >= 0 && size_t(n) < target.size(),
           "Could not read link: " + e.path);
      i.link.assign(target.data(), n);
    }
  }
  return i;
}
Tree Scan(std::string root, size_t limit,
          const TransferControl &control) {
  // lstat("link/") follows the directory link even though lstat("link")
  // identifies the selected entry. Keep trailing slash spelling equivalent.
  if (ParseLocation(root).Local())
    while (root.size() > 1 && root.back() == '/')
      root.pop_back();
  control.Check();
  Need(!InterruptedVolumes().ContainsResolved(root),
       root + ": " + kUnavailableVolumeText);
  FileEntry first;
  Need(StatFile(root, first, control) && first.directory && !first.link,
       "Choose an existing folder, not a symbolic link: " + root);
  first.path = root;
  Tree out;
  out.emplace("", Inspect(first));
  struct Pending {
    std::string path, relative;
    unsigned depth;
  };
  std::vector<Pending> pending{{root, "", 0}};
  while (!pending.empty()) {
    control.Check();
    auto p = pending.back();
    pending.pop_back();
    Need(p.depth < 256,
         "Folder nesting limit reached. Comparison is incomplete.");
    std::vector<std::string> omitted;
    auto entries = ListFiles(p.path, control, &omitted);
    if (!omitted.empty())
      throw std::runtime_error(omitted.front() + ": " + kUnavailableVolumeText);
    for (auto &e : entries) {
      control.Check();
      Need(!e.name.empty() && e.name != "." && e.name != ".." &&
               e.name.find('/') == std::string::npos,
           "Ambiguous name in folder listing.");
      auto relative = p.relative.empty() ? e.name : p.relative + "/" + e.name;
      e.path = ChildLocation(p.path, e.name);
      Need(out.size() - 1 < limit, // The root is only a validation sentinel.
           "Entry limit reached. Comparison is incomplete.");
      Need(out.emplace(relative, Inspect(e)).second,
           "Duplicate name in folder listing: " + relative);
      if (e.directory && !e.link)
        pending.push_back({e.path, relative, p.depth + 1});
    }
  }
  return out;
}
bool Same(const Item &a, const Item &b) {
  if (a.local && b.local)
    return Stable(a.native, b.native) && a.link == b.link;
  auto &x = a.entry;
  auto &y = b.entry;
  return x.directory == y.directory && x.link == y.link &&
         x.sizeKnown == y.sizeKnown && (!x.sizeKnown || x.size == y.size) &&
         x.SameModification(y);
}
void Validate(const Tree &before, const Tree &after) {
  Need(before.size() == after.size(),
       "A folder changed during comparison. Run it again.");
  for (const auto &p : before) {
    auto f = after.find(p.first);
    Need(f != after.end() && Same(p.second, f->second),
         "An entry changed during comparison: " + p.second.entry.path);
  }
}
std::string Digest(const Item &i, const TransferControl &control) {
  if (i.local) {
    struct stat now;
    Need(!lstat(i.entry.path.c_str(), &now) && Stable(i.native, now),
         "File changed before reading: " + i.entry.path);
    Need(S_ISREG(now.st_mode),
         "Special file cannot be compared: " + i.entry.path);
  }
  Item expected = i;
  if (ParseLocation(i.entry.path).scheme == "sftp") {
    // Directory listings cannot supply an exact SFTP time. Compare two
    // file stats around the read without weakening the listing's type/size check.
    FileEntry before;
    Need(StatFile(i.entry.path, before, control),
         "File changed before reading: " + i.entry.path);
    expected.entry.modified = before.modified;
    expected.entry.modifiedKnown = before.modifiedKnown;
    expected.entry.modifiedFraction = before.modifiedFraction;
    Need(Same(expected, Inspect(before)),
         "File changed before reading: " + i.entry.path);
  }
  Sha256 hash;
  uint64_t count = 0;
  ReadFile(
      i.entry.path,
      [&](const char *p, size_t n) {
        control.Check();
        hash.Update(p, n);
        count += n;
      },
      control);
  Need(!i.entry.sizeKnown || count == uint64_t(i.entry.size),
       "Byte count changed while reading: " + i.entry.path);
  FileEntry after;
  Need(StatFile(i.entry.path, after, control) && Same(expected, Inspect(after)),
       "File changed while reading: " + i.entry.path);
  return hash.Finish();
}
std::string ArchiveGuard(const std::string &root) {
  auto location = ParseLocation(root);
  if (location.Archive() && InterruptedVolumes().ContainsResolved(root))
    throw std::runtime_error(root + ": " + kUnavailableVolumeText);
  return location.Archive() ? FindArchiveVolumes(location.archive).Fingerprint()
                            : std::string();
}
} // namespace
const char *CompareLabel(CompareState s) {
  switch (s) {
  case CompareState::OnlyLeft:
    return "Only in left";
  case CompareState::OnlyRight:
    return "Only in right";
  case CompareState::TypeDifferent:
    return "Different entry types";
  case CompareState::SizeDifferent:
    return "Different sizes";
  case CompareState::ContentDifferent:
    return "Different content";
  case CompareState::SizeMatch:
    return "Same size, content unchecked";
  case CompareState::Verified:
    return "SHA-256 match";
  case CompareState::DirectoryMatch:
    return "Folder in both";
  case CompareState::LinkMatch:
    return "Same link target (not followed)";
  case CompareState::Unverified:
    return "Not verified";
  case CompareState::Error:
    return "Read/check error";
  }
  return "Unknown";
}
bool CompareMatch(CompareState s) {
  return s == CompareState::Verified || s == CompareState::SizeMatch ||
         s == CompareState::DirectoryMatch || s == CompareState::LinkMatch;
}
CompareRows SelectCompareRows(const CompareResult &result, bool showMatches,
                              size_t limit) {
  const auto &entries = result.entries;
  const size_t nonmatching = std::count_if(
      entries.begin(), entries.end(),
      [](const CompareEntry &entry) { return !CompareMatch(entry.state); });
  CompareRows selected;
  selected.total = showMatches ? entries.size() : nonmatching;
  selected.indices.reserve(std::min(limit, selected.total));
  // Reserve space for differences and unverified entries before adding matches.
  size_t matchingSlots = showMatches ? limit - std::min(limit, nonmatching) : 0;
  for (size_t i = 0; i < entries.size() && selected.indices.size() < limit;
       ++i) {
    if (CompareMatch(entries[i].state)) {
      if (!matchingSlots)
        continue;
      --matchingSlots;
    }
    selected.indices.push_back(i);
  }
  return selected;
}
CompareResult CompareFolders(const CompareRequest &r,
                             const TransferControl &control) {
  CompareResult result;
  result.checksum = r.checksum;
  try {
    Need(r.entryLimit > 0 && !r.left.empty() && !r.right.empty(),
         "Choose two folders to compare.");
    auto leftGuard = ArchiveGuard(r.left), rightGuard = ArchiveGuard(r.right);
    auto left = Scan(r.left, r.entryLimit, control),
         right = Scan(r.right, r.entryLimit, control);
    std::set<std::string> names;
    for (auto &p : left)
      if (!p.first.empty())
        names.insert(p.first);
    for (auto &p : right)
      if (!p.first.empty())
        names.insert(p.first);
    for (auto &name : names) {
      control.Check();
      CompareEntry e;
      e.relative = name;
      auto a = left.find(name), b = right.find(name);
      if (a != left.end()) {
        e.left = a->second.entry.path;
        if (!a->second.entry.directory && a->second.entry.sizeKnown)
          e.leftBytes = a->second.entry.size;
      }
      if (b != right.end()) {
        e.right = b->second.entry.path;
        if (!b->second.entry.directory && b->second.entry.sizeKnown)
          e.rightBytes = b->second.entry.size;
      }
      if (a == left.end())
        e.state = CompareState::OnlyRight;
      else if (b == right.end())
        e.state = CompareState::OnlyLeft;
      else {
        auto &x = a->second;
        auto &y = b->second;
        if (x.entry.directory != y.entry.directory ||
            x.entry.link != y.entry.link ||
            (x.local && y.local &&
             (x.native.st_mode & S_IFMT) != (y.native.st_mode & S_IFMT)))
          e.state = CompareState::TypeDifferent;
        else if (x.entry.link) {
          if (x.local && y.local)
            e.state = x.link == y.link ? CompareState::LinkMatch
                                       : CompareState::ContentDifferent;
          else {
            e.state = CompareState::Unverified;
            e.detail = "Link contents are not exposed by this provider. Links "
                       "are never followed.";
          }
        } else if (x.entry.directory)
          e.state = CompareState::DirectoryMatch;
        else if ((x.local && !S_ISREG(x.native.st_mode)) ||
                 (y.local && !S_ISREG(y.native.st_mode))) {
          e.state = CompareState::Unverified;
          e.detail = "Special files are not opened.";
        } else if (x.entry.sizeKnown && y.entry.sizeKnown &&
                   x.entry.size != y.entry.size)
          e.state = CompareState::SizeDifferent;
        else if (!r.checksum)
          e.state = x.entry.sizeKnown && y.entry.sizeKnown
                        ? CompareState::SizeMatch
                        : CompareState::Unverified;
        else {
          try {
            e.leftHash = Digest(x, control);
            e.rightHash = Digest(y, control);
            e.state = e.leftHash == e.rightHash
                          ? CompareState::Verified
                          : CompareState::ContentDifferent;
          } catch (const Cancelled &) {
            throw;
          } catch (const std::exception &error) {
            e.state = CompareState::Error;
            e.detail = error.what();
          }
        }
      }
      if (e.state == CompareState::Verified)
        ++result.verified;
      else if (e.state == CompareState::SizeMatch)
        ++result.sizeOnly;
      else if (e.state == CompareState::Unverified ||
               e.state == CompareState::Error)
        ++result.unverified;
      else if (!CompareMatch(e.state))
        ++result.different;
      result.entries.push_back(std::move(e));
    }
    // Re-list names and identities so missing/unreadable or changing trees
    // never turn into a successful comparison of a partial scan.
    Validate(left, Scan(r.left, r.entryLimit, control));
    Validate(right, Scan(r.right, r.entryLimit, control));
    Need(leftGuard == ArchiveGuard(r.left) &&
             rightGuard == ArchiveGuard(r.right),
         "An archive changed during comparison. Run it again.");
    result.complete = result.unverified == 0;
    if (result.unverified)
      result.error = "Some entries could not be verified. See the result rows.";
  } catch (const Cancelled &) {
    result.cancelled = true;
    result.error = "Comparison cancelled. Results are incomplete.";
  } catch (const std::exception &e) {
    result.error = e.what();
  }
  return result;
}
} // namespace Listi
