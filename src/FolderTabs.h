// SPDX-License-Identifier: MIT
#pragma once
#include "PaneNavigationState.h"
#include <GroupView.h>
#include <optional>
class Pane;
class BButton;
namespace Listi {
enum : uint32 {
  kTabNew = 'tbne', kTabDuplicate = 'tbdu', kTabClose = 'tbcl', kTabReopen = 'tbre',
  kTabRename = 'tbrn', kTabNamed = 'tbna', kTabLeft = 'tbml', kTabRight = 'tbmr',
  kTabPrevious = 'tbpr', kTabNext = 'tbnx', kTabChoose = 'tbch',
  kTabDetach = 'tbdt', kTabAlways = 'tbal'
};
struct FolderTab {
  int64 id;
  std::string alias;
  PaneNavigationState state;
};
// Only the active file view owns readers and watchers. The remaining tabs
// are suspended navigation values, not hidden Pane instances.
class FolderTabs : public BGroupView {
public:
  FolderTabs(int32 side, Pane &);
  bool Change(uint32 command, int64 chosen = -1);
  void OpenDirectory(const char *path);
  FolderTab Snapshot();
  BMessage ArchiveTabs();
  void RestoreTabs(const BMessage &);
  void Adopt(FolderTab tab);
  void AfterDetach();
  void Rename(const std::string &);
  void SetShown(bool filesPage, bool always);
  void UpdateLabel();
  size_t Count() const { return tabs.size(); }
  size_t Index() const { return active; }
  int64 ActiveId() const { return tabs[active].id; }
  bool CanReopen() const { return closed.has_value(); }
private:
  int32 side;
  Pane &pane;
  std::vector<FolderTab> tabs;
  std::optional<FolderTab> closed;
  size_t active = 0;
  bool pageVisible = true, pinned = false;
  struct ButtonSlot { BButton *button; size_t tab; };
  std::vector<ButtonSlot> slots;
  void Rebuild();
  void Insert(FolderTab tab);
  void Remove();
  void Restore();
};
}
