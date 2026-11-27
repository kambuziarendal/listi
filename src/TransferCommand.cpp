// SPDX-License-Identifier: MIT
#include "TransferCommand.h"
#include "ArchiveJobs.h"
#include "JobSnapshot.h"
#include "VirtualFile.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
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
using Node = std::pair<dev_t, ino_t>;

struct SelectionTrees {
  const TransferControl &control;
  std::set<Node> roots, ancestors;
  static bool SameEntry(const std::string &source, const Node &node) {
    struct stat st;
    return lstat(source.c_str(), &st) == 0 &&
           Node(st.st_dev, st.st_ino) == node;
  }
  // Only descend through a merge when the destination lies on the path to a
  // selected root. Query projected child names on the destination filesystem,
  // which may have different case and normalization rules than the source.
  void CheckMerge(const std::string &source, const std::string &target,
                  unsigned depth) const {
    control.Check();
    struct stat output;
    if (lstat(target.c_str(), &output)) {
      if (errno == ENOENT)
        return;
      throw std::runtime_error("Cannot check destination entry: " + target);
    }
    const Node node{output.st_dev, output.st_ino};
    if (roots.count(node) && (depth || !SameEntry(source, node)))
      throw std::runtime_error("A destination folder contains a selected source: " +
                               target + ". Choose another destination.");
    if (!ancestors.count(node))
      return;
    struct stat input;
    if (lstat(source.c_str(), &input) || depth > 64)
      throw std::runtime_error("Cannot check overlap with selected sources: " + source);
    if (!S_ISDIR(input.st_mode))
      return; // File operations never replace a directory with a non-directory.
    std::unique_ptr<DIR, decltype(&closedir)> children(opendir(source.c_str()), closedir);
    if (!children)
      throw std::runtime_error("Cannot check source folder: " + source);
    for (;;) {
      errno = 0;
      auto child = readdir(children.get());
      if (!child) {
        if (errno)
          throw std::runtime_error("Cannot finish checking source folder: " + source);
        break;
      }
      if (strcmp(child->d_name, ".") && strcmp(child->d_name, ".."))
        CheckMerge(ChildLocation(source, child->d_name),
                   ChildLocation(target, child->d_name), depth + 1);
    }
  }
};
}
void CheckTransferOutputTrees(const std::vector<std::string> &sources,
                              const std::string &destination,
                              const std::function<bool()> &proceed) {
  const TransferControl control{proceed};
  SelectionTrees selection{control, {}, {}};
  std::set<std::string> checked;
  for (const auto &source : sources) {
    control.Check();
    if (IsVirtual(source))
      continue;
    struct stat st;
    if (lstat(source.c_str(), &st))
      throw std::runtime_error("Cannot check selected source: " + source);
    selection.roots.emplace(st.st_dev, st.st_ino);
    auto parent = ResolveFolder(ParentLocation(source));
    while (checked.insert(parent).second) {
      control.Check();
      if (stat(parent.c_str(), &st))
        throw std::runtime_error("Cannot check source folder: " + parent);
      selection.ancestors.emplace(st.st_dev, st.st_ino);
      if (parent == "/")
        break;
      parent = ParentLocation(parent);
    }
  }
  // A different selected directory can contain the destination itself.
  for (auto parent = ResolveFolder(destination);; parent = ParentLocation(parent)) {
    control.Check();
    struct stat st;
    if (stat(parent.c_str(), &st))
      throw std::runtime_error("Cannot check destination folder: " + parent);
    if (selection.roots.count({st.st_dev, st.st_ino}))
      throw std::runtime_error("A selected folder contains the destination: " + parent);
    if (parent == "/")
      break;
  }
  // RunVirtual checks projected provider/archive children against these same
  // selected inputs before writing. They cannot be enumerated with opendir.
  for (const auto &source : sources) {
    if (!IsVirtual(source))
      selection.CheckMerge(source, ChildLocation(destination, LocationLeaf(source)), 0);
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
    if (job.action == Action::Move && from.CanonicalAuthority() != target.CanonicalAuthority())
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
