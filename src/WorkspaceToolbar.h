// SPDX-License-Identifier: MIT
#pragma once
#include <SupportDefs.h>
#include <vector>

class BGroupView;
class BStringView;
class BTextControl;
class BWindow;
class MainWindowMenu;
struct Preferences;

namespace Listi {

class FunctionButtonBank;

// The window layout owns both tool zones and their controls after attachment.
class WorkspaceToolbar {
public:
  WorkspaceToolbar(BWindow &window, MainWindowMenu &menu, Preferences &prefs);

  BGroupView *Top() const { return fTop; }
  BGroupView *Bottom() const { return fBottom; }
  const char *CommandText() const;

  void SetTransferTarget(int source, bool available, const char *destination);
  void Arrange();
  void ApplyVisibility(bool promptPending);
  void Resize(float width, bool promptPending);
  void Refresh(bool promptPending);
  bool Toggle(bool promptPending);

private:
  const std::vector<int32> &Actions() const;
  void ChooseDensity(float width);
  void SetBankVisible(bool visible);

  BWindow &fWindow;
  MainWindowMenu &fMenu;
  Preferences &fPrefs;
  BGroupView *fTop, *fBottom;
  FunctionButtonBank *fBank;
  BTextControl *fCommand;
  BStringView *fTransferTarget;
};

} // namespace Listi
