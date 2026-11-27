// SPDX-License-Identifier: MIT
#include "Trash.h"
#include "VirtualFile.h"
#include "FileOperation.h"
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Node.h>
#include <Path.h>
#include <String.h>
#include <Volume.h>
#include <fs_info.h>
#include <cerrno>
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
  // Ask for the native location without creating it. A creation failure
  // (even ENOENT) does not establish that Trash is unsupported.
  char trash[B_PATH_NAME_LENGTH];
  const status_t lookup = find_directory(B_TRASH_DIRECTORY, volume.Device(),
                                         false, trash, sizeof(trash));
  if (lookup == B_ENTRY_NOT_FOUND) {
    fs_info info{};
    if (fs_stat_dev(volume.Device(), &info) != 0)
      Require(errno, path);
    // Haiku mounts ext4 using its ext2 driver.
    const bool ext = strcmp(info.fsh_name, "ext2") == 0 ||
                     strcmp(info.fsh_name, "ext3") == 0 ||
                     strcmp(info.fsh_name, "ext4") == 0;
    if (ext &&
        !(info.flags & B_FS_IS_READONLY)) {
      BDirectory root;
      Require(volume.GetRootDirectory(&root), path);
      throw TrashUnsupported(path + ": Haiku has no Trash on this ext4 volume. "
                             "Nothing was deleted. Use Delete permanently only "
                             "if you do not need to restore the item.");
    }
  }
  Require(lookup, path + " (locating Trash)");
  if (create)
    Require(find_directory(B_TRASH_DIRECTORY, volume.Device(), true, trash,
                           sizeof(trash)), path + " (opening Trash)");
  return trash;
}
namespace {
std::string CheckedTrash(const std::string &original, bool create) {
  std::string trash = TrashDirectory(original, create);
  if (Within(original, trash) || Within(trash, original))
    throw std::runtime_error(original + ": cannot trash the Trash folder, its "
                                        "ancestors or an already trashed entry.");
  return trash;
}
void CheckIdentity(BEntry &entry, const NodeIdentity &expected) {
  node_ref actual;
  Require(entry.GetNodeRef(&actual), "Check source identity");
  if (actual.device != expected.device || actual.node != expected.inode)
    throw std::runtime_error("Selected entry changed; operation stopped.");
}
}
void CheckTrashSource(const std::string &path, const NodeIdentity *expected,
                      bool create) {
  BEntry entry(path.c_str(), false);
  Require(entry.InitCheck(), path);
  if (expected)
    CheckIdentity(entry, *expected);
  struct stat st;
  Require(entry.GetStat(&st), path);
  if (S_ISDIR(st.st_mode)) {
    BDirectory directory(&entry);
    Require(directory.InitCheck(), path);
    if (directory.IsRootDirectory())
      throw std::runtime_error(path + ": mounted volumes cannot be moved to Trash.");
  }
  CheckedTrash(EntryPath(entry), create);
}
std::string MoveToTrash(const std::string &path, const NodeIdentity *expected) {
  BEntry entry(path.c_str(), false);
  Require(entry.InitCheck(), path);
  if (expected)
    CheckIdentity(entry, *expected);
  const std::string original = EntryPath(entry);
  const std::string trash = CheckedTrash(original, true);
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
  // MoveTo(false) checks for a conflict before renaming. Haiku does not
  // provide an atomic no-replace or identity-conditional rename.
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
namespace {
std::string ReadOriginalPath(BNode &node, const std::string &path, const std::string &trash) {
  BString original;
  Require(node.ReadAttrString(kOriginalPath, &original), path);
  std::string target = original.String();
  if (target.empty() || target[0] != '/' || Leaf(target).empty() ||
      Leaf(target) == "." || Leaf(target) == ".." || Within(target, trash))
    throw std::runtime_error(
        path + ": invalid original path; entry retained in Trash.");
  return target;
}
}
std::string OriginalTrashPath(const std::string &path, const NodeIdentity &expected) {
  BEntry entry(path.c_str(), false);
  CheckIdentity(entry, expected);
  BNode node(&entry);
  BEntry parent;
  Require(entry.GetParent(&parent), path);
  auto trash = TrashDirectory(path, false);
  if (EntryPath(parent) != trash)
    throw std::runtime_error("Not a top-level Trash entry");
  return ReadOriginalPath(node, path, trash);
}
RestoreTarget CaptureRestoreTarget(const std::string &path, const NodeIdentity &expected) {
  RestoreTarget target;
  target.path = OriginalTrashPath(path, expected);
  BDirectory parent(ParentLocation(target.path).c_str());
  Require(parent.InitCheck(), target.path + " (original folder must exist)");
  node_ref identity;
  Require(parent.GetNodeRef(&identity), target.path);
  target.parent = {identity.device, identity.node};
  return target;
}
std::string RestoreFromTrash(const std::string &path, std::string *warning,
                             const NodeIdentity *expected,
                             const RestoreTarget *confirmedTarget) {
  BEntry entry(path.c_str(), false);
  if (expected)
    CheckIdentity(entry, *expected);
  std::string current = EntryPath(entry);
  std::string trash = TrashDirectory(current, false);
  BEntry parent;
  Require(entry.GetParent(&parent), current);
  if (EntryPath(parent) != trash)
    throw std::runtime_error(path +
                             ": select a top-level entry in Trash to restore.");
  BNode node(&entry);
  node_ref identity;
  Require(node.GetNodeRef(&identity), current);
  const NodeIdentity opened{identity.device, identity.node};
  if (expected && (opened.device != expected->device ||
                   opened.inode != expected->inode))
    throw std::runtime_error(current + ": selected entry changed; not restored.");
  auto target = ReadOriginalPath(node, current, trash);
  if (confirmedTarget && target != confirmedTarget->path)
    throw std::runtime_error(path + ": restore destination changed; select Restore again.");
  const auto parentPath = ParentLocation(target);
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
  node_ref destinationIdentity;
  Require(destination.GetNodeRef(&destinationIdentity), parentPath);
  if (confirmedTarget && (destinationIdentity.device != confirmedTarget->parent.device ||
                          destinationIdentity.node != confirmedTarget->parent.inode))
    throw std::runtime_error(path + ": original folder was replaced; retained in Trash. Select Restore again.");
  if (destinationIdentity.device != identity.device)
    throw std::runtime_error(path + ": original folder is now on another volume.");
  // No intentional replacement or recursive merge. The same check/rename
  // race described for Trash still applies.
  CheckIdentity(entry, opened);
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
