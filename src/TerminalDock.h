// SPDX-License-Identifier: MIT
#pragma once

#include "TerminalPanel.h"
#include <Message.h>
#include <String.h>
#include <optional>

class BSplitView;
class BView;

namespace Listi {

struct TerminalRestartPrompt {
  BString text;
  BMessage accepted;
};

// Called only with the file window locked. The detached window never locks
// the file window; its controls send requests back to this owner.
class TerminalDock {
public:
  TerminalDock(BSplitView *split, BView *workspace, BView &leftLister,
               BView &rightLister, BMessenger owner);
  ~TerminalDock();

  // Opening in another directory can require a confirmed restart. The
  // returned prompt captures both the shell and its current placement.
  std::optional<TerminalRestartPrompt> Open(const BMessage &request, int lister,
                                            const char *directory, int64 tabId,
                                            const std::string &tabLabel);
  bool ApplyPlacement(const BMessage &request);
  TerminalPanel *Docked(int lister) const;
  bool HasSessions() const { return sessions[0].visible || sessions[1].visible ||
      sessions[0].detached.IsValid() || sessions[1].detached.IsValid(); }
  int64 OpenPlacement(int lister) const;
  int64 OpenPlacement(int lister, int64 tabId) const;
  int64 OwnerTab(int lister) const { return sessions[lister].tabId; }
  void UpdateTabLabel(int lister, const std::string &label);
  bool TransferTo(int64 placement, TerminalDock &destination, int targetLister = 0);
  bool HasFocus() const;
  bool FocusFloating(int lister);
  bool Float(int64 placement);
  bool Dock(int64 placement);
  bool Close(int64 placement);
  bool CloseSessions();
  void Show(int lister);
  void SetSide(TerminalSide side);
  bool Move(int64 placement);

private:
  struct Session {
    TerminalPanel *panel = nullptr;
    BMessenger detached;
    int64 placement = 0;
    bool visible = false;
    int64 tabId = -1;
    std::string lastTabLabel;
    bool closedTab = false;
    std::string DisplayLabel() const {
      return closedTab ? lastTabLabel + " (closed)" : lastTabLabel;
    }
  };

  int FindSession(int64 placement) const;
  bool ReturnPanel(int lister);
  void UpdateVisibility();

  BSplitView *split;
  BView *listers[2];
  BMessenger owner;
  BGroupView *band;
  Session sessions[2];
  TerminalSide side = TerminalSide::Below;
  int64 generation = 0;
};

} // namespace Listi
