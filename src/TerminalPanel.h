// SPDX-License-Identifier: MIT
#pragma once

#include <GroupView.h>
#include <Messenger.h>
#include <string>

class BStringView;
class BButton;

namespace Listi {

inline constexpr uint32 kTerminalBelow = 'tlbl';
inline constexpr uint32 kTerminalBeside = 'tlbs';
inline constexpr uint32 kTerminalAbove = 'tlab';
inline constexpr uint32 kTerminalLeft = 'tlle';
inline constexpr uint32 kLeftCLI = 'tlc0';
inline constexpr uint32 kRightCLI = 'tlc1';
inline constexpr uint32 kToggleTerminalSide = 'tlts';
inline constexpr uint32 kTerminalWindow = 'tlwn';
inline constexpr uint32 kShowFloatingTerminal = 'tlof';
inline constexpr uint32 kCloseTerminal = 'tlcl';
inline constexpr uint32 kFloatTerminal = 'tlfl';
inline constexpr uint32 kDockTerminal = 'tldk';

enum class TerminalSide { Below, Right, Above, Left };

inline const char *TerminalName(int lister) {
  return lister == 0 ? "Left CLI" : "Right CLI";
}

inline bool IsSideDock(TerminalSide side) {
  return side == TerminalSide::Right || side == TerminalSide::Left;
}

class TerminalView;

class TerminalPanel : public BGroupView {
public:
  explicit TerminalPanel(BMessenger owner, int lister);
  const std::string &StartFolder() const { return startFolder; }
  int64 Placement() const { return placement; }
  int64 SessionId() const { return sessionId; }
  bool Active() const;
  bool HasFocus() const;
  void Focus();
  int Start(const char *directory);
  void Stop();
  bool HandleKey(BMessage *message);
  void SetOwner(BMessenger owner, int lister);
  void SetTabLabel(const std::string &label);
  std::string DisplayTitle() const;
  void SetPlacement(bool floating, int64 generation);
  void SetDockSide(TerminalSide side);

private:
  void RefreshLocation();
  std::string startFolder, tabLabel;
  int64 sessionId = 0;
  int64 placement = 0;
  bool floating = false;
  TerminalSide dockSide = TerminalSide::Below;
  BGroupLayout *header;
  BStringView *location;
  TerminalView *terminal;
  BButton *position, *move, *close;
};

} // namespace Listi
