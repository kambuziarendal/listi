// SPDX-License-Identifier: MIT
#include "PaneModel.h"
#include "NameFilter.h"
#include "VirtualFile.h"
#include <algorithm>
#include <fnmatch.h>
#include <limits>
#include <stdexcept>
#include <strings.h>
std::string FileTypeKey(const std::string &name) {
  std::string folded = name;
  for (char &c : folded)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  // A compressed TAR is a different type from a single compressed stream.
  for (const char *suffix :
       {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.lzma", ".tar.lz", ".tar.zst",
        ".tar.lz4", ".tar.z"}) {
    const std::string ending = suffix;
    if (folded.size() > ending.size() &&
        folded.compare(folded.size() - ending.size(), ending.size(), ending) ==
            0)
      return ending;
  }
  auto dot = folded.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 == folded.size())
    return {};
  return folded.substr(dot);
}
std::string PaneModel::FocusedFileType() const {
  auto entry = Focused();
  return entry && !entry->unmountedDisk && !entry->directory && !entry->parent && entry->kind != 2
             ? FileTypeKey(entry->name)
             : std::string();
}
bool PaneModel::SelectSameFileType() {
  const auto type = FocusedFileType();
  if (type.empty())
    return false;
  selection.clear();
  for (auto entry : visible)
    if (!entry->parent && !entry->unmountedDisk && !entry->directory && entry->kind != 2 &&
        FileTypeKey(entry->name) == type)
      selection.insert(entry->path);
  anchor = focused;
  return true;
}
size_t PaneModel::MatchPattern(const char *pattern, bool apply) {
  size_t matches = 0;
  for (const auto *entry : visible) {
    if (entry->parent || fnmatch(pattern, entry->name.c_str(), 0) != 0)
      continue;
    if (apply) {
      if (matches == 0) {
        selection.clear();
        focused = anchor = entry->path;
      }
      selection.insert(entry->path);
    }
    ++matches;
  }
  return matches;
}
bool PaneModel::MatchesSnapshot(const std::vector<std::unique_ptr<PaneEntry>> &rows) const {
  if (entries.size() != rows.size()) return false;
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto &a = *entries[i], &b = *rows[i];
    if (a.path != b.path || a.device != b.device || a.node != b.node ||
        a.kind != b.kind || a.directory != b.directory || a.size != b.size ||
        a.modified != b.modified)
      return false;
  }
  return true;
}
void PaneModel::BindArchiveView(Listi::Job &job, const std::string &location) const {
  const auto from = Listi::ParseLocation(location);
  if (!from.Archive())
    return;
  if (archiveVersion.empty() ||
      job.expectedArchiveSets.emplace(from.archive, archiveVersion).first->second != archiveVersion)
    throw std::runtime_error("Archive view is incomplete or changed. Reload and select again.");
}
void PaneModel::Clear() {
  archiveVersion.clear();
  visible.clear();
  entries.clear();
  selection.clear();
  focused.clear();
  anchor.clear();
}
void PaneModel::Add(std::unique_ptr<PaneEntry> entry) {
  entries.push_back(std::move(entry));
}
void PaneModel::AddScanned(std::unique_ptr<PaneEntry> entry, const char *query,
                           bool restoreSelection) {
  if (Listi::MatchesNameFilter(entry->name.c_str(), query, entry->parent)) {
    visible.push_back(entry.get());
    if (restoreSelection && !entry->parent)
      selection.insert(entry->path);
  }
  entries.push_back(std::move(entry));
}
void PaneModel::Filter(const std::string &query) {
  visible.clear();
  for (auto &entry : entries) {
    if (Listi::MatchesNameFilter(entry->name.c_str(), query.c_str(),
                                 entry->parent))
      visible.push_back(entry.get());
    else {
      selection.erase(entry->path);
      if (focused == entry->path)
        focused.clear();
      if (anchor == entry->path)
        anchor.clear();
    }
  }
  Sort(sorting);
}
void PaneModel::Sort(PaneSort value) {
  sorting = value;
  if (sorting.column < 0 || sorting.column > 2)
    sorting.column = 0;
  std::stable_sort(visible.begin(), visible.end(), [this](auto a, auto b) {
    int order = 0;
    if (sorting.column == 1) {
      const auto x = a->directory || !a->sizeKnown ? -1 : a->size;
      const auto y = b->directory || !b->sizeKnown ? -1 : b->size;
      order = (x > y) - (x < y);
    } else if (sorting.column == 2) {
      order = (a->modified > b->modified) - (a->modified < b->modified);
    } else {
      if (a->parent != b->parent)
        order = a->parent ? -1 : 1;
      else if ((a->kind == 1) != (b->kind == 1))
        order = a->kind == 1 ? -1 : 1;
      else
        order = strcasecmp(a->name.c_str(), b->name.c_str());
    }
    return sorting.ascending ? order < 0 : order > 0;
  });
}

std::vector<std::string> PaneModel::Selected() const {
  std::vector<std::string> result;
  for (auto entry : visible)
    if (!entry->parent && IsSelected(entry->path))
      result.push_back(entry->path);
  return result;
}
std::vector<std::string> PaneModel::SelectedFiles() const {
  std::vector<std::string> result;
  for (const auto *row : visible) {
    if (row->parent || !IsSelected(row->path))
      continue;
    if (row->unmountedDisk)
      return {};
    result.push_back(row->path);
  }
  return result;
}
std::map<std::string, Listi::NodeIdentity>
PaneModel::SelectedLocalIdentities(size_t limit) const {
  if (selection.size() > limit)
    throw std::runtime_error("Select at most " + std::to_string(limit) + " entries.");
  std::map<std::string, Listi::NodeIdentity> nodes;
  for (const auto *row : visible) {
    if (row->parent || !IsSelected(row->path) || row->path.empty() ||
        row->path.front() != '/')
      continue;
    if (row->device < 0 || row->device > std::numeric_limits<int32_t>::max() ||
        row->node < 0)
      throw std::runtime_error("Refresh and select the entry again: " + row->path);
    const Listi::NodeIdentity identity{int32_t(row->device), row->node};
    if (!nodes.emplace(row->path, identity).second)
      throw std::runtime_error("Duplicate selected row. Refresh the folder: " +
                               row->path);
  }
  return nodes;
}

std::map<std::string, Listi::FileEntry> PaneModel::SelectedRemoteEntries() const {
  std::map<std::string, Listi::FileEntry> revisions;
  for (const auto *row : visible) {
    if (row->parent || !IsSelected(row->path) || row->path.empty() ||
        row->path.front() == '/')
      continue;
    auto [it, inserted] = revisions.try_emplace(row->path);
    if (!inserted)
      throw std::runtime_error("Duplicate selected row. Refresh the folder: " + row->path);
    auto &entry = it->second;
    entry.directory = row->directory;
    entry.link = row->kind == 2;
    entry.sizeKnown = row->sizeKnown;
    entry.size = row->size;
    entry.permissionsKnown = row->permissions >= 0;
    if (entry.permissionsKnown)
      entry.permissions = row->permissions;
    entry.modifiedKnown = row->modifiedKnown;
    entry.modified = row->modified;
    entry.modifiedFraction = row->modifiedFraction;
    entry.providerIdentity = row->providerIdentity;
  }
  return revisions;
}

const PaneEntry *PaneModel::OnlySelected() const {
  if (selection.size() != 1)
    return nullptr;
  const PaneEntry *match = nullptr;
  for (const auto *row : visible) {
    if (row->parent || row->path != *selection.begin())
      continue;
    // Duplicate visible rows with the same path aren't an unambiguous selection.
    if (match)
      return nullptr;
    match = row;
  }
  return match;
}

SelectionSummary PaneModel::Summary() const {
  SelectionSummary result;
  // Cached listing metadata only: no filesystem/network calls during selection.
  for (auto entry : visible) {
    if (entry->parent)
      continue;
    ++result.visible;
    if (!IsSelected(entry->path))
      continue;
    ++result.selected;
    if (entry->kind == 2) {
      ++result.links;
      continue;
    }
    if (entry->directory) {
      ++result.folders;
      continue;
    }
    ++result.files;
    if (!entry->sizeKnown || entry->size < 0) {
      ++result.unknown;
      continue;
    }
    const auto bytes = static_cast<uint64_t>(entry->size);
    if (bytes > std::numeric_limits<uint64_t>::max() - result.bytes) {
      result.bytes = std::numeric_limits<uint64_t>::max();
      result.overflow = true;
    } else
      result.bytes += bytes;
  }
  return result;
}
void PaneModel::Select(int mode) {
  if (mode == 0) {
    selection.clear();
    return;
  }
  for (auto entry : visible) {
    if (entry->parent)
      continue;
    if (mode == 2 && IsSelected(entry->path))
      selection.erase(entry->path);
    else
      selection.insert(entry->path);
  }
}
void PaneModel::Pick(int index, bool extend, bool toggle) {
  if (index < 0 || size_t(index) >= visible.size())
    return;
  auto row = visible[index];
  if (extend && !toggle) {
    auto start = std::find_if(visible.begin(), visible.end(),
                              [&](auto e) { return e->path == anchor; });
    int from = start == visible.end() ? index : int(start - visible.begin());
    if (start == visible.end())
      anchor = row->path;
    selection.clear();
    for (int i = std::min(from, index); i <= std::max(from, index); ++i)
      if (!visible[i]->parent)
        selection.insert(visible[i]->path);
  } else {
    if (!toggle)
      selection.clear();
    if (toggle && IsSelected(row->path))
      selection.erase(row->path);
    else if (!row->parent)
      selection.insert(row->path);
    anchor = row->path;
  }
  focused = row->path;
}
void PaneModel::RestoreSelection(const std::vector<std::string> &paths) {
  const std::set<std::string> wanted(paths.begin(), paths.end());
  for (auto entry : visible)
    if (!entry->parent && wanted.count(entry->path))
      selection.insert(entry->path);
}
void PaneModel::SetSelection(const std::vector<std::string> &paths,
                             const std::string &focus) {
  selection.clear();
  RestoreSelection(paths);
  focused = focus;
  anchor = focus;
}
PaneEntry *PaneModel::Focused() const {
  if (focused.empty() && selection.empty())
    return nullptr;
  for (auto entry : visible)
    if (entry->path == focused)
      return entry;
  for (auto entry : visible)
    if (IsSelected(entry->path))
      return entry;
  return nullptr;
}
bool PaneModel::Transfer(const std::string &path, int state, float fraction) {
  for (auto &entry : entries)
    if (entry->path == path) {
      entry->transferState = state;
      entry->fraction = fraction;
      return true;
    }
  return false;
}
