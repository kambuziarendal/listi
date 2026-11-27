// SPDX-License-Identifier: MIT
#include "TransferCommand.h"
#include "FileDrop.h"
#include "JobSnapshot.h"
#include "Pane.h"
#include <Window.h>
#include "FileClipboard.h"
#include "VirtualFile.h"
#include <algorithm>
#include <Path.h>
#include <Directory.h>
#include <cerrno>
#include <cstring>
#include <limits.h>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <cstdlib>
namespace Listi {
namespace {
constexpr const char *kDestinationVersion = "listi:destination-archive-version";
constexpr const char *kArchiveSelection = "listi:archive-selection";
constexpr const char *kRemoteSelection = "listi:remote-selection";
constexpr const char *kSelection = "listi:drag-selection";
void Require(status_t s) {
  if (s != B_OK)
    throw std::runtime_error(
        "The dragged file list is incomplete. Select the files again.");
}
std::vector<ClipboardItem> ReadDragRefs(const BMessage &message) {
  type_code type;
  int32 count;
  if (message.GetInfo("refs", &type, &count) != B_OK || type != B_REF_TYPE ||
      count < 1 || count > 1024)
    throw std::runtime_error("Drop between 1 and 1024 local entries.");
  std::vector<std::string> paths;
  for (int32 i = 0; i < count; ++i) {
    entry_ref ref;
    Require(message.FindRef("refs", i, &ref));
    BEntry entry(&ref, false);
    BPath path;
    Require(entry.GetPath(&path));
    paths.emplace_back(path.Path());
  }
  BMessage selection;
  if (message.HasMessage(kSelection))
    Require(message.FindMessage(kSelection, &selection));
  else
    selection = MakeFileSelection(paths);
  auto items = ReadFileClipboard(selection);
  std::set<std::string> remaining(paths.begin(), paths.end());
  if (remaining.size() != paths.size() || items.size() != paths.size())
    throw std::runtime_error(
        "The dragged file selection does not match its references.");
  for (const auto &item : items) {
    if (!remaining.erase(item.path))
      throw std::runtime_error(
          "The dragged file selection changed. Select it again.");
  }
  return items;
}
}
BMessage MakeFileDrag(const std::vector<std::string> &paths,
                      const std::map<std::string, NodeIdentity> &expected) {
  if (std::any_of(paths.begin(), paths.end(),
                  [](const auto &path) { return IsVirtual(path); })) {
    if (paths.empty() || paths.size() > kFileSelectionLimit)
      throw std::runtime_error("Select between 1 and 1024 entries.");
    BMessage drag(B_SIMPLE_DATA);
    for (const auto &path : paths) {
      if (!IsVirtual(path))
        throw std::runtime_error(
            "Drag local and virtual selections separately.");
      Require(drag.AddString("listi:path", path.c_str()));
    }
    return drag;
  }
  auto selection = MakeFileSelection(paths, false, expected);
  BMessage drag(B_SIMPLE_DATA);
  for (const auto &item : ReadFileClipboard(selection))
    Require(drag.AddRef("refs", &item.ref));
  Require(drag.AddMessage(kSelection, &selection));
  return drag;
}
BMessage MakePaneFileDrag(const PaneModel &model) {
  const auto paths = model.SelectedFiles();
  auto drag = MakeFileDrag(paths, model.SelectedLocalIdentities(kFileSelectionLimit));
  if (!paths.empty() && IsVirtual(paths.front())) {
    Job snapshot;
    model.BindArchiveView(snapshot, paths.front());
    const bool archive = !snapshot.expectedArchiveSets.empty();
    if (!archive)
      snapshot.expectedRemoteSources = model.SelectedRemoteEntries();
    const auto encoded = EncodeJob(snapshot, 0);
    Require(drag.AddMessage(archive ? kArchiveSelection : kRemoteSelection, &encoded));
  }
  return drag;
}
void PostPaneFileDrop(Pane &pane, const PaneModel &receiver, const BMessage &drag,
                      const std::string &destination) {
  try {
    Job view;
    receiver.BindArchiveView(view, destination);
    BMessage drop(drag);
    drop.what = kDrop;
    // Destination fields belong to the receiver, never the drag source.
    drop.RemoveName("destination");
    drop.RemoveName(kDestinationVersion);
    Require(drop.AddString("destination", destination.c_str()));
    if (!view.expectedArchiveSets.empty())
      Require(drop.AddString(kDestinationVersion, receiver.archiveVersion.c_str()));
    Require(pane.Window()->PostMessage(&drop));
  } catch (const std::exception &error) {
    pane.ShowStatus(error.what());
  }
}
Job PrepareFileDrop(const BMessage &message, const std::string &destination,
                    bool allowSameFolder, std::string *missingLeaf) {
  if (missingLeaf)
    missingLeaf->clear();
  if (message.FlattenedSize() > 2 * 1024 * 1024)
    throw std::runtime_error("The dragged selection is too large.");
  Job job;
  job.destination = destination;
  if (message.HasString("listi:path")) {
    if (message.HasRef("refs"))
      throw std::runtime_error("Drag local and virtual selections separately.");
    type_code type;
    int32 count;
    Require(message.GetInfo("listi:path", &type, &count));
    if (type != B_STRING_TYPE || count < 1 || count > 1024)
      throw std::runtime_error("Invalid virtual drop selection.");
    for (int32 i = 0; i < count; ++i) {
      const char *path;
      Require(message.FindString("listi:path", i, &path));
      if (!IsVirtual(path))
        throw std::runtime_error("Invalid virtual drop selection.");
      job.sources.emplace_back(path);
    }
    if (std::any_of(job.sources.begin(), job.sources.end(),
                    [](const auto &s) { return ParseLocation(s).Archive(); })) {
      BMessage snapshot;
      if (message.FindMessage(kArchiveSelection, &snapshot) != B_OK)
        throw std::runtime_error("Missing archive version. Reload and drag the entries again.");
      job.expectedArchiveSets = DecodeJob(snapshot).expectedArchiveSets;
      for (const auto &source : job.sources)
        if (!job.expectedArchiveSets.count(ParseLocation(source).archive))
          throw std::runtime_error("Incomplete archive selection. Reload and select again.");
    }
    // Older drags carry URLs only. A drag with a snapshot must cover the
    // entire selection; never silently downgrade it to the old behaviour.
    if (message.HasMessage(kRemoteSelection)) {
      BMessage snapshot;
      Require(message.FindMessage(kRemoteSelection, &snapshot));
      job.expectedRemoteSources = DecodeJob(snapshot).expectedRemoteSources;
      if (job.expectedRemoteSources.size() != job.sources.size() ||
          std::any_of(job.sources.begin(), job.sources.end(), [&](const std::string &source) {
            return !job.expectedRemoteSources.count(source);
          }))
        throw std::runtime_error("The dragged network selection changed. Select the entries again.");
    }
  } else {
    for (const auto &item : ReadDragRefs(message)) {
      job.sources.push_back(item.path);
      job.expectedSources.emplace(item.path, item.node);
    }
  }
  const auto target = ParseLocation(destination);
  if (target.Archive() && message.what == kDrop) {
    const auto shown = message.GetString(kDestinationVersion, "");
    if (!shown[0] ||
        job.expectedArchiveSets.emplace(target.archive, shown).first->second != shown)
      throw std::runtime_error("Archive destination changed or is incomplete. Reload and select again.");
  }
  if (!destination.empty() && destination[0] == '/') {
    char canonical[PATH_MAX];
    const auto end = destination.find_last_not_of('/');
    const auto path = end == std::string::npos ? "/" : destination.substr(0, end + 1);
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
      const auto error = errno;
      const auto slash = path.rfind('/');
      const auto leaf = path.substr(slash + 1);
      if (!missingLeaf || error != ENOENT || slash == std::string::npos ||
          leaf == "." || leaf == ".." || message.HasString("listi:path") ||
          !realpath(path.substr(0, slash + 1).c_str(), canonical))
        throw std::runtime_error("Drop destination unavailable: " + destination);
      job.destination = canonical;
      job = PrepareTransfer(std::move(job), true);
      *missingLeaf = leaf;
      return job;
    }
    if (!realpath(destination.c_str(), canonical))
      throw std::runtime_error("Drop destination unavailable: " + destination);
    job.destination = canonical;
  }
  return PrepareTransfer(std::move(job), allowSameFolder);
}
Job CreateFileDropDestination(Job parentJob, const std::string &leaf) {
  if (leaf.empty() || leaf == "." || leaf == ".." || leaf.find('/') != std::string::npos)
    throw std::runtime_error("Invalid destination folder name.");
  parentJob = PrepareTransfer(std::move(parentJob), true);
  const auto path = parentJob.destination +
                    (parentJob.destination == "/" ? "" : "/") + leaf;
  BDirectory parent(parentJob.destination.c_str()), created;
  node_ref id;
  if (parent.GetNodeRef(&id) != B_OK ||
      id.device != parentJob.expectedDestination.device ||
      id.node != parentJob.expectedDestination.inode)
    throw std::runtime_error("Destination parent changed: " + path);
  const auto status = parent.CreateDirectory(leaf.c_str(), &created);
  if (status != B_OK)
    throw std::runtime_error("Cannot create " + path + ": " + strerror(status));
  if (created.GetNodeRef(&id) != B_OK)
    throw std::runtime_error("Cannot verify created folder: " + path);
  parentJob.destination = path;
  parentJob.expectedDestination = {int32_t(id.device), int64_t(id.node)};
  return PrepareTransfer(std::move(parentJob));
}
bool CanMoveFileDrop(const Job &job) {
  const auto volume = job.expectedDestination.device;
  return !job.sources.empty() && volume >= 0 && !IsVirtual(job.destination) &&
         std::all_of(job.sources.begin(), job.sources.end(),
                     [&](const auto &path) {
                       const auto source = job.expectedSources.find(path);
                       return source != job.expectedSources.end() &&
                              source->second.device == volume;
                     });
}
}
