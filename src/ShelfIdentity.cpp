// SPDX-License-Identifier: MIT
#include "ShelfIdentity.h"
#include "VirtualFile.h"
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Query.h>
#include <Volume.h>
#include <VolumeRoster.h>
#include <fs_info.h>
#include <algorithm>
#include <cstring>

namespace Listi {
namespace {
constexpr size_t kMaximumVolumes = 64;
constexpr size_t kDirectoryScanBudget = 4096;

ShelfNodeStamp Stamp(const struct stat &st) {
  return {int64(st.st_ino), int64(st.st_crtim.tv_sec),
          int32(st.st_crtim.tv_nsec)};
}

struct VolumeProof {
  std::string device, trash;
  ShelfNodeStamp root;
};

std::optional<VolumeProof> IdentifyVolume(dev_t device, bool withTrash = false) {
  fs_info info{};
  if (fs_stat_dev(device, &info) != 0 || strcmp(info.fsh_name, "bfs") ||
      !(info.flags & B_FS_IS_PERSISTENT) || !info.device_name[0])
    return std::nullopt;
  BDirectory root;
  BVolume mounted(device);
  struct stat st;
  if (mounted.GetRootDirectory(&root) != B_OK ||
      root.GetStat(&st) != B_OK || st.st_dev != device ||
      st.st_ino != info.root || st.st_crtim.tv_sec <= 0)
    return std::nullopt;
  BPath trash;
  std::string excluded;
  if (withTrash &&
      find_directory(B_TRASH_DIRECTORY, &trash, false, &mounted) == B_OK)
    excluded = trash.Path();
  return VolumeProof{info.device_name, std::move(excluded), Stamp(st)};
}

bool MatchesVolume(const ShelfProof &proof, const VolumeProof &volume) {
  return volume.device == proof.device && volume.root == proof.volumeRoot;
}

bool MatchesEntry(const ShelfProof &proof, const struct stat &st) {
  return (st.st_mode & S_IFMT) == proof.kind && Stamp(st) == proof.entry;
}

void Bind(ShelfEntry &entry, const struct stat &st) {
  entry.identity = {int32(st.st_dev), int64(st.st_ino)};
  entry.unresolved = false;
  entry.restoredState = ShelfState::Ready;
}

std::string LeafName(const std::string &path) {
  return path.substr(path.find_last_of('/') + 1);
}

bool SafeCandidate(const std::string &path, const std::string &trash) {
  if (path.size() < 2 ||
      (!trash.empty() &&
       (path == trash || path.rfind(trash + "/", 0) == 0)))
    return false;
  return !IsListiPrivateEntry(path);
}

// Follow the known parent directory, not a filesystem-wide scan. The shared
// budget caps total enumeration per refresh, including absent entries.
void ResolveInParent(ShelfEntry &entry, dev_t volume, size_t &budget,
                     const std::string &trash) {
  const auto &proof = *entry.proof;
  node_ref parentRef(volume, ino_t(proof.parent.inode));
  BDirectory parent(&parentRef);
  struct stat st;
  if (parent.GetStat(&st) != B_OK || st.st_dev != volume ||
      !(Stamp(st) == proof.parent))
    return;
  // Try the original leaf directly if only the parent moved. Otherwise scan
  // for the original BFS inode, verifying its birth time as well.
  const auto leaf = LeafName(entry.path);
  BEntry candidate(&parent, leaf.c_str(), false);
  const auto status = candidate.GetStat(&st);
  if (status != B_OK || st.st_dev != volume || !MatchesEntry(proof, st)) {
    BPath old;
    if (status == B_OK && candidate.GetPath(&old) == B_OK &&
        entry.path == old.Path())
      entry.restoredState = st.st_dev == volume ? ShelfState::Replaced
                                                : ShelfState::Ambiguous;
    for (;;) {
      if (!budget)
        return;
      --budget;
      if (parent.GetNextEntry(&candidate, false) != B_OK)
        return;
      if (candidate.GetStat(&st) == B_OK && st.st_dev == volume &&
          MatchesEntry(proof, st))
        break;
    }
  }
  BPath found;
  struct stat current;
  if (candidate.GetPath(&found) != B_OK ||
      (entry.path != found.Path() &&
       !SafeCandidate(found.Path(), trash)) ||
      candidate.GetStat(&current) != B_OK || current.st_dev != volume ||
      !MatchesEntry(proof, current))
    return;
  entry.path = found.Path();
  Bind(entry, current);
}

void ResolveByName(ShelfEntry &entry, dev_t device, size_t &budget,
                   const std::string &trash) {
  const auto leaf = LeafName(entry.path);
  // Query equality interprets glob metacharacters. Don't accidentally start
  // a broad wildcard query when an unusual leaf cannot be queried literally.
  if (!budget || leaf.find_first_of("*?[]\\") != std::string::npos)
    return;
  BVolume volume(device);
  BQuery query;
  if (!volume.KnowsQuery() || query.SetVolume(&volume) != B_OK ||
      query.PushAttr("name") != B_OK ||
      query.PushString(leaf.c_str()) != B_OK || query.PushOp(B_EQ) != B_OK ||
      query.Fetch() != B_OK)
    return;

  std::string matched;
  struct stat current {};
  ShelfNodeStamp parentStamp{};
  const auto &proof = *entry.proof;
  for (;;) {
    BEntry candidate;
    const auto status = query.GetNextEntry(&candidate, false);
    if (status == B_ENTRY_NOT_FOUND)
      break;
    if (status != B_OK || !budget)
      return;
    --budget;
    BPath path;
    struct stat st;
    if (candidate.GetPath(&path) != B_OK || LeafName(path.Path()) != leaf ||
        candidate.GetStat(&st) != B_OK ||
        st.st_dev != device || !MatchesEntry(proof, st))
      continue;
    if (!matched.empty() ||
        (entry.path != path.Path() &&
         !SafeCandidate(path.Path(), trash)))
      return;
    BEntry parent;
    struct stat parentStat;
    if (candidate.GetParent(&parent) != B_OK ||
        parent.GetStat(&parentStat) != B_OK || parentStat.st_dev != device ||
        parentStat.st_crtim.tv_sec <= 0)
      return;
    parentStamp = Stamp(parentStat);
    current = st;
    matched = path.Path();
  }

  if (matched.empty())
    return;
  entry.path = std::move(matched);
  entry.proof->parent = parentStamp;
  Bind(entry, current);
}
}

std::optional<ShelfProof> CaptureShelfProof(const std::string &path,
                                            const struct stat &entry) {
  const auto volume = IdentifyVolume(entry.st_dev);
  struct stat parent, current;
  auto slash = path.find_last_of('/');
  const auto folder = path.substr(0, slash ? slash : 1);
  if (!volume || entry.st_crtim.tv_sec <= 0 ||
      stat(folder.c_str(), &parent) != 0 || parent.st_dev != entry.st_dev ||
      parent.st_crtim.tv_sec <= 0 || lstat(path.c_str(), &current) != 0 ||
      current.st_dev != entry.st_dev || !(Stamp(current) == Stamp(entry)) ||
      (current.st_mode & S_IFMT) != (entry.st_mode & S_IFMT))
    return std::nullopt;
  return ShelfProof{volume->device, volume->root, Stamp(parent), Stamp(entry),
                    uint32(entry.st_mode & S_IFMT)};
}

ShelfState CheckShelfProof(const ShelfProof &proof, const struct stat &entry) {
  const auto volume = IdentifyVolume(entry.st_dev);
  if (!volume || !MatchesVolume(proof, *volume))
    return ShelfState::Ambiguous;
  return MatchesEntry(proof, entry) ? ShelfState::Ready : ShelfState::Replaced;
}

void ResolveShelfReferences(std::vector<ShelfEntry> &entries) {
  if (std::none_of(entries.begin(), entries.end(), [](const auto &entry) {
        return entry.restored && entry.proof;
      }))
    return;
  const auto original = entries;
  std::vector<std::pair<dev_t, VolumeProof>> volumes;
  BVolumeRoster roster;
  BVolume mounted;
  for (size_t i = 0; i < kMaximumVolumes &&
                     roster.GetNextVolume(&mounted) == B_OK;
       ++i) {
    if (auto volume = IdentifyVolume(mounted.Device(), true))
      volumes.emplace_back(mounted.Device(), std::move(*volume));
  }
  size_t budget = kDirectoryScanBudget;
  for (auto &entry : entries) {
    if (!entry.restored || !entry.proof)
      continue;
    entry.unresolved = true;
    entry.restoredState = ShelfState::Missing;
    const auto &proof = *entry.proof;
    const std::pair<dev_t, VolumeProof> *matched = nullptr;
    for (const auto &volume : volumes) {
      if (!MatchesVolume(proof, volume.second))
        continue;
      if (matched) {
        entry.restoredState = ShelfState::Ambiguous;
        matched = nullptr;
        break;
      }
      matched = &volume;
    }
    if (!matched)
      continue;
    const auto &[device, volume] = *matched;
    // Do not open a path merely because it was recorded in settings.
    // Inspect entries anchored in the verified BFS parent or its name index.
    ResolveInParent(entry, device, budget, volume.trash);
    if (entry.unresolved)
      ResolveByName(entry, device, budget, volume.trash);
  }

  // Resolve the whole set before reserving paths. A rename batch may have
  // exchanged two names: both references are safe if both identities match.
  std::map<std::string, std::vector<size_t>> movingTo;
  std::vector<std::string> occupied;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].path == original[i].path)
      occupied.push_back(entries[i].path);
    else
      movingTo[entries[i].path].push_back(i);
  }
  for (const auto &[path, claims] : movingTo)
    if (claims.size() > 1)
      occupied.push_back(path);
  // An unresolved reference keeps its old path. Do not take that path, or
  // any old path retained as a consequence of rejecting another move.
  while (!occupied.empty()) {
    auto claim = movingTo.find(occupied.back());
    occupied.pop_back();
    if (claim == movingTo.end())
      continue;
    for (auto i : claim->second) {
      entries[i] = original[i];
      entries[i].unresolved = true;
      entries[i].restoredState = ShelfState::Ambiguous;
      occupied.push_back(entries[i].path);
    }
    movingTo.erase(claim);
  }
}
} // namespace Listi
