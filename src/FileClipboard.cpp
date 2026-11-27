// SPDX-License-Identifier: MIT
#include "TransferCommand.h"
#include "FileClipboard.h"
#include "Common.h"
#include "ClipboardLock.h"
#include "ArchiveJobs.h"
#include "ArchiveVolumes.h"
#include "JobSnapshot.h"
#include "VirtualFile.h"
#include <FindDirectory.h>
#include <Messenger.h>
#include <OS.h>
#include <Path.h>
#include <Volume.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits.h>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace Listi {
namespace {
constexpr int32 kCopyMode = 'Tcsl', kCutMode = 'Tmsl';
constexpr const char *kLease = "listi:paste-lease";
constexpr ssize_t kMaximumSelectionBytes = 2 * 1024 * 1024;
constexpr size_t kMaximumTextBytes = 4 * 1024 * 1024;
void Require(status_t status, const char *operation) {
  if (status != B_OK)
    throw std::runtime_error(std::string(operation) + ": " + strerror(status));
}
std::string Key(char prefix, const NodeIdentity &node) {
  return std::string(1, prefix) + std::to_string(node.device) + "_" +
         std::to_string(node.inode);
}
NodeIdentity Identity(const std::string &path, bool directory = false) {
  struct stat st;
  if ((directory ? stat(path.c_str(), &st) : lstat(path.c_str(), &st)) != 0)
    throw std::runtime_error(path + ": entry is unavailable.");
  if (directory && !S_ISDIR(st.st_mode))
    throw std::runtime_error("Open a local destination folder first.");
  return {int32_t(st.st_dev), int64_t(st.st_ino)};
}
bool Same(const NodeIdentity &a, const NodeIdentity &b) {
  return a.device == b.device && a.inode == b.inode;
}
void Allowed(const std::string &path, const NodeIdentity &node) {
  if (path.empty() || path.front() != '/' || path == "/")
    throw std::runtime_error(
        "The file clipboard requires ordinary local entries.");
  struct stat st;
  if (lstat(path.c_str(), &st) ||
      !(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)))
    throw std::runtime_error(path + ": unsupported clipboard entry.");
  if (S_ISDIR(st.st_mode)) {
    BEntry entry(path.c_str(), false), parent;
    BPath parentPath;
    Require(entry.GetParent(&parent), "Read parent folder");
    Require(parent.GetPath(&parentPath), "Read parent path");
    auto parentNode = Identity(parentPath.Path(), true);
    if (node.device != parentNode.device || Same(node, parentNode))
      throw std::runtime_error(
          "Volume roots cannot be put on the file clipboard.");
    for (auto which : {B_DESKTOP_DIRECTORY, B_TRASH_DIRECTORY}) {
      BPath protectedPath;
      BVolume volume(node.device);
      if (find_directory(which, &protectedPath, false, &volume) == B_OK) {
        struct stat protectedStat;
        if (!stat(protectedPath.Path(), &protectedStat) &&
            node.device == protectedStat.st_dev &&
            node.inode == protectedStat.st_ino)
          throw std::runtime_error(
              "Desktop and Trash roots cannot be put on the file clipboard.");
      }
    }
  }
}
void Notify(const std::vector<ClipboardItem> &items, bool clear, bool removed) {
  // Native Tracker notification ABI. Send one node per notification: Tracker's
  // pose loop stops at its first node absent from a particular folder window.
  bool first = true;
  for (const auto &item : items) {
    BMessage notice('TCch');
    notice.AddInt32("device", item.ref.device);
    notice.AddInt64("directory", item.ref.directory);
    notice.AddBool("clearClipboard", clear && first);
    struct WireNode {
      node_ref node;
      uint32 mode;
    } wire{};
    wire.node.device = item.node.device;
    wire.node.node = item.node.inode;
    wire.mode = removed ? 'Tdel' : item.cut ? kCutMode : kCopyMode;
    notice.AddData("tcnode", 'TCNR', &wire, sizeof(wire));
    BMessenger("application/x-vnd.Be-TRAK")
        .SendMessage(&notice, (BHandler *)nullptr, 250000);
    first = false;
  }
}
void Commit(BClipboard *clipboard, const BMessage &message) {
  *clipboard->Data() = message;
  Require(clipboard->Commit(true), "Save clipboard");
}
} // namespace

bool HasClipboardFiles(const BMessage &message) {
  type_code type;
  return message.CountNames(B_REF_TYPE) ||
         message.GetInfo("listi:archive-copy", &type) == B_OK;
}

std::string ReadClipboardText(BClipboard *clipboard) {
  ClipboardLock lock(clipboard);
  const BMessage &message = *clipboard->Data();
  if (HasClipboardFiles(message))
    throw std::runtime_error("The clipboard contains files. Use Paste to copy or move them.");
  type_code type;
  int32 count;
  const void *bytes;
  ssize_t size;
  if (message.GetInfo("text/plain", &type, &count) != B_OK ||
      type != B_MIME_TYPE || count != 1 ||
      message.FindData("text/plain", B_MIME_TYPE, &bytes, &size) != B_OK ||
      size <= 0)
    throw std::runtime_error("Copy some plain text before saving from the clipboard.");
  if (size_t(size) > kMaximumTextBytes)
    throw std::runtime_error("Clipboard text exceeds the 4 MiB limit.");
  return std::string(static_cast<const char *>(bytes), size);
}

void PutPathsAsText(const std::vector<std::string> &paths,
                    BClipboard *clipboard) {
  if (paths.empty())
    throw std::runtime_error("Select at least one entry first.");
  std::string text;
  for (const auto &path : paths) {
    if (text.size() + path.size() + !text.empty() > kMaximumTextBytes)
      throw std::runtime_error("The selected paths exceed the 4 MiB text limit.");
    if (!text.empty())
      text += '\n';
    text += path;
  }
  BMessage message;
  Require(message.AddData("text/plain", B_MIME_TYPE, text.data(), text.size()),
          "Add clipboard text");
  ClipboardLock lock(clipboard);
  Commit(clipboard, message);
}

void CopySelectionText(uint32 action, const std::vector<std::string> &paths,
                       const std::string &root, BClipboard *clipboard) {
  if (action == kCopyPaths)
    return PutPathsAsText(paths, clipboard);
  const std::string prefix = root.empty() || root.back() == '/' ? root : root + "/";
  std::vector<std::string> lines;
  for (const auto &path : paths) {
    if (action == kCopyNames)
      lines.push_back(path == "/" ? path : path.substr(path.rfind('/') + 1));
    else if (action == kCopyRelative && !prefix.empty() &&
             path.compare(0, prefix.size(), prefix) == 0 && path.size() > prefix.size())
      lines.push_back(path.substr(prefix.size()));
    else
      throw std::runtime_error("Select entries below the displayed folder.");
  }
  PutPathsAsText(lines, clipboard);
}

std::vector<ClipboardItem> ReadFileClipboard(const BMessage &message) {
  if (message.FlattenedSize() > kMaximumSelectionBytes)
    throw std::runtime_error(
        "Clipboard data is too large. Select fewer files.");
  std::vector<ClipboardItem> items;
  std::set<std::string> nodes;
  char *name;
  type_code type;
  int32 count;
  for (int32 i = 0;
       message.GetInfo(B_REF_TYPE, i, &name, &type, &count) == B_OK; ++i) {
    if (items.size() >= kFileSelectionLimit || count != 1 || name[0] != 'r')
      throw std::runtime_error(
          "Unsupported file clipboard. Copy the files again.");
    ClipboardItem item;
    item.key = name;
    Require(message.FindRef(name, &item.ref), "Read file reference");
    BEntry entry(&item.ref, false);
    BPath path;
    Require(entry.GetPath(&path), "Locate clipboard file");
    item.path = path.Path();
    item.node = Identity(item.path);
    // A ref is a name in a parent directory. It must still name the original
    // device/inode encoded by Tracker, not a replacement at that pathname.
    if (item.key != Key('r', item.node) || !nodes.insert(item.key).second)
      throw std::runtime_error(
          "A clipboard file was replaced. Copy or cut it again.");
    int32 mode;
    Require(message.FindInt32(Key('m', item.node).c_str(), &mode),
            "Read file clipboard mode");
    if (mode != kCopyMode && mode != kCutMode)
      throw std::runtime_error("Unsupported file clipboard action.");
    item.cut = mode == kCutMode;
    Allowed(item.path, item.node);
    items.push_back(item);
  }
  if (items.empty())
    throw std::runtime_error("The clipboard contains no local files.");
  for (size_t i = 0; i < items.size(); ++i)
    for (size_t j = i + 1; j < items.size(); ++j)
      if (items[i].path.compare(0, items[j].path.size() + 1,
                                items[j].path + "/") == 0 ||
          items[j].path.compare(0, items[i].path.size() + 1,
                                items[i].path + "/") == 0)
        throw std::runtime_error(
            "Select either a folder or its contents, not both.");
  return items;
}

BMessage MakeFileSelection(const std::vector<std::string> &paths, bool cut,
                           const std::map<std::string, NodeIdentity> &expected) {
  if (paths.empty() || paths.size() > kFileSelectionLimit)
    throw std::runtime_error("Select between 1 and 1024 local entries first.");
  BMessage message;
  for (const auto &path : paths) {
    auto node = Identity(path);
    if (!expected.empty()) {
      const auto row = expected.find(path);
      if (row == expected.end() || !Same(node, row->second))
        throw std::runtime_error(path + ": selection changed. Select the entry again.");
    }
    Allowed(path, node);
    BEntry entry(path.c_str(), false);
    entry_ref ref;
    Require(entry.GetRef(&ref), "Read file reference");
    Require(
        message.AddInt32(Key('m', node).c_str(), cut ? kCutMode : kCopyMode),
        "Add clipboard mode");
    Require(message.AddRef(Key('r', node).c_str(), &ref), "Add clipboard file");
  }
  ReadFileClipboard(message);
  return message;
}

void PutFileClipboard(const std::vector<std::string> &paths, bool cut,
                      BClipboard *clipboard,
                      const std::map<std::string, NodeIdentity> &expected) {
  if (std::any_of(paths.begin(), paths.end(), [](const auto &path) {
        return ParseLocation(path).Archive();
      })) {
    if (paths.empty() || paths.size() > kFileSelectionLimit)
      throw std::runtime_error("Select between 1 and 1024 archive entries.");
    for (const auto &path : paths) {
      const auto location = ParseLocation(path);
      if (!location.Archive() || location.path == "/")
        throw std::runtime_error(
            "Copy archive entries separately from local files.");
    }
    Job selection{cut ? Action::Move : Action::Copy, paths, ""};
    FreezeArchiveTargets(selection);
    if (cut) {
      const auto archive = ParseLocation(paths.front()).archive;
      PrepareArchiveEditTarget(archive);
      for (const auto &path : paths)
        if (ParseLocation(path).archive != archive)
          throw std::runtime_error("Cut entries from one archive at a time.");
    }
    BMessage message, encoded = EncodeJob(selection, 0);
    if (encoded.FlattenedSize() > kMaximumSelectionBytes)
      throw std::runtime_error("Clipboard selection is too large.");
    Require(message.AddMessage("listi:archive-copy", &encoded),
            "Add archive selection");
    ClipboardLock lock(clipboard);
    Commit(clipboard, message);
    return;
  }
  auto message = MakeFileSelection(paths, cut, expected);
  auto items = ReadFileClipboard(message);
  {
    ClipboardLock lock(clipboard);
    Commit(clipboard, message);
  }
  if (clipboard == be_clipboard)
    Notify(items, true, false);
}

ClipboardPaste PrepareFilePaste(const std::string &destination,
                                BClipboard *clipboard) {
  {
    ClipboardLock lock(clipboard);
    BMessage encoded;
    if (clipboard->Data()->FindMessage("listi:archive-copy", &encoded) ==
        B_OK) {
      if (encoded.FlattenedSize() > kMaximumSelectionBytes)
        throw std::runtime_error("Clipboard selection is too large.");
      if (clipboard->Data()->HasString(kLease))
        throw std::runtime_error(
            "An archive cut transfer is pending. Let it finish or cut the remaining entries again.");
      auto job = DecodeJob(encoded);
      const bool cut = job.action == Action::Move;
      if (!cut && job.action != Action::Copy)
        throw std::runtime_error("Invalid archive clipboard action.");
      job.destination = destination;
      if (job.sources.empty() || job.sources.size() > 1024)
        throw std::runtime_error("Invalid archive clipboard selection.");
      for (const auto &path : job.sources) {
        const auto location = ParseLocation(path);
        if (!location.Archive() || location.path == "/" ||
            !job.expectedArchiveSets.count(location.archive))
          throw std::runtime_error("Invalid archive clipboard source.");
      }
      for (const auto &entry : job.expectedArchiveSets)
        if (FindArchiveVolumes(entry.first).Fingerprint() != entry.second)
          throw std::runtime_error(
              "An archive changed since Copy. Select the entries again.");
      ClipboardPaste paste;
      if (!cut && !ParseLocation(destination).Archive() &&
          (destination.empty() || destination[0] != '/'))
        throw std::runtime_error(
            "Paste into a local folder or a writable archive.");
      paste.jobs.push_back(PrepareTransfer(std::move(job)));
      if (cut) {
        static std::atomic<int64> serial{0};
        paste.archiveCut = true;
        paste.lease = std::to_string(getpid()) +
                      ":archive:" + std::to_string(system_time()) + ":" +
                      std::to_string(++serial);
        BMessage message(*clipboard->Data());
        Require(message.AddString(kLease, paste.lease.c_str()),
                "Reserve archive cut clipboard");
        Commit(clipboard, message);
      }
      return paste;
    }
  }
  if (ParseLocation(destination).Archive()) {
    ClipboardLock lock(clipboard);
    if (clipboard->Data()->HasString(kLease))
      throw std::runtime_error(
          "A cut transfer is pending. Let it finish before pasting again.");
    ClipboardPaste paste;
    paste.items = ReadFileClipboard(*clipboard->Data());
    Job transfer;
    transfer.destination = destination;
    bool cut = false, copy = false;
    for (const auto &item : paste.items) {
      cut |= item.cut;
      copy |= !item.cut;
      transfer.sources.push_back(item.path);
      transfer.expectedSources.emplace(item.path, item.node);
    }
    if (cut && copy)
      throw std::runtime_error(
          "Cut or copy one selection at a time into an archive.");
    transfer.action = cut ? Action::Move : Action::Copy;
    paste.jobs.push_back(PrepareTransfer(std::move(transfer)));
    if (cut) {
      static std::atomic<int64> serial{0};
      paste.lease = std::to_string(getpid()) +
                    ":local-archive:" + std::to_string(system_time()) + ":" +
                    std::to_string(++serial);
      BMessage message(*clipboard->Data());
      Require(message.AddString(kLease, paste.lease.c_str()),
              "Reserve local cut clipboard");
      Commit(clipboard, message);
    }
    return paste;
  }
  if (destination.empty() || destination[0] != '/')
    throw std::runtime_error(
        "Paste requires a local folder or an open writable archive.");
  char canonical[PATH_MAX];
  if (!realpath(destination.c_str(), canonical))
    throw std::runtime_error("The destination folder is unavailable.");
  auto destinationNode = Identity(canonical, true);
  ClipboardLock lock(clipboard);
  ClipboardPaste paste;
  if (clipboard->Data()->HasString(kLease))
    throw std::runtime_error(
        "A cut transfer is pending or was interrupted. Let it finish, or copy/cut the remaining files again.");
  paste.items = ReadFileClipboard(*clipboard->Data());
  Job copy, move;
  copy.action = Action::Copy;
  move.action = Action::Move;
  copy.destination = move.destination = canonical;
  copy.expectedDestination = move.expectedDestination = destinationNode;
  // Check the entire Tracker selection before splitting mixed copy/cut modes.
  // Two source names must never compete for one output in separate jobs.
  std::set<std::string> names;
  for (auto &item : paste.items) {
    if (!names.insert(Leaf(item.path)).second)
      throw std::runtime_error(
          "Two clipboard entries have the same name. Paste them separately.");
    Job &job = item.cut ? move : copy;
    job.sources.push_back(item.path);
    job.expectedSources.emplace(item.path, item.node);
  }
  if (!copy.sources.empty() && !move.sources.empty()) {
    // The queue checks both halves together, outside the clipboard lock.
    copy.siblingPasteSources = move.expectedSources;
    move.siblingPasteSources = copy.expectedSources;
  }
  if (!copy.sources.empty())
    paste.jobs.push_back(PrepareTransfer(std::move(copy)));
  if (!move.sources.empty()) {
    paste.jobs.push_back(PrepareTransfer(std::move(move)));
    static std::atomic<int64> serial{0};
    paste.lease = std::to_string(getpid()) + ":" +
                  std::to_string(system_time()) + ":" +
                  std::to_string(++serial);
    BMessage message(*clipboard->Data());
    message.RemoveName(kLease);
    Require(message.AddString(kLease, paste.lease.c_str()),
            "Reserve cut clipboard");
    Commit(clipboard, message);
  }
  return paste;
}

void FinishFilePaste(const ClipboardPaste &paste,
                     const std::vector<std::string> &moved,
                     BClipboard *clipboard) {
  if (paste.lease.empty())
    return;
  std::vector<ClipboardItem> removed;
  {
    ClipboardLock lock(clipboard);
    BMessage message(*clipboard->Data());
    if (paste.lease != message.GetString(kLease, ""))
      return;
    if (paste.archiveCut) {
      // A move is all-or-nothing for one source archive. Failed/cancelled jobs
      // release the lease without silently refreshing the original snapshot.
      bool complete = !paste.jobs.empty();
      for (const auto &path : paste.jobs.front().sources)
        complete = complete &&
                   std::find(moved.begin(), moved.end(), path) != moved.end();
      if (complete)
        message.RemoveName("listi:archive-copy");
      message.RemoveName(kLease);
      Commit(clipboard, message);
      return;
    }
    for (const auto &item : paste.items) {
      if (!item.cut ||
          std::find(moved.begin(), moved.end(), item.path) == moved.end())
        continue;
      // Tracker may already have updated the reference after a rename, or
      // removed it after a cross-volume move. Never touch another node/mode.
      int32 mode;
      if (message.FindInt32(Key('m', item.node).c_str(), &mode) == B_OK &&
          mode == kCutMode) {
        message.RemoveName(item.key.c_str());
        message.RemoveName(Key('m', item.node).c_str());
        removed.push_back(item);
      }
    }
    message.RemoveName(kLease);
    Commit(clipboard, message);
  }
  if (clipboard == be_clipboard)
    Notify(removed, false, true);
}
} // namespace Listi
