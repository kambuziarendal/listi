// SPDX-License-Identifier: MIT
#pragma once
#include "UnavailableVolume.h"
#include "VirtualFile.h"
#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <sys/stat.h>

namespace Listi {
// SFTP v3 does not expose a portable device/inode. Compare directory views,
// not absolute paths: a local daemon may chroot or bind-mount elsewhere.
// Equal views are inconclusive, not proof of identity. A filesystem-like
// server must actually expose the names and file data it will mutate.
class LocalUploadSelection {
public:
  using Names = std::set<std::string>;
  bool Empty() const { return folders.empty(); }

private:
  struct Folder {
    std::string path;
    bool entireTree;
    Names names;
  };
  std::vector<Folder> folders;
  std::map<std::string, std::vector<size_t>> containingName, candidatesByRemoteParent;
  size_t comparisons = 0;
  std::set<std::string> fileParents;
  std::set<std::pair<dev_t, ino_t>> selectedFiles;
  size_t entries = 0;

  void AddFolder(const std::string &path, bool entireTree, const TransferControl &control,
                 unsigned depth = 0) {
    control.Check();
    if (depth > 128 || InterruptedVolumes().ContainsResolved(path))
      throw std::runtime_error("Cannot inspect selected upload folder: " + path);
    Names names;
    // ListFiles intentionally hides private staging and unavailable entries;
    // the identity witness needs the raw names, including dotfiles.
    for (const auto &entry : std::filesystem::directory_iterator(path)) {
      control.Check();
      if (++entries > 200000)
        throw std::runtime_error("Local upload identity check exceeds the entry limit.");
      names.insert(entry.path().filename().string());
      if (entireTree && std::filesystem::is_directory(entry.symlink_status()))
        AddFolder(entry.path().string(), true, control, depth + 1);
    }
    for (const auto &name : names)
      containingName[name].push_back(folders.size());
    folders.push_back({path, entireTree, std::move(names)});
  }

public:
  void Add(const std::string &source, const FileEntry &entry, const TransferControl &control) {
    if (entry.directory) {
      AddFolder(source, true, control);
    } else {
      struct stat st;
      if (lstat(source.c_str(), &st) || !S_ISREG(st.st_mode))
        throw std::runtime_error("Cannot protect selected upload file: " + source);
      selectedFiles.emplace(st.st_dev, st.st_ino);
      const auto parent = std::filesystem::absolute(source).parent_path().string();
      if (fileParents.insert(parent).second)
        AddFolder(parent, false, control);
    }
  }

private:
  void CompareStep(const TransferControl &control) {
    control.Check();
    if (++comparisons > 2000000)
      throw std::runtime_error("Local upload identity check exceeds the comparison limit.");
  }

  const std::vector<size_t> &PossibleParents(const std::string &parent, const Names &remote,
                                              const TransferControl &control) {
    const auto inserted = candidatesByRemoteParent.try_emplace(parent);
    auto &possible = inserted.first->second;
    if (!inserted.second)
      return possible;
    // Index by a rare visible child name. A positive name absent locally
    // rules out a folder; absence from a filtered READDIR does not.
    const std::vector<size_t> *shortlist = nullptr;
    for (const auto &name : remote) {
      CompareStep(control);
      const auto found = containingName.find(name);
      if (found == containingName.end())
        return possible;
      if (!shortlist || found->second.size() < shortlist->size())
        shortlist = &found->second;
    }
    for (size_t n = 0; n < (shortlist ? shortlist->size() : folders.size()); ++n) {
      CompareStep(control);
      const auto index = shortlist ? (*shortlist)[n] : n;
      if (std::all_of(remote.begin(), remote.end(), [&](const auto &name) {
            CompareStep(control);
            return folders[index].names.count(name);
          }))
        possible.push_back(index);
    }
    return possible;
  }

  bool DisjointForNewName(const Folder &candidate, const std::string &remoteParent,
                          const Names &listed, const TransferControl &control) {
    // Probe a regular, unlisted name directly before trusting an apparently
    // separate (often empty) upload directory. Never treat the omission of a
    // dotfile as proof. An inaccessible/indeterminate LSTAT aborts preflight.
    for (const auto &name : candidate.names) {
      CompareStep(control);
      if (name.front() == '.' || listed.count(name))
        continue;
      FileEntry entry;
      if (!StatFile(ChildLocation(remoteParent, name), entry, control))
        return true;
    }
    return false;
  }

public:
  using Digest = std::string (*)(const std::string &, const TransferControl &);
  void CheckParent(const Names &names, const std::string &target, const FileEntry *existing,
                   const TransferControl &control, Digest digest) {
    const auto parent = ParentLocation(target);
    std::optional<std::string> targetHash;
    for (const auto index : PossibleParents(parent, names, control)) {
      CompareStep(control);
      const auto &candidate = folders[index];
      const auto local = ChildLocation(candidate.path, LocationLeaf(target));
      if (!existing) {
        // A new name cannot replace a selected standalone file. But creating
        // even an empty folder or stage inside a selected tree is a mutation.
        if (!candidate.entireTree || DisjointForNewName(candidate, parent, names, control))
          continue;
      } else {
        struct stat st;
        if (lstat(local.c_str(), &st))
          throw std::runtime_error("Local directory view changed before upload: " + local);
        if (!candidate.entireTree && !selectedFiles.count({st.st_dev, st.st_ino}))
          continue;
        if (S_ISREG(st.st_mode) && !existing->directory && !existing->link) {
          if (existing->sizeKnown && existing->size != st.st_size)
            continue;
          // Two separate folders can both contain just 'memo'. Different
          // bytes rule out the candidate without trusting timestamps.
          if (!targetHash)
            targetHash = digest(target, control);
          if (digest(local, control) != *targetHash)
            continue;
        }
      }
      throw std::runtime_error("Cannot distinguish the SFTP destination from a selected local "
                               "source. Choose a separate destination: " + target);
    }
  }
};
} // namespace Listi
