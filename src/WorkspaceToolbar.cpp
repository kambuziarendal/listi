// SPDX-License-Identifier: MIT
#include "WorkspaceToolbar.h"

#include "Common.h"
#include "ExplicitSubmit.h"
#include "FunctionButtonBank.h"
#include "MainWindowMenu.h"
#include "ViewVisibility.h"
#include "WorkspaceActions.h"
#include <GroupLayout.h>
#include <GroupView.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>

namespace Listi {

WorkspaceToolbar::WorkspaceToolbar(BWindow &window, MainWindowMenu &menu,
                                   Preferences &prefs)
    : fWindow(window), fMenu(menu), fPrefs(prefs),
      fTop(new BGroupView(B_VERTICAL, 5)),
      fBottom(new BGroupView(B_VERTICAL, 5)),
      fBank(new FunctionButtonBank(menu)),
      fCommand(new BTextControl("Command:", "", nullptr)),
      fTransferTarget(new BStringView("panel transfer target", "")) {
  fTop->SetName("top workspace tools");
  fTop->GroupLayout()->SetInsets(8, 4, 8, 4);
  fBottom->SetName("bottom workspace tools");
  fBottom->GroupLayout()->SetInsets(8, 4, 8, 8);
  fCommand->TextView()->AddFilter(
      new ExplicitSubmit(BMessenger(&window), BMessage(kCommand)));
  fCommand->SetToolTip(
      "Runs a shell command in the active directory using Terminal.");
  fTransferTarget->SetTruncation(B_TRUNCATE_MIDDLE);
  fTransferTarget->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
  fTransferTarget->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
  fTransferTarget->Hide();
  fTop->AddChild(fTransferTarget);
  fBottom->AddChild(fBank);
  fBottom->AddChild(fCommand);
}

const char *WorkspaceToolbar::CommandText() const { return fCommand->Text(); }

void WorkspaceToolbar::SetTransferTarget(int source, bool available, const char *destination) {
  BString label;
  if (available) {
    label << "Copy / Move: " << (source == 0 ? "Left → Right" : "Right → Left")
          << "   " << (destination ? destination : "Open a destination folder");
  }
  if (label != fTransferTarget->Text()) {
    fTransferTarget->SetText(label);
    fTransferTarget->SetToolTip(label.IsEmpty() ? nullptr : label.String());
  }
  SetViewVisible(fTransferTarget, available);
  SetBankVisible(!fBank->IsHidden(fBank));
  fMenu.SetTransferDirection(source, available);
}

void WorkspaceToolbar::Arrange() {
  auto *bankZone = fPrefs.bankTop ? fTop : fBottom;
  if (fBank->Parent() != bankZone) {
    (fPrefs.bankTop ? fBottom : fTop)->GroupLayout()->RemoveView(fBank);
    bankZone->GroupLayout()->AddView(int32(0), fBank);
  }
  auto *commandZone = fPrefs.commandTop ? fTop : fBottom;
  if (fCommand->Parent() != commandZone) {
    (fPrefs.commandTop ? fBottom : fTop)->GroupLayout()->RemoveView(fCommand);
    commandZone->AddChild(fCommand);
  }
  ChooseDensity(fWindow.Bounds().Width());
}

void WorkspaceToolbar::ApplyVisibility(bool promptPending) {
  SetViewVisible(fCommand, fPrefs.command && !fPrefs.SinglePane());
  Refresh(promptPending);
}

void WorkspaceToolbar::Resize(float width, bool promptPending) {
  ChooseDensity(width);
  Refresh(promptPending);
}

const std::vector<int32> &WorkspaceToolbar::Actions() const {
  return fPrefs.fullActionButtons ? fPrefs.actionButtons : kCompactWorkspaceActions;
}

void WorkspaceToolbar::ChooseDensity(float width) {
  const bool narrow = fPrefs.fullActionButtons && fPrefs.adaptive && width < 850;
  fBank->SetActions(Actions(), !fPrefs.fullActionButtons || narrow, narrow);
}

void WorkspaceToolbar::SetBankVisible(bool visible) {
  SetViewVisible(fBank, visible);
  for (auto *zone : {fTop, fBottom}) {
    const bool hasBank = fBank->Parent() == zone && !fBank->IsHidden(fBank);
    const bool hasCommand =
        fCommand->Parent() == zone && !fCommand->IsHidden(fCommand);
    const bool hasTransfer =
        fTransferTarget->Parent() == zone && !fTransferTarget->IsHidden(fTransferTarget);
    SetViewVisible(zone, hasBank || hasCommand || hasTransfer);
  }
}

void WorkspaceToolbar::Refresh(bool promptPending) {
  const bool available = !fPrefs.SinglePane() && !Actions().empty();
  const bool requested = fPrefs.ActionButtonsVisible();
  const bool visible = available && requested && !promptPending;
  bool overflow = false;
  SetBankVisible(visible);
  if (visible) {
    // Showing a layout item invalidates layout but does not run it inline.
    // Measure before the window's auto-size pass can impose a larger frame.
    auto minimum = fWindow.GetLayout()->MinSize();
    const auto bounds = fWindow.Bounds();
    if (minimum.width > bounds.Width()) {
      fBank->SetActions(Actions(), true, true);
      minimum = fWindow.GetLayout()->MinSize();
    }
    overflow =
        minimum.width > bounds.Width() || minimum.height > bounds.Height();
    if (overflow)
      SetBankVisible(false);
  }
  fMenu.SetFunctionButtonState(available, requested, overflow,
                             !fPrefs.SinglePane() && !fPrefs.actionButtons.empty(),
                             fPrefs.fullActionButtons);
}

bool WorkspaceToolbar::Toggle(bool promptPending) {
  if (fPrefs.SinglePane() || Actions().empty())
    return false;
  auto &choice =
      fPrefs.IsSmidja() ? fPrefs.smidjaButtons : fPrefs.tviskiptButtons;
  choice = !fPrefs.ActionButtonsVisible();
  Refresh(promptPending);
  return true;
}

} // namespace Listi
