// SPDX-License-Identifier: MIT
#pragma once
#include "Sha256.h"
#include "SftpFile.h"
#include "UnavailableVolume.h"
#include "VirtualFile.h"
#include <algorithm>
#include <cerrno>
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
class LocalTransferSelection {
public:
  using Names = std::set<std::string>;
  bool Empty() const { return folders.empty(); }

private:
  struct Folder {
    std::string path;
    bool entireTree;
    Names names;
    Names mutations;
  };
  std::vector<Folder> folders;
  std::map<std::string, std::vector<size_t>> containingName, candidatesByRemoteParent;
  std::set<std::pair<size_t, std::string>> distinctFolders;
  size_t comparisons = 0;
  uint64_t witnessBytesRemaining = 32 * 1024 * 1024;
  std::set<std::string> fileParents;
  std::map<std::string, size_t> outputParents;
  std::set<std::pair<dev_t, ino_t>> selectedFiles;
  size_t entries = 0;

  void AddFolder(const std::string &path, bool entireTree, const TransferControl &control,
                 unsigned depth = 0) {
    control.Check();
    if (depth > 128 || InterruptedVolumes().ContainsResolved(path))
      throw std::runtime_error("Cannot inspect local transfer folder: " + path);
    Names names;
    // ListFiles intentionally hides private staging and unavailable entries;
    // the identity witness needs the raw names, including dotfiles.
    for (const auto &entry : std::filesystem::directory_iterator(path)) {
      control.Check();
      if (++entries > 200000)
        throw std::runtime_error("Local transfer identity check exceeds the entry limit.");
      names.insert(entry.path().filename().string());
      if (entireTree && std::filesystem::is_directory(entry.symlink_status()))
        AddFolder(entry.path().string(), true, control, depth + 1);
    }
    for (const auto &name : names)
      containingName[name].push_back(folders.size());
    folders.push_back({path, entireTree, std::move(names), {}});
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
      throw std::runtime_error("Local transfer identity check exceeds the comparison limit.");
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

  std::string WitnessDigest(const std::string &path, uint64_t expected,
                            const TransferControl &control) {
    FileEntry current;
    if (!StatFile(path, current, control) || current.directory || current.link ||
        !current.sizeKnown || current.size < 0 || uint64_t(current.size) != expected)
      throw std::runtime_error("File changed during transfer preflight: " + path);
    if (!IsVirtual(path)) {
      struct stat st;
      if (lstat(path.c_str(), &st) || !S_ISREG(st.st_mode))
        throw std::runtime_error("File changed during transfer preflight: " + path);
    }
    Sha256 hash;
    ReadFile(path, [&](const char *bytes, size_t size) {
      if (size > expected)
        throw std::runtime_error("File changed during transfer preflight: " + path);
      expected -= size;
      witnessBytesRemaining -= size;
      hash.Update(bytes, size);
    }, control);
    if (expected)
      throw std::runtime_error("File changed during transfer preflight: " + path);
    return hash.Finish();
  }

  // Missing listed names already ruled out this candidate. Inspect other
  // children too: an identical file alone does not identify its parent.
  bool DifferentFolder(size_t index, const std::string &remoteParent,
                       const Names &listed, const TransferControl &control, bool compareContent = false) {
    const auto &candidate = folders[index];
    const auto key = std::make_pair(index, remoteParent);
    if (distinctFolders.count(key))
      return true;
    std::vector<std::pair<std::string, uint64_t>> smallFiles;
    for (const auto &name : candidate.names) {
      CompareStep(control);
      const bool visible = listed.count(name) != 0;
      if (!visible && name.front() == '.')
        continue; // A server may hide private entries; omission is not proof.
      const auto remotePath = ChildLocation(remoteParent, name);
      const auto local = ChildLocation(candidate.path, name);
      struct stat st;
      if (lstat(local.c_str(), &st))
        throw std::runtime_error("Local directory view changed: " + local);
      if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
        continue; // An unselected special entry is not an identity witness.
      FileEntry remote;
      try {
        if (!StatFile(remotePath, remote, control)) {
          if (visible)
            throw std::runtime_error("SFTP directory view changed: " + remoteParent);
          distinctFolders.insert(key);
          return true;
        }
      } catch (const UnsupportedSftpEntry &) {
        continue; // Do not rely on entries whose remote type is unspecified.
      }
      if (remote.directory != bool(S_ISDIR(st.st_mode)) ||
          remote.link != bool(S_ISLNK(st.st_mode)) ||
          (S_ISREG(st.st_mode) && remote.sizeKnown && remote.size != st.st_size)) {
        distinctFolders.insert(key);
        return true;
      }
      if (compareContent && S_ISREG(st.st_mode) && remote.sizeKnown &&
          st.st_size > 0 && st.st_size <= 1024 * 1024)
        smallFiles.emplace_back(name, st.st_size);
    }
    for (const auto &[name, size] : smallFiles) {
      CompareStep(control);
      if (size > witnessBytesRemaining / 2)
        continue;
      const auto localHash = WitnessDigest(ChildLocation(candidate.path, name), size, control);
      if (localHash != WitnessDigest(ChildLocation(remoteParent, name), size, control)) {
        distinctFolders.insert(key);
        return true;
      }
    }
    return false;
  }

  bool AffectsSelectedLeaf(const Folder &candidate, const std::string &leaf,
                           const TransferControl &control) {
    if (candidate.mutations.count(leaf))
      return true;

    // An exact different spelling may still name the same local file.
    struct stat selected;
    if (lstat(ChildLocation(candidate.path, leaf).c_str(), &selected)) {
      if (errno == ENOENT)
        return false;
      throw std::runtime_error("Cannot check selected file alias: " + candidate.path);
    }
    for (const auto &name : candidate.mutations) {
      CompareStep(control);
      struct stat output;
      if (lstat(ChildLocation(candidate.path, name).c_str(), &output)) {
        if (errno == ENOENT)
          continue;
        throw std::runtime_error("Cannot check local output alias: " + candidate.path);
      }
      if (selected.st_dev == output.st_dev && selected.st_ino == output.st_ino)
        return true;
    }
    return false;
  }

public:
  // Index only existing local parents of projected mutations. For a new
  // output tree, its first mkdir is the only possible entry into a source.
  void AddOutput(const std::string &target, const TransferControl &control) {
    const auto parent = ParentLocation(target);
    const auto [it, added] = outputParents.try_emplace(parent, folders.size());
    if (added)
      AddFolder(parent, false, control);
    folders[it->second].mutations.insert(LocationLeaf(target));
  }

  // With a selected file, only a replacement of that file is disallowed.
  // With a selected tree, creating even a new name inside it is disallowed.
  void CheckSourceFolder(const Names &names, const std::string &folder,
                         const std::string &selectedLeaf, const TransferControl &control) {
    for (const auto index : PossibleParents(folder, names, control)) {
      CompareStep(control);
      const auto &candidate = folders[index];
      if (!selectedLeaf.empty()) {
        if (!AffectsSelectedLeaf(candidate, selectedLeaf, control))
          continue;
        const auto local = ChildLocation(candidate.path, selectedLeaf);
        struct stat st;
        FileEntry remote;
        if (lstat(local.c_str(), &st) || !S_ISREG(st.st_mode) ||
            !StatFile(ChildLocation(folder, selectedLeaf), remote, control) ||
            remote.directory || remote.link)
          throw std::runtime_error("Cannot establish selected SFTP file identity: " + local);
        if (remote.sizeKnown && remote.size != st.st_size)
          continue;
      }
      if (DifferentFolder(index, folder, names, control, true))
        continue;
      throw std::runtime_error("Cannot distinguish the local destination from a selected SFTP "
                               "source. Choose a separate destination: " + candidate.path);
    }
  }

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
        if (!candidate.entireTree)
          continue;
      } else {
        struct stat st;
        if (lstat(local.c_str(), &st))
          throw std::runtime_error("Local directory view changed before upload: " + local);
        if (!candidate.entireTree && !selectedFiles.count({st.st_dev, st.st_ino}))
          continue;
        const bool regular = S_ISREG(st.st_mode) && !existing->directory && !existing->link;
        if (regular && existing->sizeKnown && existing->size != st.st_size)
          continue;
        if (st.st_nlink == 1 && DifferentFolder(index, parent, names, control, true))
          continue;
        if (regular) {
          // Two separate folders can both contain just 'memo'. Different
          // bytes rule out the candidate without trusting timestamps.
          if (!targetHash) {
            FileEntry current;
            if (!StatFile(target, current, control) || current.directory || current.link ||
                !existing->SameRevision(current))
              throw std::runtime_error("File changed during transfer preflight: " + target);
            targetHash = digest(target, control);
          }
          if (lstat(local.c_str(), &st) || !S_ISREG(st.st_mode))
            throw std::runtime_error("File changed during transfer preflight: " + local);
          if (digest(local, control) != *targetHash)
            continue;
        }
      }
      if (!existing && DifferentFolder(index, parent, names, control))
        continue;
      throw std::runtime_error("Cannot distinguish the SFTP destination from a selected local "
                               "source. Choose a separate destination: " + target);
    }
  }
};
} // namespace Listi
