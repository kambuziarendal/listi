// SPDX-License-Identifier: MIT
#include "NamedEntry.h"
#include "UnavailableVolume.h"
#include "FileClipboard.h"
#include "VirtualFile.h"
#include <Entry.h>
#include <Path.h>
#include <StorageDefs.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <stdexcept>
#include <sys/stat.h>
namespace Listi {
namespace {
std::string Folder(const std::string &path) {
  char resolved[PATH_MAX];
  struct stat st;
  if (path.empty() || path[0] != '/' || !realpath(path.c_str(), resolved) ||
      stat(resolved, &st) || !S_ISDIR(st.st_mode))
    throw std::runtime_error(
        "Open an available local destination folder first.");
  return resolved;
}
bool Exists(const std::string &path) {
  struct stat st;
  if (!lstat(path.c_str(), &st))
    return true;
  if (errno != ENOENT)
    throw std::runtime_error("Cannot inspect the chosen name: " +
                             std::string(strerror(errno)));
  return false;
}
std::string Suggest(const std::string &folder, const std::string &leaf,
                    bool duplicate, bool directory) {
  std::string stem = leaf, extension;
  auto dot = leaf.find_last_of('.');
  if (duplicate && !directory && dot != std::string::npos && dot > 0) {
    stem = leaf.substr(0, dot);
    extension = leaf.substr(dot);
  }
  for (int i = 1; i <= 1000; ++i) {
    std::string suffix = duplicate ? " copy" : " link";
    if (i > 1)
      suffix += " " + std::to_string(i);
    std::string name = stem + suffix + extension;
    if (name.size() >= B_FILE_NAME_LENGTH)
      name = std::string(duplicate ? "Copy" : "Link") + " " + std::to_string(i);
    if (!Exists(Join(folder, name)))
      return name;
  }
  throw std::runtime_error(
      "Too many matching names. Choose another destination or rename an entry first.");
}
std::vector<std::string> Components(const std::string &path) {
  std::vector<std::string> parts;
  for (size_t start = 1; start < path.size();) {
    auto end = path.find('/', start);
    parts.push_back(
        path.substr(start, end == std::string::npos ? end : end - start));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return parts;
}
}
void ValidateEntryLeaf(const std::string &name) {
  if (name.empty() || name == "." || name == ".." ||
      name.find('/') != std::string::npos ||
      name.find('\0') != std::string::npos)
    throw std::runtime_error("Enter one file or folder name, without a slash.");
  if (name.size() >= B_FILE_NAME_LENGTH)
    throw std::runtime_error("That name is too long. Use fewer characters.");
}
void ValidateNewEntryName(const std::string &folder, const std::string &name) {
  ValidateEntryLeaf(name);
  if (InterruptedVolumes().ContainsResolved(folder))
    throw std::runtime_error(kUnavailableVolumeText);
  if (Exists(Join(folder, name)))
    throw std::runtime_error(
        "That name already exists. Choose a different name; nothing will be replaced.");
}
Job PrepareNamedEntry(Action action, const std::vector<std::string> &selection,
                      const std::string &linkDestination,
                      const std::map<std::string, NodeIdentity> &expected) {
  if (action != Action::Duplicate && action != Action::Link)
    throw std::runtime_error("Unsupported named entry action.");
  if (selection.size() != 1 || IsVirtual(selection[0]))
    throw std::runtime_error(
        "Select exactly one local file, folder or symbolic link.");
  auto items = ReadFileClipboard(MakeFileSelection(selection, false, expected));
  const auto &item = items.front();
  Job job;
  job.action = action;
  auto split = item.path.find_last_of('/');
  auto parent = Folder(split == 0 ? "/" : item.path.substr(0, split));
  auto source = Join(parent, Leaf(item.path));
  job.sources.push_back(source);
  job.expectedSources.emplace(source, item.node);
  job.destination =
      action == Action::Duplicate ? parent : Folder(linkDestination);
  struct stat destination;
  if (stat(job.destination.c_str(), &destination))
    throw std::runtime_error("The destination folder is unavailable.");
  job.expectedDestination = {int32_t(destination.st_dev),
                             int64_t(destination.st_ino)};
  struct stat st;
  if (lstat(source.c_str(), &st))
    throw std::runtime_error("The source is unavailable.");
  job.entryName = Suggest(job.destination, Leaf(source),
                          action == Action::Duplicate, S_ISDIR(st.st_mode));
  return job;
}
std::string LinkText(const Job &job) {
  if (job.sources.size() != 1)
    throw std::runtime_error("A link needs one target.");
  if (!job.relativeLink)
    return job.sources.front();
  auto target = Components(job.sources.front()),
       parent = Components(job.destination);
  size_t shared = 0;
  while (shared < target.size() && shared < parent.size() &&
         target[shared] == parent[shared])
    ++shared;
  std::string relative;
  for (size_t i = shared; i < parent.size(); ++i)
    relative += "../";
  for (size_t i = shared; i < target.size(); ++i) {
    if (!relative.empty() && relative.back() != '/')
      relative += '/';
    relative += target[i];
  }
  return relative.empty() ? "." : relative;
}
}
