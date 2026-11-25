// SPDX-License-Identifier: MIT
#include "FunctionButtonBank.h"

#include "MainWindowMenu.h"
#include "WorkspaceActions.h"
#include <Button.h>
#include <GridLayout.h>
#include <GridView.h>
#include <GroupLayout.h>
#include "VirtualFile.h"
#include <utility>

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
  LabelTransfers();
}

void FunctionButtonBank::SetDestination(std::string arrow, std::string folder) {
  if (arrow != fArrow || folder != fFolder) {
    fArrow = std::move(arrow);
    fFolder = std::move(folder);
    LabelTransfers();
  }
}

void FunctionButtonBank::LabelTransfers() {
  if (!fGrid)
    return;
  for (int32 i = 0; i < fGrid->CountChildren(); ++i) {
    auto *button = dynamic_cast<BButton *>(fGrid->ChildAt(i));
    const auto what = button->Message()->what;
    if (what != kCopy && what != kMove)
      continue;
    BString label(what == kCopy ? "Copy" : "Move");
    BString hint(label);
    if (!fArrow.empty()) {
      BString leaf(LocationLeaf(fFolder).c_str());
      if (leaf.IsEmpty()) leaf = fFolder.empty() ? "?" : "/";
      if (leaf.CountChars() > 12) {
        leaf.TruncateChars(11);
        leaf << "…";
      }
      label << ' ' << fArrow.c_str() << ' ' << leaf;
      hint << " to " << (fFolder.empty() ? "other panel (no folder)" : fFolder.c_str());
    }
    const char *shortcut = what == kCopy ? "Shift+C" : "M";
    if (!fCompact && fArrow.empty())
      label << " [" << shortcut << "]";
    hint << "\nCommand+" << shortcut << " (normally Alt)";
    button->SetLabel(label);
    button->SetToolTip(hint.String());
  }
}

}
