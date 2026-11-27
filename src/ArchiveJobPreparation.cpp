// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "ArchiveJobChecks.h"
#include "ArchiveVolumes.h"
#include "ArchiveWriting.h"
#include "JobSnapshot.h"
#include "MoveVolume.h"
#include "VirtualFile.h"
#include <StorageDefs.h>
#include <cerrno>
#include <cstdlib>
#include <set>
#include <sys/stat.h>

namespace Listi {
using namespace ArchiveJobChecks;

Job PrepareArchiveJob(bool extract, const std::vector<std::string> &sources) {
  Require(!sources.empty(), "Select files or folders first.");
  Job job;
  job.action = extract ? Action::ExtractArchive : Action::CreateArchive;
  job.sources = sources;
  Require(!extract || sources.size() == 1 ||
              ParseLocation(sources[0]).Archive(),
          "Select one archive, or entries inside one archive.");
  std::set<std::string> names, capturedArchives;
  for (const auto &source : sources) {
    const auto local = LocalSource(source);
    struct stat s;
    Require(!lstat(local.c_str(), &s), "Source is unavailable.");
    Require(S_ISREG(s.st_mode) || (!extract && S_ISDIR(s.st_mode)),
            "Select regular files or folders, not links.");
    if (!extract)
      Require(!IsVirtual(source),
              "Copy archive entries to a local folder before packing them.");
    Require(names.insert(LocationLeaf(source)).second,
            "Selected entries have duplicate names.");
    job.expectedSources[local] = {int32_t(s.st_dev), int64_t(s.st_ino)};
    if (extract && capturedArchives.insert(local).second) {
      auto set = FindArchiveVolumes(local);
      job.expectedArchiveSets[set.files[0].path] = set.Fingerprint();
      for (const auto &part : set.files) {
        struct stat st;
        Require(!lstat(part.path.c_str(), &st), "Archive part disappeared.");
        job.expectedSources[part.path] = {int32_t(st.st_dev),
                                          int64_t(st.st_ino)};
      }
      Require(FindArchiveVolumes(local).Fingerprint() == set.Fingerprint(),
              "Archive parts changed during selection.");
    }
  }
  return job;
}
Job PrepareArchiveEditTarget(const std::string &archive,
                             const std::string &internalFolder) {
  Require(!archive.empty() && archive[0] == '/' && !IsVirtual(archive),
          "Choose one local writable archive.");
  Job job;
  job.action = Action::EditArchive;
  job.destination = archive;
  job.archiveFormat = DetectArchiveEditFormat(archive, {});
  struct stat archiveStat;
  Require(!lstat(archive.c_str(), &archiveStat) && S_ISREG(archiveStat.st_mode),
          "Archive is unavailable.");
  job.expectedSources[archive] = {int32_t(archiveStat.st_dev),
                                  int64_t(archiveStat.st_ino)};
  auto set = FindArchiveVolumes(archive);
  job.expectedArchiveSets[set.files[0].path] = set.Fingerprint();
  job.destinationFolder = ParentLocation(archive);
  job.entryName = Leaf(archive);
  struct stat parent;
  Require(!stat(job.destinationFolder.c_str(), &parent) &&
              S_ISDIR(parent.st_mode),
          "Archive folder is unavailable.");
  job.expectedDestination = {int32_t(parent.st_dev), int64_t(parent.st_ino)};
  job.archivePath = MemberPath(internalFolder);
  return job;
}
void SetArchiveEditAdditions(Job &job, const std::vector<std::string> &add) {
  Require(job.action == Action::EditArchive && !job.destination.empty(),
          "Archive edit target is missing.");
  Require(!add.empty(), "Choose files or folders to add.");
  VerifyArchiveSets(job);
  Guards(job);
  Job updated = job;
  for (const auto &source : updated.sources)
    updated.expectedSources.erase(source);
  updated.sources.clear();
  std::set<std::string> names, capturedArchives;
  for (const auto &source : add) {
    const auto location = ParseLocation(source);
    Require(
        location.Local() || location.Archive(),
        "Copy network files to a local folder before adding them to an archive.");
    Require(source != job.destination,
            "An archive cannot be added to itself while it is being edited.");
    const auto physical = location.Archive() ? location.archive : source;
    struct stat st;
    Require(!lstat(physical.c_str(), &st) &&
                (S_ISREG(st.st_mode) ||
                 (!location.Archive() && S_ISDIR(st.st_mode))),
            "Only regular files, folders and archive entries can be added.");
    Require(!location.Archive() || !MemberPath(location.path).empty(),
            "Select entries inside an archive, not its root.");
    Require(names.insert(LocationLeaf(source)).second,
            "Selected additions have duplicate names.");
    updated.sources.push_back(source);
    updated.expectedSources[physical] = {int32_t(st.st_dev),
                                         int64_t(st.st_ino)};
    if (location.Archive() && capturedArchives.insert(physical).second) {
      auto set = FindArchiveVolumes(physical);
      updated.expectedArchiveSets[set.files.front().path] = set.Fingerprint();
    }
  }
  job = std::move(updated);
}
Job PrepareArchiveEditJob(const std::string &archive,
                          const std::vector<std::string> &add,
                          const std::vector<std::string> &remove,
                          const std::string &internalFolder) {
  Require(!add.empty() || !remove.empty(),
          "Choose files to add or archive entries to remove.");
  Job job = PrepareArchiveEditTarget(archive, internalFolder);
  if (!add.empty())
    SetArchiveEditAdditions(job, add);
  std::set<std::string> removed;
  for (const auto &source : remove) {
    auto location = ParseLocation(source);
    Require(location.Archive() && location.archive == archive &&
                location.path != "/",
            "Select entries from one archive, not the archive root.");
    removed.insert(MemberPath(location.path));
  }
  for (const auto &path : removed) {
    bool covered = false;
    for (const auto &kept : job.archiveRemove)
      if (path.size() > kept.size() &&
          path.compare(0, kept.size(), kept) == 0 && path[kept.size()] == '/') {
        covered = true;
        break;
      }
    if (!covered)
      job.archiveRemove.push_back(path);
  }
  return job;
}
void FreezeArchiveTargets(Job &transfer) {
  std::set<std::string> inspected;
  auto capture = [&](const std::string &path) {
    const auto location = ParseLocation(path);
    if (!location.Archive() || !inspected.insert(location.archive).second)
      return;
    const auto set = FindArchiveVolumes(location.archive);
    const auto fingerprint = set.Fingerprint();
    const auto shown = transfer.expectedArchiveSets.find(location.archive);
    Require(shown == transfer.expectedArchiveSets.end() || shown->second == fingerprint,
            "Archive changed since selection. Reload and select again.");
    const auto inserted = transfer.expectedArchiveSets.emplace(
        set.files.front().path, fingerprint);
    Require(inserted.first->second == fingerprint,
            "Archive parts changed after selection. Reload and select again.");
  };
  capture(transfer.destination);
  for (const auto &source : transfer.sources)
    capture(source);
}
Job PrepareArchiveMutation(const Job &transfer) {
  VerifyArchiveSets(transfer);
  auto location = ParseLocation(transfer.destination);
  if (transfer.action == Action::Delete || transfer.action == Action::Trash) {
    Require(!transfer.sources.empty(), "Select archive entries first.");
    location = ParseLocation(transfer.sources.front());
    auto edit = PrepareArchiveEditJob(location.archive, {}, transfer.sources);
    RequireArchiveVersions(transfer, edit);
    return edit;
  }
  Require(location.Archive(), "Choose a destination inside an archive.");
  if (transfer.action == Action::Move && !transfer.sources.empty() &&
      ParseLocation(transfer.sources.front()).archive != location.archive)
    return PrepareArchiveMove(transfer);
  auto edit = PrepareArchiveEditTarget(location.archive);
  if (transfer.action == Action::MakeDirectory) {
    edit.archiveFolders.push_back(MemberPath(location.path));
  } else {
    Require(transfer.action == Action::Rename ||
                transfer.action == Action::Move,
            "This action cannot change archive entries.");
    Require(!transfer.sources.empty() && (transfer.action != Action::Rename ||
                                          transfer.sources.size() == 1),
            "Choose one archive entry to rename.");
    for (const auto &path : transfer.sources) {
      const auto from = ParseLocation(path);
      Require(
          from.Archive() && from.archive == location.archive,
          "Move between archives or local folders is not supported yet. Use Copy; originals are retained.");
      const auto to = transfer.action == Action::Rename
                          ? location.path
                          : ParseLocation(ChildLocation(transfer.destination,
                                                        LocationLeaf(path)))
                                .path;
      const auto source = MemberPath(from.path), destination = MemberPath(to);
      Require(
          !source.empty() && !destination.empty() && source != destination &&
              destination.compare(0, source.size() + 1, source + "/") != 0,
          "An archive entry cannot be moved onto itself or into its contents.");
      edit.archiveRenames[source] = destination;
    }
  }
  RequireArchiveVersions(transfer, edit);
  return edit;
}
Job PrepareArchiveCopy(const Job &transfer) {
  VerifyArchiveSets(transfer);
  Require(
      transfer.action == Action::Copy,
      "Moving into archives is not supported. Use Copy; originals will be kept.");
  const auto target = ParseLocation(transfer.destination);
  Require(target.Archive(), "Choose a folder inside a local writable archive.");
  auto edit =
      PrepareArchiveEditJob(target.archive, transfer.sources, {}, target.path);
  for (const auto &item : transfer.expectedSources) {
    const auto found = edit.expectedSources.find(item.first);
    Require(found != edit.expectedSources.end() &&
                found->second.device == item.second.device &&
                found->second.inode == item.second.inode,
            "A selected source was replaced. Select it again.");
  }
  RequireArchiveVersions(transfer, edit);
  return edit;
}
Job PrepareArchiveMove(const Job &transfer) {
  Require(!transfer.sources.empty() && transfer.sources.size() <= 1024,
          "Select between 1 and 1024 entries inside one writable archive.");
  Guards(transfer);
  VerifyArchiveSets(transfer);
  const auto target = ParseLocation(transfer.destination);
  const auto source = ParseLocation(transfer.sources.front());
  if (source.Local() != target.Local() &&
      (source.Archive() || target.Archive())) {
    Require((source.Local() && target.Archive()) ||
                (source.Archive() && target.Local()),
            "Move between a local folder and one writable local archive.");
    const auto archive = source.Archive() ? source.archive : target.archive;
    auto edit =
        PrepareArchiveEditTarget(archive, target.Archive() ? target.path : "");
    Job move = transfer;
    move.action = Action::MoveArchive;
    std::set<std::string> names;
    auto canonical = [](const std::string &path) {
      char *resolved = realpath(path.c_str(), nullptr);
      std::string result = resolved ? resolved : "";
      free(resolved);
      Require(!result.empty(), "Cannot resolve local move path: " + path);
      return result;
    };
    const auto container = canonical(archive);
    const auto localTarget =
        target.Local() ? canonical(transfer.destination) : std::string();
    for (const auto &path : transfer.sources) {
      const auto location = ParseLocation(path);
      Require(
          location.Archive() == source.Archive() &&
              (source.Archive() ? location.archive == source.archive &&
                                      !MemberPath(location.path).empty()
                                : location.Local()),
          "Choose local entries or entries from one archive, not a mixed selection.");
      Require(!LocationLeaf(path).empty() &&
                  names.insert(LocationLeaf(path)).second,
              "Selected entries have duplicate or empty names.");
      auto local = source.Local()
                       ? canonical(path)
                       : ChildLocation(localTarget, LocationLeaf(path));
      Require(local != container &&
                  container.compare(0, local.size() + 1, local + "/") != 0,
              "A move cannot replace or contain its own archive.");
    }
    for (const auto &path : transfer.sources)
      for (const auto &other : transfer.sources)
        Require(path == other ||
                    other.compare(0, path.size() + 1, path + "/") != 0,
                "Select either a folder or its contents, not both.");
    FreezeLocalJob(move, target.Local() ? transfer.destination
                                        : edit.destinationFolder);
    move.expectedSources.insert(edit.expectedSources.begin(),
                                edit.expectedSources.end());
    move.expectedArchiveSets = edit.expectedArchiveSets;
    CheckMoveVolumes(move);
    Guards(transfer);
    VerifyArchiveSets(transfer);
    RequireArchiveVersions(transfer, move);
    return move;
  }
  Require(
      source.Archive() && target.Archive(),
      "Archive Cut/Move requires a local writable archive at one or both ends.");
  auto from = PrepareArchiveEditTarget(source.archive);
  auto to = PrepareArchiveEditTarget(target.archive, target.path);
  const auto a = from.expectedSources.at(source.archive),
             b = to.expectedSources.at(target.archive);
  Require(
      source.archive == target.archive || a.device != b.device ||
          a.inode != b.inode,
      "Source and destination are aliases of the same archive. Open the same archive path in both panels.");
  std::set<std::string> names, members;
  for (const auto &path : transfer.sources) {
    const auto location = ParseLocation(path);
    Require(location.Archive() && location.archive == source.archive &&
                !MemberPath(location.path).empty(),
            "Move entries from one archive at a time, not its root.");
    Require(names.insert(LocationLeaf(path)).second,
            "Selected entries have duplicate names.");
    members.insert(MemberPath(location.path));
  }
  for (const auto &member : members)
    for (const auto &other : members)
      Require(member == other ||
                  other.compare(0, member.size() + 1, member + "/") != 0,
              "Select either a folder or its contents, not both.");
  if (source.archive == target.archive)
    PrepareArchiveMutation(
        Job{Action::Move, transfer.sources, transfer.destination});
  Job move = transfer;
  move.action = Action::MoveArchive;
  move.expectedSources = from.expectedSources;
  move.expectedSources.insert(to.expectedSources.begin(),
                              to.expectedSources.end());
  move.expectedArchiveSets = from.expectedArchiveSets;
  move.expectedArchiveSets.insert(to.expectedArchiveSets.begin(),
                                  to.expectedArchiveSets.end());
  move.destinationFolder = to.destinationFolder;
  move.expectedDestination = to.expectedDestination;
  CheckMoveVolumes(move);
  // Do not refresh stale clipboard/confirmation guards during preparation.
  Guards(transfer);
  VerifyArchiveSets(transfer);
  RequireArchiveVersions(transfer, move);
  return move;
}
void SetArchiveDestination(Job &job, const std::string &folder,
                           const std::string &name) {
  Require(!folder.empty() && folder[0] == '/' && !IsVirtual(folder),
          "Choose a local destination folder.");
  Require(!name.empty() && name.size() < B_FILE_NAME_LENGTH && name != "." &&
              name != ".." &&
              name.find_first_of("/\\\r\n") == std::string::npos,
          "Enter a single file or folder name.");
  VerifyArchiveSets(job);
  Job sourceGuards = job;
  sourceGuards.destinationFolder.clear();
  Guards(sourceGuards);
  struct stat s;
  Require(!stat(folder.c_str(), &s) && S_ISDIR(s.st_mode),
          "Destination is not a folder.");
  job.destinationFolder = folder;
  job.expectedDestination = {int32_t(s.st_dev), int64_t(s.st_ino)};
  auto destination = Join(folder, name);
  struct stat existing;
  Require(lstat(destination.c_str(), &existing) != 0 && errno == ENOENT,
          "That name already exists or is unavailable. Choose a new name.");
  job.entryName = name;
  job.destination = destination;
}
}
