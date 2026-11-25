// SPDX-License-Identifier: MIT
#pragma once
#include "PaneNavigationState.h"
#include <View.h>
#include <optional>
class Pane;
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
class FolderTabs : public BView {
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
  struct Slot {
    size_t tab;
    BRect frame, close;
    std::string label;
  };
  struct Action {
    uint32 command = 0;
    int64 id = -1;
    bool operator==(const Action &other) const {
      return command == other.command && id == other.id;
    }
  };
  int32 side;
  Pane &pane;
  std::vector<FolderTab> tabs;
  std::optional<FolderTab> closed;
  size_t active = 0;
  bool pageVisible = true, pinned = false;
  std::vector<Slot> slots;
  BRect overflow;
  Action pressed;
  BPoint hover{-1, -1};
  void Draw(BRect) override;
  void FrameResized(float, float) override;
  void MouseDown(BPoint) override;
  void MouseUp(BPoint) override;
  void MouseMoved(BPoint, uint32, const BMessage *) override;
  bool GetToolTipAt(BPoint, BToolTip **) override;
  void Arrange();
  std::string LabelAt(size_t index) const;
  Action ActionAt(BPoint) const;
  void ShowOverflow();
  void Post(Action);
  bool Close(int64 id);
  void Rebuild();
  void SaveActive();
  void Insert(FolderTab tab);
  void Remove(size_t index);
  void Restore();
};
}
