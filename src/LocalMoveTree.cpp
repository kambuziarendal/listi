// SPDX-License-Identifier: MIT
#include "LocalMoveTree.h"
#include "MoveVolume.h"
#include "ArchiveMetadata.h"
#include "VirtualFile.h"
#include <Node.h>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fs_attr.h>
#include <memory>
#include <unistd.h>

namespace Listi {
namespace {
void Require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
void Check(const Hooks &hooks) {
  if (hooks.proceed && !hooks.proceed())
    throw Cancelled();
}
bool Identity(const struct stat &a, const struct stat &b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_mode == b.st_mode;
}
bool Same(const struct stat &a, const struct stat &b) {
  return Identity(a, b) && a.st_uid == b.st_uid && a.st_gid == b.st_gid && a.st_nlink == b.st_nlink && a.st_size == b.st_size &&
         a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
         a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
         a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
         a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
void Portable(const std::string &path, const struct stat &st) {
  Require(!S_ISREG(st.st_mode) || st.st_nlink == 1,
          "Archive moves do not preserve hard links. Use Copy: " + path);
  Require(!(st.st_mode & 07000),
          "Archive moves do not preserve special permission bits. Use Copy: " +
              path);
  BNode node(path.c_str());
  char name[B_ATTR_NAME_LENGTH];
  Require(node.InitCheck() == B_OK &&
              node.GetNextAttrName(name) == B_ENTRY_NOT_FOUND,
          "Archive moves do not preserve filesystem attributes. Use Copy to "
          "retain the original: " +
              path);
}
void FlushPath(const std::string &path) {
  int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  Require(fd >= 0, "Cannot open moved destination for flushing: " + path);
  const int status = fsync(fd);
  close(fd);
  Require(!status, "Cannot flush moved destination: " + path);
}
} // namespace
LocalMoveTree::LocalMoveTree(const std::vector<std::string> &roots,
                             const Hooks &hooks, bool portableSource,
                             std::optional<dev_t> requiredVolume)
    : portable(portableSource) {
  for (const auto &selected : roots) {
    struct stat st;
    Require(!lstat(selected.c_str(), &st) && !S_ISLNK(st.st_mode),
            "Move entry disappeared or is a link: " + selected);
    if (requiredVolume)
      RequireSameMoveVolume(st.st_dev, *requiredVolume);
    char *resolved = realpath(selected.c_str(), nullptr);
    const std::string root = resolved ? resolved : "";
    free(resolved);
    Require(!root.empty(), "Cannot resolve move entry: " + selected);
    aliases.emplace(selected, root);
    for (auto parent = ParentLocation(root); !parent.empty();) {
      struct stat p;
      Require(!lstat(parent.c_str(), &p) && S_ISDIR(p.st_mode),
              "Move parent is not a real folder: " + parent);
      parents.emplace(parent, p);
      if (parent == "/")
        break;
      parent = ParentLocation(parent);
    }
    Capture(root, st.st_dev, hooks, 0);
  }
  Verify();
}
void LocalMoveTree::Capture(const std::string &path, dev_t volume,
                            const Hooks &hooks, unsigned depth) {
  Check(hooks);
  Require(depth <= 64 && entries.size() < 200000,
          "Move exceeds the folder or entry limit.");
  struct stat st;
  Require(!lstat(path.c_str(), &st) && st.st_dev == volume &&
              (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)),
          "Archive moves require ordinary files and folders on one volume, "
          "without links: " +
              path);
  if (portable)
    Portable(path, st);
  Require(entries.emplace(path, st).second,
          "Overlapping move selections are not supported.");
  if (!S_ISDIR(st.st_mode))
    return;
  std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(path.c_str()),
                                                closedir);
  Require(bool(dir), "Cannot list move folder: " + path);
  for (;;) {
    errno = 0;
    auto child = readdir(dir.get());
    if (!child) {
      Require(!errno, "Cannot finish listing move folder: " + path);
      break;
    }
    if (!strcmp(child->d_name, ".") || !strcmp(child->d_name, ".."))
      continue;
    Capture(Join(path, child->d_name), volume, hooks, depth + 1);
  }
}
void LocalMoveTree::GuardParents() const {
  for (const auto &alias : aliases) {
    if (removed.count(alias.first))
      continue;
    char *resolved = realpath(alias.first.c_str(), nullptr);
    const std::string actual = resolved ? resolved : "";
    free(resolved);
    Require(actual == alias.second,
            "Move path changed during operation: " + alias.first);
  }
  for (const auto &entry : parents) {
    struct stat now;
    Require(!lstat(entry.first.c_str(), &now) && Identity(entry.second, now),
            "A move parent folder changed. Source cleanup stopped: " +
                entry.first);
  }
}
void LocalMoveTree::Verify() const {
  GuardParents();
  for (const auto &entry : entries) {
    struct stat now;
    Require(!lstat(entry.first.c_str(), &now) && Same(entry.second, now),
            "Move tree changed. Source cleanup stopped: " + entry.first);
    if (portable)
      Portable(entry.first, now);
  }
}
// Use the original frozen stat records, not the private staging files.
TarMemberMap LocalMoveTree::TarMetadata(const std::string &targetFolder) const {
  TarMemberMap metadata;
  ArchiveMetadataBudget budget;
  for (const auto &[selected, root] : aliases) {
    const auto leaf = LocationLeaf(selected);
    const auto target = targetFolder.empty() ? leaf : Join(targetFolder, leaf);
    auto record = [&](const std::string &name, const struct stat &st) {
      budget.Charge(sizeof(TarMemberMap::value_type) + ArchiveMetadataBudget::NodeOverhead + name.size());
      TarMemberMetadata member{st.st_uid, st.st_gid, {}, {}, bool(S_ISDIR(st.st_mode)),
                               unsigned(st.st_mode) & 07777, st.st_mtim.tv_sec, st.st_mtim.tv_nsec};
      Require(metadata.emplace(name, std::move(member)).second,
              "Conflicting local move destinations: " + name);
    };
    record(target, entries.at(root));
    const auto prefix = root + "/";
    for (auto it = entries.lower_bound(prefix);
         it != entries.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
      record(target + it->first.substr(root.size()), it->second);
  }
  return metadata;
}
void LocalMoveTree::Remove(const std::string &selected, const Hooks &hooks) {
  const auto &root = aliases.at(selected);
  // Verify all roots before the caller starts cleanup. During cleanup, parent
  // timestamps legitimately change. Still guard every inode and regular file.
  for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
    const auto &path = it->first;
    if (path != root && path.compare(0, root.size() + 1, root + "/") != 0)
      continue;
    Check(hooks);
    GuardParents();
    for (auto parent = ParentLocation(path); entries.count(parent);
         parent = ParentLocation(parent)) {
      struct stat now;
      Require(!lstat(parent.c_str(), &now) && Identity(entries.at(parent), now),
              "Source parent changed during cleanup: " + parent);
    }
    struct stat now;
    const bool directory = S_ISDIR(it->second.st_mode);
    Require(!lstat(path.c_str(), &now) &&
                (directory ? Identity(it->second, now) : Same(it->second, now)),
            "Source changed during cleanup: " + path);
    if (portable)
      Portable(path, now);
    Require(!(directory ? rmdir(path.c_str()) : unlink(path.c_str())),
            "Source cleanup was incomplete. Verified destination retained: " +
                path);
  }
  removed.insert(selected);
  FlushPath(ParentLocation(root));
}
} // namespace Listi
