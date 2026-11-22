// SPDX-License-Identifier: MIT
#include "Trash.h"
#include "FileOperation.h"
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Node.h>
#include <Path.h>
#include <String.h>
#include <Volume.h>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>

namespace Listi {
namespace {
// Tracker's on-disk restoration attribute, not a private API dependency.
const char *kOriginalPath = "_trk/original_path";
void Require(status_t status, const std::string &path) {
  if (status != B_OK)
    throw std::runtime_error(path + ": " + strerror(status));
}
bool Within(const std::string &path, const std::string &parent) {
  return path == parent ||
         path.compare(0, parent.size() + 1, parent + "/") == 0;
}
std::string EntryPath(BEntry &entry) {
  BPath path;
  Require(entry.GetPath(&path), "Resolve path");
  return path.Path();
}
} // namespace
std::string TrashDirectory(const std::string &path, bool create) {
  BEntry entry(path.c_str(), false);
  struct stat st;
  Require(entry.GetStat(&st), path);
  BVolume volume(st.st_dev);
  Require(volume.InitCheck(), path);
  if (!volume.IsPersistent() || volume.IsReadOnly() || !volume.KnowsAttr())
    throw std::runtime_error(
        path +
        ": this volume has no supported writable Trash. Nothing was deleted.");
  BPath trash;
  Require(find_directory(B_TRASH_DIRECTORY, &trash, create, &volume), path);
  return trash.Path();
}
namespace {
std::string CheckedTrash(const std::string &original) {
  std::string trash = TrashDirectory(original, true);
  if (Within(original, trash) || Within(trash, original))
    throw std::runtime_error(original + ": cannot trash the Trash folder, its "
                                        "ancestors or an already trashed entry.");
  return trash;
}
void CheckIdentity(BEntry &entry, const NodeIdentity &expected) {
  node_ref actual;
  Require(entry.GetNodeRef(&actual), "Check source identity");
  if (actual.device != expected.device || actual.node != expected.inode)
    throw std::runtime_error("Selected source changed; not moved to Trash.");
}
}
void CheckTrashSource(const std::string &path) {
  BEntry entry(path.c_str(), false);
  Require(entry.InitCheck(), path);
  CheckedTrash(EntryPath(entry));
}
std::string MoveToTrash(const std::string &path, const NodeIdentity *expected) {
  BEntry entry(path.c_str(), false);
  Require(entry.InitCheck(), path);
  if (expected)
    CheckIdentity(entry, *expected);
  const std::string original = EntryPath(entry);
  const std::string trash = CheckedTrash(original);
  BDirectory directory(trash.c_str());
  Require(directory.InitCheck(), trash);
  BNode node(&entry);
  Require(node.InitCheck(), original);
  node_ref identity, trashIdentity;
  Require(node.GetNodeRef(&identity), original);
  Require(directory.GetNodeRef(&trashIdentity), trash);
  if (trashIdentity.device != identity.device)
    throw std::runtime_error(original + ": Trash is not on the source volume.");
  if (expected && (identity.device != expected->device ||
                   identity.node != expected->inode))
    throw std::runtime_error(original + ": source changed; not moved to Trash.");
  BString previous;
  bool hadPrevious = node.ReadAttrString(kOriginalPath, &previous) == B_OK;
  BString saved(original.c_str());
  Require(node.WriteAttrString(kOriginalPath, &saved), original);
  // MoveTo(false) never overwrites, including a name created after our check.
  auto restoreMetadata = [&] {
    if (hadPrevious)
      node.WriteAttrString(kOriginalPath, &previous);
    else
      node.RemoveAttr(kOriginalPath);
  };
  std::string name;
  status_t status = B_FILE_EXISTS;
  for (unsigned n = 0; n < 10000 && status == B_FILE_EXISTS; n++) {
    name = n == 0 ? Leaf(original)
                              : Leaf(original) + " (" + std::to_string(n) + ")";
    if (name.size() >= B_FILE_NAME_LENGTH)
      name =
          "trashed-" + std::to_string(identity.node) + "-" + std::to_string(n);
    try {
      if (expected)
        CheckIdentity(entry, *expected);
    } catch (...) {
      restoreMetadata();
      throw;
    }
    status = entry.MoveTo(&directory, name.c_str(), false);
  }
  if (status != B_OK) {
    restoreMetadata();
    Require(status, original);
  }
  if (expected)
    CheckIdentity(entry, *expected);
  return Join(trash, name);
}
std::string RestoreFromTrash(const std::string &path, std::string *warning) {
  BEntry entry(path.c_str(), false);
  std::string current = EntryPath(entry);
  std::string trash = TrashDirectory(current, false);
  BEntry parent;
  Require(entry.GetParent(&parent), current);
  if (EntryPath(parent) != trash)
    throw std::runtime_error(path +
                             ": select a top-level entry in Trash to restore.");
  BNode node(&entry);
  BString original;
  Require(node.ReadAttrString(kOriginalPath, &original), current);
  std::string target = original.String();
  if (target.empty() || target[0] != '/' || Leaf(target).empty() ||
      Leaf(target) == "." || Leaf(target) == ".." || Within(target, trash))
    throw std::runtime_error(
        path + ": invalid original path; entry retained in Trash.");
  std::string parentPath = target.substr(0, target.find_last_of('/'));
  if (parentPath.empty())
    parentPath = "/";
  BDirectory destination(parentPath.c_str());
  Require(destination.InitCheck(),
          parentPath + " (original folder must exist)");
  // A former parent may now be a symlink into Trash. Check the opened
  // directory, not just the spelling saved by Tracker.
  BEntry resolvedParent;
  Require(destination.GetEntry(&resolvedParent), parentPath);
  if (Within(EntryPath(resolvedParent), trash))
    throw std::runtime_error(
        path + ": original folder now points into Trash; entry retained.");
  // No replacement or recursive merge during restore. A conflicting file is
  // safe.
  Require(entry.MoveTo(&destination, Leaf(target).c_str(), false), target);
  const auto status = node.RemoveAttr(kOriginalPath);
  if (status != B_OK) {
    const auto message = target + " (restored, but could not clear Trash metadata)";
    if (!warning)
      Require(status, message);
    else
      *warning = message + ": " + strerror(status);
  }
  return target;
}
} // namespace Listi
