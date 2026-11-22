// SPDX-License-Identifier: MIT
#pragma once
#include <Message.h>
#include <cstddef>
#include <string>
#include <vector>

// Values are the existing on-disk workspace IDs. Never reorder.
enum class Workspace : int32 { tviskipt = 0, einn = 1, askr = 2, smidja = 3 };
inline const char *WorkspaceId(Workspace workspace) {
  switch (workspace) {
  case Workspace::einn:
    return "einn";
  case Workspace::askr:
    return "askr";
  case Workspace::smidja:
    return "smidja";
  default:
    return "tviskipt";
  }
}
inline const char *WorkspaceName(Workspace workspace) {
  switch (workspace) {
  case Workspace::einn:
    return "Einn";
  case Workspace::askr:
    return "Askr";
  case Workspace::smidja:
    return "Smiðja";
  default:
    return "Tvískipt";
  }
}
inline const char *WorkspaceMenuLabel(Workspace workspace) {
  switch (workspace) {
  case Workspace::einn:
    return "Einn: simple single-panel browsing";
  case Workspace::askr:
    return "Askr: folder tree and file list";
  case Workspace::smidja:
    return "Smiðja: workbench with more tools";
  default:
    return "Tvískipt: side-by-side file panels";
  }
}
constexpr int32 MillerMode = 4;
constexpr int32 ViewModeCount = 5;
// A recursive scan is explicitly requested, not restored as a folder default.
constexpr int32 FlatMode = ViewModeCount;
inline const char *ViewModeLabel(int32 mode) {
  static const char *const labels[] = {"Details", "Icons", "Large icons",
                                       "Thumbnails", "Miller columns", "Flat View"};
  return mode >= 0 && mode <= FlatMode ? labels[mode] : "Details";
}

// Persisted sidebar mode IDs. Append new modes without reordering.
enum class SidebarMode : int32 {
  Hidden = 0, FolderTree = 1, Favorites = 2, ShortcutsAndTree = 3, Shelf = 4,
  RecentFolders = 5
};
constexpr SidebarMode SidebarModes[] = {
    SidebarMode::Hidden, SidebarMode::FolderTree, SidebarMode::Favorites,
    SidebarMode::ShortcutsAndTree, SidebarMode::Shelf, SidebarMode::RecentFolders};
inline bool IsVisibleSidebar(int32 mode) {
  return mode >= static_cast<int32>(SidebarMode::FolderTree) &&
         mode <= static_cast<int32>(SidebarMode::RecentFolders);
}
inline bool SidebarHasTree(SidebarMode mode) {
  return mode == SidebarMode::FolderTree || mode == SidebarMode::ShortcutsAndTree;
}
inline const char *SidebarModeLabel(SidebarMode mode) {
  switch (mode) {
  case SidebarMode::FolderTree:
    return "File tree";
  case SidebarMode::Favorites:
    return "Shortcuts";
  case SidebarMode::ShortcutsAndTree:
    return "Shortcuts above tree";
  case SidebarMode::Shelf:
    return "Shelf";
  case SidebarMode::RecentFolders:
    return "Recent folders";
  default:
    return "Hidden";
  }
}

// A snapshot of layout controls only. Folder-specific views, addresses, and
// general preferences are deliberately not part of a named profile.
struct WorkspaceLayout {
  static constexpr int32 ActionButtonCount = 13;
  bool buttons = true, command = false, fullActionButtons = false;
  bool bankTop = false, commandTop = false, inspectorLeft = false;
  bool panelsStacked = false;
  bool treeRight = false, filterBottom = false, adaptive = false;
  std::vector<int32> actionButtons = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  float splitWeights[3] = {0.39f, 0.39f, 0.22f};
  int32 defaultModes[4] = {};
  int32 paneOverrides[4][2] = {{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}};
  int32 DefaultMode(Workspace w) const { return defaultModes[int(w)]; }
  void SetDefaultMode(Workspace w, int32 mode) {
    if (mode < 0 || mode >= ViewModeCount || DefaultMode(w) == mode)
      return;
    defaultModes[int(w)] = mode;
    paneOverrides[int(w)][0] = paneOverrides[int(w)][1] = -1;
  }
  bool HasPaneMode(int pane) const {
    return paneOverrides[int(workspace)][pane] >= 0;
  }
  int32 PaneMode(int pane) const {
    return HasPaneMode(pane) ? paneOverrides[int(workspace)][pane]
                             : DefaultMode(workspace);
  }
  void SetPaneMode(int pane, int32 mode) {
    paneOverrides[int(workspace)][pane] =
        mode >= 0 && mode < ViewModeCount ? mode : -1;
  }
  bool paneTree[2] = {false, false};
  SidebarMode askrSidebar[2] = {SidebarMode::FolderTree, SidebarMode::FolderTree};
  SidebarMode askrRestore[2] = {SidebarMode::FolderTree, SidebarMode::FolderTree};
  SidebarMode smidjaRestore[2] = {SidebarMode::FolderTree, SidebarMode::FolderTree};
  SidebarMode smidjaSidebar[2] = {SidebarMode::FolderTree,
                                 SidebarMode::FolderTree};
  bool preview = false;
  bool smidjaPreview = true;
  Workspace workspace = Workspace::tviskipt;
  bool SinglePane() const {
    return workspace == Workspace::einn || workspace == Workspace::askr;
  }
  bool IsEinn() const { return workspace == Workspace::einn; }
  bool IsSmidja() const { return workspace == Workspace::smidja; }
  bool HasConfigurableSidebar() const {
    return workspace == Workspace::askr || IsSmidja();
  }
  SidebarMode Sidebar(int pane) const {
    if (HasConfigurableSidebar())
      return IsSmidja() ? smidjaSidebar[pane] : askrSidebar[pane];
    return paneTree[pane] ? SidebarMode::FolderTree : SidebarMode::Hidden;
  }
  SidebarMode RestoreSidebar(int pane) const {
    return IsSmidja() ? smidjaRestore[pane] : askrRestore[pane];
  }
  void SetSidebar(int pane, SidebarMode mode) {
    SetSidebar(workspace, pane, mode);
  }
  void SetSidebar(Workspace target, int pane, SidebarMode mode) {
    if (target != Workspace::askr && target != Workspace::smidja) {
      paneTree[pane] = mode == SidebarMode::FolderTree;
      return;
    }
    const bool smidja = target == Workspace::smidja;
    auto &current = (smidja ? smidjaSidebar : askrSidebar)[pane];
    auto &restore = (smidja ? smidjaRestore : askrRestore)[pane];
    if (current != SidebarMode::Hidden)
      restore = current;
    current = mode;
    if (mode != SidebarMode::Hidden)
      restore = mode;
  }
  bool Preview() const { return IsSmidja() ? smidjaPreview : preview; }
  void SetPreview(bool value) {
    (IsSmidja() ? smidjaPreview : preview) = value;
  }
  int32 singlePane = 0;

  void Load(const BMessage &);
  void Archive(BMessage &) const;
};

struct WorkspaceProfile {
  std::string name;
  WorkspaceLayout layout;
};

class WorkspaceProfiles {
public:
  static constexpr std::size_t Limit = 32;
  const std::vector<WorkspaceProfile> &Items() const { return items; }
  const WorkspaceProfile *Find(const std::string &name) const;
  void Add(const std::string &name, const WorkspaceLayout &layout);
  void Update(const std::string &name, const WorkspaceLayout &layout);
  void Rename(const std::string &oldName, const std::string &newName);
  void Remove(const std::string &name);
  void Load(const BMessage &);
  void Archive(BMessage &) const;

private:
  WorkspaceProfile &Require(const std::string &name);
  std::vector<WorkspaceProfile> items;
};
