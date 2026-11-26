// SPDX-License-Identifier: MIT
#include "JobSnapshot.h"
#include "UnavailableVolume.h"
#include "VirtualFile.h"
#include <sys/stat.h>
#include <algorithm>
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
BMessage EncodeJob(const Job &job, uint32 what) {
  BMessage m(what);
  m.AddInt32("action", int(job.action));
  m.AddString("archiveFormat", job.archiveFormat.c_str());
  m.AddInt32("archiveLevel", job.archiveLevel);
  m.AddInt64("archivePasswordToken", job.archivePasswordToken);
  m.AddBool("archiveEncryptNames", job.archiveEncryptNames);
  m.AddString("archivePath", job.archivePath.c_str());
  for (const auto &path : job.archiveRemove)
    m.AddString("archiveRemove", path.c_str());
  for (const auto &path : job.archiveFolders)
    m.AddString("archiveFolder", path.c_str());
  for (const auto &rename : job.archiveRenames) {
    BMessage item;
    item.AddString("from", rename.first.c_str());
    item.AddString("to", rename.second.c_str());
    m.AddMessage("archiveRename", &item);
  }
  m.AddString("entryName", job.entryName.c_str());
  m.AddString("destination", job.destination.c_str());
  m.AddString("destinationFolder", job.destinationFolder.c_str());
  m.AddInt32("destinationDevice", job.expectedDestination.device);
  m.AddInt64("destinationNode", job.expectedDestination.inode);
  for (const auto &path : job.sources)
    m.AddString("source", path.c_str());
  for (const auto &rename : job.bulkNames) {
    BMessage item;
    item.AddString("source", rename.first.c_str());
    item.AddString("name", rename.second.c_str());
    m.AddMessage("bulkName", &item);
  }
  for (const auto &entry : job.expectedSources) {
    BMessage item;
    item.AddString("path", entry.first.c_str());
    item.AddInt32("device", entry.second.device);
    item.AddInt64("node", entry.second.inode);
    m.AddMessage("identity", &item);
  }
  for (const auto &[path, entry] : job.expectedRemoteSources) {
    BMessage item;
    item.AddString("path", path.c_str());
    item.AddBool("directory", entry.directory);
    item.AddBool("link", entry.link);
    if (entry.sizeKnown)
      item.AddInt64("size", entry.size);
    if (entry.modifiedKnown) {
      item.AddInt64("modified", entry.modified);
      item.AddString("fraction", entry.modifiedFraction.c_str());
    }
    item.AddString("providerIdentity", entry.providerIdentity.c_str());
    // Retain the original field name for previously encoded Delete jobs.
    m.AddMessage("remoteDelete", &item);
  }
  for (const auto &[source, target] : job.restoreTargets) {
    BMessage item;
    item.AddString("source", source.c_str());
    item.AddString("target", target.c_str());
    m.AddMessage("restoreTarget", &item);
  }
  for (const auto &entry : job.expectedArchiveSets) {
    BMessage item;
    item.AddString("path", entry.first.c_str());
    item.AddString("fingerprint", entry.second.c_str());
    m.AddMessage("archiveSet", &item);
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
  const char *path;
  for (int32 i = 0; m.FindString("source", i, &path) == B_OK; ++i)
    job.sources.emplace_back(path);
  for (int32 i = 0; m.FindString("archiveRemove", i, &path) == B_OK; ++i)
    job.archiveRemove.emplace_back(path);
  for (int32 i = 0; m.FindString("archiveFolder", i, &path) == B_OK; ++i)
    job.archiveFolders.emplace_back(path);
  BMessage item;
  for (int32 i = 0; m.FindMessage("archiveRename", i, &item) == B_OK; ++i)
    job.archiveRenames[item.GetString("from", "")] = item.GetString("to", "");
  for (int32 i = 0; m.FindMessage("bulkName", i, &item) == B_OK; ++i)
    job.bulkNames[item.GetString("source", "")] = item.GetString("name", "");
  for (int32 i = 0; m.FindMessage("identity", i, &item) == B_OK; ++i)
    job.expectedSources[item.GetString("path", "")] = {
        item.GetInt32("device", -1), item.GetInt64("node", -1)};
  for (int32 i = 0; m.FindMessage("remoteDelete", i, &item) == B_OK; ++i) {
    FileEntry entry;
    bool directory, link;
    if (item.FindString("path", &path) != B_OK ||
        item.FindBool("directory", &directory) != B_OK || item.FindBool("link", &link) != B_OK)
      continue;
    entry.directory = directory;
    entry.link = link;
    entry.sizeKnown = item.HasInt64("size");
    entry.size = item.GetInt64("size", 0);
    entry.modifiedKnown = item.HasInt64("modified");
    entry.modified = item.GetInt64("modified", 0);
    entry.modifiedFraction = item.GetString("fraction", "");
    entry.providerIdentity = item.GetString("providerIdentity", "");
    job.expectedRemoteSources.emplace(path, std::move(entry));
  }
  for (int32 i = 0; m.FindMessage("archiveSet", i, &item) == B_OK; ++i)
    job.expectedArchiveSets[item.GetString("path", "")] =
        item.GetString("fingerprint", "");
  for (int32 i = 0; m.FindMessage("restoreTarget", i, &item) == B_OK; ++i)
    job.restoreTargets[item.GetString("source", "")] = item.GetString("target", "");
  return job;
}
std::optional<Job> DecodeSubmittedJob(const BMessage &message,
                                      std::initializer_list<Action> allowed) {
  int32 value;
  if (message.IsSourceRemote() || message.FindInt32("action", &value) != B_OK ||
      std::find(allowed.begin(), allowed.end(), static_cast<Action>(value)) ==
          allowed.end())
    return std::nullopt;
  return DecodeJob(message);
}
}
