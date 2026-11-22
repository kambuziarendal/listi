// SPDX-License-Identifier: MIT
#include "TransferCommand.h"
#include "ArchiveJobs.h"
#include "JobSnapshot.h"
#include "VirtualFile.h"
#include <cstdlib>
#include <limits.h>
#include <set>
#include <sys/stat.h>

namespace Listi {
namespace {
bool Matches(const struct stat &st, const NodeIdentity &id) {
  return st.st_dev == id.device && st.st_ino == id.inode;
}
std::string ResolveFolder(const std::string &path) {
  char resolved[PATH_MAX];
  if (!realpath(path.c_str(), resolved))
    throw std::runtime_error("Folder unavailable: " + path);
  return resolved;
}
}
bool InTransferSourceFolder(const Job &job) {
  if (job.expectedDestination.device < 0 || IsVirtual(job.destination))
    return false;
  for (const auto &source : job.sources) {
    if (IsVirtual(source))
      continue;
    struct stat parent;
    if (stat(ParentLocation(source).c_str(), &parent))
      throw std::runtime_error("The source folder is unavailable: " + source);
    if (Matches(parent, job.expectedDestination))
      return true;
  }
  return false;
}
Job PrepareTransfer(Job job, bool allowSameFolder) {
  if ((job.action != Action::Copy && job.action != Action::Move) ||
      job.sources.empty())
    throw std::runtime_error("Select entries to copy or move first.");
  const auto target = ParseLocation(job.destination);
  if (target.Local() && (job.destination.empty() || job.destination[0] != '/'))
    throw std::runtime_error("Open a destination folder first.");

  FreezeLocalJob(job, job.destination);
  FreezeArchiveTargets(job);
  if (job.action == Action::Move &&
      (target.Archive() || ParseLocation(job.sources.front()).Archive()))
    return PrepareArchiveMove(job);
  if (target.Archive())
    return PrepareArchiveCopy(job);

  const std::string resolved =
      target.Local() ? ResolveFolder(job.destination) : "";
  std::set<std::string> outputNames;
  for (const auto &source : job.sources) {
    const auto name = LocationLeaf(source);
    if (!outputNames.insert(name).second)
      throw std::runtime_error(
          "Two entries would use the same destination name: " + name);
    const auto from = ParseLocation(source);
    if (job.action == Action::Move && from.Authority() != target.Authority())
      throw std::runtime_error(
          "Use Copy between these locations. The originals are kept.");
    if (!target.Local() || !from.Local())
      continue;
    struct stat st;
    if (lstat(source.c_str(), &st) ||
        !Matches(st, job.expectedSources.at(source)))
      throw std::runtime_error("The selected entry changed: " + source);
    if (S_ISDIR(st.st_mode)) {
      const auto directory = ResolveFolder(source);
      if (directory == "/" || directory == resolved ||
          resolved.rfind(directory + "/", 0) == 0)
        throw std::runtime_error(
            "A folder cannot be transferred into itself or its contents.");
    }
  }
  if (!allowSameFolder && InTransferSourceFolder(job))
    throw std::runtime_error(
        "A selected entry is already in the destination folder.");
  return job;
}
}
