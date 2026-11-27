// SPDX-License-Identifier: MIT
#include "JobSnapshot.h"
#include "UnavailableVolume.h"
#include "VirtualFile.h"
#include <sys/stat.h>
#include <algorithm>
#include <set>
namespace Listi {
void FreezeLocalJob(Job &job, const std::string &folder) {
  if (InterruptedVolumes().Contains(folder))
    throw std::runtime_error(kUnavailableVolumeText);
  for (const auto &path : job.sources) {
    if (InterruptedVolumes().Contains(path))
      throw std::runtime_error(kUnavailableVolumeText);
    if (IsVirtual(path))
      continue;
    struct stat st;
    if (lstat(path.c_str(), &st))
      throw std::runtime_error(
          "Selected entry is unavailable. Select it again.");
    auto inserted = job.expectedSources.emplace(
        path, NodeIdentity{int32_t(st.st_dev), int64_t(st.st_ino)});
    const auto &expected = inserted.first->second;
    if (expected.device != st.st_dev || expected.inode != st.st_ino)
      throw std::runtime_error("The selected entry was replaced: " + path);
  }
  if (!folder.empty() && !IsVirtual(folder)) {
    struct stat st;
    if (stat(folder.c_str(), &st) || !S_ISDIR(st.st_mode))
      throw std::runtime_error("The destination folder is unavailable.");
    if (job.expectedDestination.device >= 0 &&
        (job.expectedDestination.device != st.st_dev ||
         job.expectedDestination.inode != st.st_ino))
      throw std::runtime_error(
          "The selected destination folder was replaced: " + folder);
    job.expectedDestination = {int32_t(st.st_dev), int64_t(st.st_ino)};
    job.destinationFolder = folder;
  }
}
void RequireJobField(status_t status) {
  if (status != B_OK)
    throw std::runtime_error("Cannot prepare the complete job. Nothing was submitted. Try again.");
}
namespace {
void RequireSnapshot(bool ok) {
  if (!ok)
    throw std::runtime_error("Incomplete job selection. Nothing was submitted. Select the entries again.");
}
int32 FieldCount(const BMessage &message, const char *field, type_code expectedType) {
  type_code type;
  int32 count;
  const auto status = message.GetInfo(field, &type, &count);
  if (status == B_NAME_NOT_FOUND)
    return 0;
  RequireSnapshot(status == B_OK && type == expectedType && count > 0);
  return count;
}
std::string RequiredString(const BMessage &message, const char *field) {
  const char *value;
  RequireSnapshot(message.FindString(field, &value) == B_OK && *value);
  return value;
}
template<class F>
void ReadItems(const BMessage &message, const char *field, F read) {
  const auto count = FieldCount(message, field, B_MESSAGE_TYPE);
  for (int32 i = 0; i < count; ++i) {
    BMessage item;
    RequireSnapshot(message.FindMessage(field, i, &item) == B_OK);
    read(item);
  }
}
void ReadPaths(const BMessage &message, const char *field, std::vector<std::string> &paths) {
  const auto count = FieldCount(message, field, B_STRING_TYPE);
  for (int32 i = 0; i < count; ++i) {
    const char *path;
    RequireSnapshot(message.FindString(field, i, &path) == B_OK && *path);
    paths.emplace_back(path);
  }
}
} // namespace

BMessage EncodeJob(const Job &job, uint32 what) {
  BMessage m(what);
  RequireJobField(m.AddInt32("action", int(job.action)));
  RequireJobField(m.AddInt64("sourceCount", static_cast<int64>(job.sources.size())));
  RequireJobField(m.AddString("archiveFormat", job.archiveFormat.c_str()));
  RequireJobField(m.AddInt32("archiveLevel", job.archiveLevel));
  RequireJobField(m.AddInt64("archivePasswordToken", job.archivePasswordToken));
  RequireJobField(m.AddBool("archiveEncryptNames", job.archiveEncryptNames));
  RequireJobField(m.AddString("archivePath", job.archivePath.c_str()));
  for (const auto &path : job.archiveRemove)
    RequireJobField(m.AddString("archiveRemove", path.c_str()));
  for (const auto &path : job.archiveFolders)
    RequireJobField(m.AddString("archiveFolder", path.c_str()));
  for (const auto &rename : job.archiveRenames) {
    BMessage item;
    RequireJobField(item.AddString("from", rename.first.c_str()));
    RequireJobField(item.AddString("to", rename.second.c_str()));
    RequireJobField(m.AddMessage("archiveRename", &item));
  }
  RequireJobField(m.AddString("entryName", job.entryName.c_str()));
  RequireJobField(m.AddString("destination", job.destination.c_str()));
  RequireJobField(m.AddString("destinationFolder", job.destinationFolder.c_str()));
  RequireJobField(m.AddInt32("destinationDevice", job.expectedDestination.device));
  RequireJobField(m.AddInt64("destinationNode", job.expectedDestination.inode));
  for (const auto &path : job.sources)
    RequireJobField(m.AddString("source", path.c_str()));
  for (const auto &rename : job.bulkNames) {
    BMessage item;
    RequireJobField(item.AddString("source", rename.first.c_str()));
    RequireJobField(item.AddString("name", rename.second.c_str()));
    RequireJobField(m.AddMessage("bulkName", &item));
  }
  for (const auto &entry : job.expectedSources) {
    BMessage item;
    RequireJobField(item.AddString("path", entry.first.c_str()));
    RequireJobField(item.AddInt32("device", entry.second.device));
    RequireJobField(item.AddInt64("node", entry.second.inode));
    RequireJobField(m.AddMessage("identity", &item));
  }
  for (const auto &[path, entry] : job.expectedRemoteSources) {
    BMessage item;
    RequireJobField(item.AddString("path", path.c_str()));
    RequireJobField(item.AddBool("directory", entry.directory));
    RequireJobField(item.AddBool("link", entry.link));
    if (entry.permissionsKnown)
      RequireJobField(item.AddInt32("permissions", entry.permissions));
    if (entry.sizeKnown)
      RequireJobField(item.AddInt64("size", entry.size));
    if (entry.modifiedKnown) {
      RequireJobField(item.AddInt64("modified", entry.modified));
      RequireJobField(item.AddString("fraction", entry.modifiedFraction.c_str()));
    }
    RequireJobField(item.AddString("providerIdentity", entry.providerIdentity.c_str()));
    // Retain the original field name for previously encoded Delete jobs.
    RequireJobField(m.AddMessage("remoteDelete", &item));
  }
  for (const auto &[source, target] : job.restoreTargets) {
    BMessage item;
    RequireJobField(item.AddString("source", source.c_str()));
    RequireJobField(item.AddString("target", target.path.c_str()));
    if (!target.path.empty()) {
      RequireJobField(item.AddInt32("parentDevice", target.parent.device));
      RequireJobField(item.AddInt64("parentNode", target.parent.inode));
    }
    RequireJobField(m.AddMessage("restoreTarget", &item));
  }
  for (const auto &entry : job.expectedArchiveSets) {
    BMessage item;
    RequireJobField(item.AddString("path", entry.first.c_str()));
    RequireJobField(item.AddString("fingerprint", entry.second.c_str()));
    RequireJobField(m.AddMessage("archiveSet", &item));
  }
  return m;
}
Job DecodeJob(const BMessage &m) {
  Job job;
  job.action = static_cast<Action>(m.GetInt32("action", 0));
  job.archiveFormat = m.GetString("archiveFormat", "zip");
  job.archiveLevel = m.GetInt32("archiveLevel", 6);
  job.archivePasswordToken = m.GetInt64("archivePasswordToken", 0);
  job.archiveEncryptNames = m.GetBool("archiveEncryptNames", false);
  job.archivePath = m.GetString("archivePath", "");
  job.entryName = m.GetString("entryName", "");
  job.destination = m.GetString("destination", "");
  job.destinationFolder = m.GetString("destinationFolder", "");
  job.expectedDestination = {m.GetInt32("destinationDevice", -1),
                             m.GetInt64("destinationNode", -1)};
  ReadPaths(m, "source", job.sources);
  // Legacy snapshots have no declared count. Their selection and identities
  // still have to agree when submitted through a confirmation.
  if (FieldCount(m, "sourceCount", B_INT64_TYPE))
    RequireSnapshot(m.GetInt64("sourceCount", -1) == static_cast<int64>(job.sources.size()));
  ReadPaths(m, "archiveRemove", job.archiveRemove);
  ReadPaths(m, "archiveFolder", job.archiveFolders);
  ReadItems(m, "archiveRename", [&](const BMessage &item) {
    RequireSnapshot(job.archiveRenames.emplace(RequiredString(item, "from"), RequiredString(item, "to")).second);
  });
  ReadItems(m, "bulkName", [&](const BMessage &item) {
    RequireSnapshot(job.bulkNames.emplace(RequiredString(item, "source"), RequiredString(item, "name")).second);
  });
  ReadItems(m, "identity", [&](const BMessage &item) {
    NodeIdentity identity{item.GetInt32("device", -1), item.GetInt64("node", -1)};
    RequireSnapshot(identity.device >= 0 && identity.inode >= 0 &&
                    job.expectedSources.emplace(RequiredString(item, "path"), identity).second);
  });
  ReadItems(m, "remoteDelete", [&](const BMessage &item) {
    FileEntry entry;
    RequireSnapshot(item.FindBool("directory", &entry.directory) == B_OK &&
                    item.FindBool("link", &entry.link) == B_OK);
    entry.permissionsKnown = FieldCount(item, "permissions", B_INT32_TYPE) != 0;
    if (entry.permissionsKnown) {
      const auto mode = item.GetInt32("permissions", -1);
      RequireSnapshot(mode >= 0 && mode <= 07777);
      entry.permissions = mode;
    }
    entry.sizeKnown = FieldCount(item, "size", B_INT64_TYPE) != 0;
    entry.size = item.GetInt64("size", 0);
    entry.modifiedKnown = FieldCount(item, "modified", B_INT64_TYPE) != 0;
    entry.modified = item.GetInt64("modified", 0);
    entry.modifiedFraction = item.GetString("fraction", "");
    entry.providerIdentity = item.GetString("providerIdentity", "");
    RequireSnapshot(job.expectedRemoteSources.emplace(RequiredString(item, "path"), std::move(entry)).second);
  });
  ReadItems(m, "archiveSet", [&](const BMessage &item) {
    RequireSnapshot(job.expectedArchiveSets.emplace(RequiredString(item, "path"), RequiredString(item, "fingerprint")).second);
  });
  ReadItems(m, "restoreTarget", [&](const BMessage &item) {
    // An empty (but present) target records an unavailable original path.
    const char *target;
    RequireSnapshot(item.FindString("target", &target) == B_OK);
    NodeIdentity parent{item.GetInt32("parentDevice", -1), item.GetInt64("parentNode", -1)};
    RequireSnapshot(!*target || (parent.device >= 0 && parent.inode >= 0));
    RequireSnapshot(job.restoreTargets.emplace(RequiredString(item, "source"), RestoreTarget{target, parent}).second);
  });
  return job;
}
std::optional<Job> DecodeSubmittedJob(const BMessage &message,
                                      std::initializer_list<Action> allowed) {
  int32 value;
  if (message.IsSourceRemote() || message.FindInt32("action", &value) != B_OK ||
      std::find(allowed.begin(), allowed.end(), static_cast<Action>(value)) ==
          allowed.end())
    return std::nullopt;
  try {
    auto job = DecodeJob(message);
    const bool create = job.action == Action::MakeFile || job.action == Action::MakeDirectory;
    const bool edit = job.action == Action::EditArchive;
    const bool archiveMutation = edit && (!job.archiveRemove.empty() || !job.archiveRenames.empty() ||
                                           !job.archiveFolders.empty());
    if ((create && !job.sources.empty()) || (!create && !archiveMutation && job.sources.empty()))
      return std::nullopt;
    if (!job.destinationFolder.empty() &&
        (job.expectedDestination.device < 0 || job.expectedDestination.inode < 0))
      return std::nullopt;
    // Extract can guard additional archive volumes. Edit and MoveArchive
    // also guard the containers, not just individually selected local paths.
    const bool extraLocalGuards = edit || job.action == Action::ExtractArchive || job.action == Action::MoveArchive;
    std::set<std::string> distinct;
    size_t local = 0, remote = 0;
    for (const auto &path : job.sources) {
      if (!distinct.insert(path).second)
        return std::nullopt;
      const auto location = ParseLocation(path);
      if (location.Local()) {
        ++local;
        if (!job.expectedSources.count(path))
          return std::nullopt;
      } else if (!location.Archive()) {
        ++remote;
        const bool mutation = job.action == Action::Delete || job.action == Action::Rename ||
                              job.action == Action::Move;
        // Older read-only drags can supply URLs without revision metadata.
        if ((mutation || !job.expectedRemoteSources.empty()) && !job.expectedRemoteSources.count(path))
          return std::nullopt;
      }
      if (job.action == Action::BulkRename && !job.bulkNames.count(path))
        return std::nullopt;
      if (job.action == Action::Restore && !job.restoreTargets.count(path))
        return std::nullopt;
    }
    if ((!extraLocalGuards && job.expectedSources.size() != local) ||
        (!job.expectedRemoteSources.empty() && job.expectedRemoteSources.size() != remote))
      return std::nullopt;
    return job;
  } catch (const std::runtime_error &) {
    return std::nullopt;
  }
}
}
