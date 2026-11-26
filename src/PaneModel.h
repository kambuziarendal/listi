// SPDX-License-Identifier: MIT
#pragma once
#include "FileOperation.h"
#include "DiskActions.h"
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>
class BBitmap;
struct AttributeCellPreview {
  std::string text;
  uint32_t type = 0;
};
struct PaneEntry {
  std::string name, path;
  std::vector<AttributeCellPreview> attributeCells;
  bool directory = false, parent = false, sizeKnown = true;
  int kind = 0, transferState = 0;
  int64_t device = -1, node = -1;
  int64_t size = 0, modified = 0;
  bool modifiedKnown = false;
  std::string modifiedFraction, providerIdentity;
  float fraction = 0;
  std::shared_ptr<BBitmap> icon;
  bool iconRequested = false;
  std::unique_ptr<Listi::DiskPartition> partition;
  bool unmountedDisk = false;
};
std::string FileTypeKey(const std::string &name);
struct PaneSort {
  int column = 0;
  bool ascending = true;
};
struct SelectionSummary {
  size_t visible = 0, selected = 0, files = 0, folders = 0, links = 0,
         unknown = 0;
  uint64_t bytes = 0;
  bool overflow = false;
};
// Pane owns entries and selection. Renderers never own each other's state.
// All access occurs on the window thread; scanner messages carry values only.
class PaneModel {
public:
  void Clear();
  bool MatchesSnapshot(const std::vector<std::unique_ptr<PaneEntry>> &) const;
  void Add(std::unique_ptr<PaneEntry> entry);
  // Scans expose matching rows immediately, then sort once at completion.
  void AddScanned(std::unique_ptr<PaneEntry> entry, const char *query,
                  bool restoreSelection);
  void Filter(const std::string &query);
  void Sort(PaneSort value);
  PaneSort Sorting() const { return sorting; }
  const std::vector<PaneEntry *> &Visible() const { return visible; }
  size_t HiddenCount() const { return entries.size() - visible.size(); }
  bool IsSelected(const std::string &path) const {
    return selection.count(path);
  }
  std::vector<std::string> Selected() const;
  // Non-filesystem disk rows must never become file-operation sources.
  std::vector<std::string> SelectedFiles() const;
  std::map<std::string, Listi::NodeIdentity> SelectedLocalIdentities(size_t limit = SIZE_MAX) const;
  std::map<std::string, Listi::FileEntry> SelectedRemoteEntries() const;
  const PaneEntry *OnlySelected() const;
  SelectionSummary Summary() const;
  void Select(int mode);
  std::string FocusedFileType() const;
  bool SelectSameFileType();
  size_t MatchPattern(const char *pattern, bool apply);
  void Pick(int index, bool extend, bool toggle);
  void RestoreSelection(const std::vector<std::string> &paths);
  void SetSelection(const std::vector<std::string> &paths,
                    const std::string &focus);
  PaneEntry *Focused() const;
  bool Transfer(const std::string &path, int state, float fraction);

private:
  PaneSort sorting;
  std::vector<std::unique_ptr<PaneEntry>> entries;
  std::vector<PaneEntry *> visible;
  std::set<std::string> selection;
  std::string focused, anchor;
};
