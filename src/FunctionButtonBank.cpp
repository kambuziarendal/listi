// SPDX-License-Identifier: MIT
#include "FunctionButtonBank.h"

#include "MainWindowMenu.h"
#include "WorkspaceActions.h"
#include <Button.h>
#include <GridLayout.h>
#include <GridView.h>
#include <GroupLayout.h>

namespace Listi {

FunctionButtonBank::FunctionButtonBank(MainWindowMenu &menu)
    : BGroupView(B_VERTICAL, 5), fMenu(menu) {
  SetName("workspace action buttons");
  SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
}

void FunctionButtonBank::SetActions(const std::vector<int32> &actions,
                                    bool compact, bool wrap) {
  if (fGrid && fCompact == compact && fWrap == wrap && fActions == actions)
    return;
  fCompact = compact;
  fWrap = wrap;
  fActions = actions;
  fMenu.ClearFunctionActions();
  if (fGrid) {
    GroupLayout()->RemoveView(fGrid);
    delete fGrid;
  }

  const int columns = wrap ? 3 : (compact ? 5 : 6);
  fGrid = new BGridView(compact ? 3 : 5, 5);
  int index = 0;
  for (auto id : fActions) {
    if (id < 0 || id >= WorkspaceLayout::ActionButtonCount)
      continue;
    const auto &action = kWorkspaceActions[id];
    fMenu.AddFunctionAction(action);
    BString label(action.label);
    if (!compact && action.message != kNone && *action.shortcut)
      label << " [" << action.shortcut << "]";
    auto button = new BButton(label, new BMessage(action.message));
    BString hint("Command+");
    hint << action.shortcut
         << " (normally Alt). File shortcuts apply outside text fields.";
    button->SetToolTip(*action.shortcut ? hint.String() : action.description);
    button->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    fGrid->GridLayout()->AddView(button, index % columns, index / columns);
    ++index;
  }
  GroupLayout()->AddView(int32(0), fGrid);
}

}
