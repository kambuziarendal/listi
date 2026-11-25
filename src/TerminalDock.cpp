// SPDX-License-Identifier: MIT
#include "TerminalDock.h"
#include "WindowOverview.h"
#include "Common.h"
#include "MainWindow.h"
#include "VirtualFile.h"

#include <GroupLayout.h>
#include <Screen.h>
#include <SplitView.h>
#include <Window.h>
#include <algorithm>
#include <cstring>

namespace Listi {
namespace {

constexpr uint32 kFocusFloating = 'tlfc';

class FloatingTerminal : public WorkingWindow {
public:
  FloatingTerminal(BRect frame, BMessenger receiver, int lister,
                   int64 placement)
      : WorkingWindow(frame, TerminalName(lister), "CLI",
                B_AUTO_UPDATE_SIZE_LIMITS),
        owner(receiver), placement(placement) {
    SetLayout(new BGroupLayout(B_VERTICAL, 0));
    AddShortcut('W', B_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
  }

  TerminalPanel *Panel() const {
    return static_cast<TerminalPanel *>(ChildAt(0));
  }

  void DispatchMessage(BMessage *message, BHandler *target) override {
    if (message->what == B_QUIT_REQUESTED && target == this) {
      // The title-bar close button and Command+W dock the session. Haiku's
      // application shutdown instead calls QuitRequested/Quit directly.
      BMessage dock(kDockTerminal);
      dock.AddInt64("terminal placement", placement);
      owner.SendMessage(&dock, static_cast<BHandler *>(nullptr), 0);
      return;
    }
    BWindow::DispatchMessage(message, target);
  }

  void MessageReceived(BMessage *message) override {
    if (HandleWindowAction(message))
      return;
    if (message->what == kFocusFloating) {
      if (IsMinimized())
        Minimize(false);
      Activate();
      if (auto *panel = Panel())
        panel->Focus();
      return;
    }
    BWindow::MessageReceived(message);
  }

private:
  const BMessenger owner;
  const int64 placement;
};

BRect FloatingFrame(BWindow *owner, const TerminalPanel &panel) {
  const BRect workingArea = BScreen(owner).Frame().InsetByCopy(20, 40);
  const BRect previous = owner->Frame();
  if (!workingArea.IsValid())
    return previous.OffsetByCopy(24, 24);
  const float width =
      std::min(workingArea.Width(), std::max(640.0f, panel.Bounds().Width()));
  const float height =
      std::min(workingArea.Height(), std::max(330.0f, panel.Bounds().Height()));
  const float left = std::clamp(previous.left + 35, workingArea.left,
                                workingArea.right - width);
  const float top = std::clamp(previous.top + 45, workingArea.top,
                               workingArea.bottom - height);
  return BRect(left, top, left + width, top + height);
}

} // namespace

TerminalDock::TerminalDock(BSplitView *split, BView *workspace,
                           BMessenger owner)
    : split(split), owner(owner), band(new BGroupView(B_HORIZONTAL, 6)) {
  split->AddChild(workspace, 0.68f);
  split->SetCollapsible(0, false);
  split->AddChild(band, 0.32f);
  for (int lister = 0; lister < 2; ++lister) {
    auto &session = sessions[lister];
    session.panel = new TerminalPanel(owner, lister);
    session.panel->SetPlacement(false, session.placement = ++generation);
    band->GroupLayout()->AddView(session.panel);
    session.panel->Hide();
  }
  UpdateVisibility();
}

TerminalDock::~TerminalDock() {
  for (auto &session : sessions) {
    if (session.detached.LockTarget()) {
      BLooper *looper;
      session.detached.Target(&looper);
      // Quit directly: requesting a normal window close would redock instead.
      looper->Quit();
    }
  }
}

std::optional<TerminalRestartPrompt>
TerminalDock::Open(const BMessage &message, int lister, const char *directory) {
  auto *terminalPanel = Docked(lister);
  if (!terminalPanel) {
    FocusFloating(lister);
    return std::nullopt;
  }
  const bool floating = message.what == kShowFloatingTerminal;
  const bool hasSession = terminalPanel->Active();
  const bool here = message.what == kTerminal;
  const char *path = message.GetString("folder", directory);
  if (message.HasString("folder") &&
      (message.GetInt64("session", -1) != terminalPanel->SessionId() ||
       message.GetInt64("terminal placement", -1) !=
           terminalPanel->Placement()))
    return std::nullopt;
  if (!hasSession || here) {
    if (!strcmp(path, "@disks") || IsVirtual(path)) {
      PostMainWindowNotice("Open a local folder first.");
      return std::nullopt;
    }
    const bool restart = hasSession && terminalPanel->StartFolder() != path;
    if (restart && !message.HasString("folder")) {
      BMessage accepted(message.what);
      accepted.AddString("folder", path);
      accepted.AddInt32("terminal lister", lister);
      accepted.AddInt64("session", terminalPanel->SessionId());
      accepted.AddInt64("terminal placement", terminalPanel->Placement());
      BString prompt("Close the current CLI and start a new shell in ");
      prompt << path << "? Any running command will stop.";
      return TerminalRestartPrompt{prompt, accepted};
    }
    if (!hasSession || restart) {
      if (const int error = terminalPanel->Start(path)) {
        BString detail("Could not open Listi CLI: ");
        detail << strerror(error);
        PostMainWindowNotice(detail);
        return std::nullopt;
      }
    }
  }
  Show(lister);
  if (!floating || !Float(terminalPanel->Placement()))
    terminalPanel->Focus();
  return std::nullopt;
}

bool TerminalDock::ApplyPlacement(const BMessage &message) {
  switch (message.what) {
  case kTerminalBelow:
    SetSide(TerminalSide::Below);
    break;
  case kTerminalBeside:
    SetSide(TerminalSide::Right);
    break;
  case kTerminalAbove:
    SetSide(TerminalSide::Above);
    break;
  case kTerminalLeft:
    SetSide(TerminalSide::Left);
    break;
  case kToggleTerminalSide:
    Move(message.GetInt64("terminal placement", -1));
    break;
  case kFloatTerminal:
    Float(message.GetInt64("terminal placement", -1));
    break;
  case kDockTerminal:
    Dock(message.GetInt64("terminal placement", -1));
    break;
  default:
    return false;
  }
  return true;
}

TerminalPanel *TerminalDock::Docked(int lister) const {
  return sessions[lister].panel;
}

bool TerminalDock::HasFocus() const {
  for (const auto &session : sessions)
    if (session.visible && session.panel->HasFocus())
      return true;
  return false;
}

int TerminalDock::FindSession(int64 requested) const {
  for (int lister = 0; lister < 2; ++lister)
    if (sessions[lister].placement == requested)
      return lister;
  return -1;
}

void TerminalDock::UpdateVisibility() {
  split->SetItemCollapsed(split->GetLayout()->IndexOfView(band),
                          !sessions[0].visible && !sessions[1].visible);
}

void TerminalDock::Show(int lister) {
  auto &session = sessions[lister];
  if (!session.visible) {
    session.panel->Show();
    session.visible = true;
  }
  UpdateVisibility();
}

void TerminalDock::SetSide(TerminalSide requested) {
  if (side == requested)
    return;
  TerminalPanel *focused = nullptr;
  for (auto &session : sessions)
    if (session.panel && session.panel->HasFocus())
      focused = session.panel;
  const auto leading = [](TerminalSide position) {
    return position == TerminalSide::Above || position == TerminalSide::Left;
  };
  if (leading(side) != leading(requested)) {
    const int32 oldIndex = split->GetLayout()->IndexOfView(band);
    const float weight = split->ItemWeight(oldIndex);
    split->GetLayout()->RemoveView(band);
    split->Window()->Sync();
    split->AddChild(leading(requested) ? 0 : 1, band, weight);
  }
  side = requested;
  const bool lateral = IsSideDock(side);
  split->SetOrientation(lateral ? B_HORIZONTAL : B_VERTICAL);
  band->GroupLayout()->SetOrientation(lateral ? B_VERTICAL : B_HORIZONTAL);
  for (auto &session : sessions)
    if (session.panel)
      session.panel->SetDockSide(side);
  UpdateVisibility();
  if (focused)
    focused->Focus();
}

bool TerminalDock::Move(int64 requested) {
  const int lister = FindSession(requested);
  if (lister < 0 || !sessions[lister].visible)
    return false;
  switch (side) {
  case TerminalSide::Below:
    SetSide(TerminalSide::Right);
    break;
  case TerminalSide::Right:
    SetSide(TerminalSide::Above);
    break;
  case TerminalSide::Above:
    SetSide(TerminalSide::Left);
    break;
  case TerminalSide::Left:
    SetSide(TerminalSide::Below);
    break;
  }
  sessions[lister].panel->Focus();
  return true;
}

bool TerminalDock::FocusFloating(int lister) {
  auto &detached = sessions[lister].detached;
  if (!detached.IsValid())
    return false;
  BMessage focus(kFocusFloating);
  detached.SendMessage(&focus, static_cast<BHandler *>(nullptr), 0);
  return true;
}

bool TerminalDock::Float(int64 requested) {
  const int lister = FindSession(requested);
  if (lister < 0)
    return false;
  auto &session = sessions[lister];
  auto *panel = session.panel;
  if (!panel || !session.visible)
    return false;
  auto *window = new FloatingTerminal(FloatingFrame(split->Window(), *panel),
                                      owner, lister, generation + 1);
  // Remove through the layout; BView::RemoveSelf can report failure after
  // removal of the last layout item has already detached the view.
  if (!band->GroupLayout()->RemoveView(panel)) {
    window->Quit();
    return false;
  }
  // app_server tracks view tokens across the application, not per window.
  // Finish removing the old server views before creating their counterparts.
  split->Window()->Sync();
  if (!window->GetLayout()->AddView(panel)) {
    const int32 index = lister == 0 || !sessions[0].panel ? 0 : 1;
    band->GroupLayout()->AddView(index, panel);
    window->Quit();
    return false;
  }
  panel->SetPlacement(true, session.placement = ++generation);
  panel->Focus();
  session.detached = BMessenger(window);
  session.panel = nullptr;
  session.visible = false;
  UpdateVisibility();
  window->Describe(TerminalName(lister), std::string("Started in: ") + panel->StartFolder());
  window->Show();
  return true;
}

bool TerminalDock::ReturnPanel(int lister) {
  auto &session = sessions[lister];
  if (!session.detached.LockTarget())
    return false;
  BLooper *looper;
  session.detached.Target(&looper);
  auto *window = static_cast<FloatingTerminal *>(looper);
  auto *returning = window->Panel();
  if (!returning || !window->GetLayout()->RemoveView(returning)) {
    window->Unlock();
    return false;
  }
  window->Sync();
  const int32 index = lister == 0 || !sessions[0].panel ? 0 : 1;
  if (!band->GroupLayout()->AddView(index, returning)) {
    window->GetLayout()->AddView(returning);
    window->Unlock();
    return false;
  }
  session.panel = returning;
  session.visible = true;
  returning->SetDockSide(side);
  returning->SetPlacement(false, session.placement = ++generation);
  session.detached = {};
  window->Quit();
  return true;
}

bool TerminalDock::Dock(int64 requested) {
  const int lister = FindSession(requested);
  if (lister < 0 || !ReturnPanel(lister))
    return false;
  UpdateVisibility();
  split->Window()->Activate();
  sessions[lister].panel->Focus();
  return true;
}

bool TerminalDock::Close(int64 requested) {
  const int lister = FindSession(requested);
  if (lister < 0)
    return false;
  auto &session = sessions[lister];
  if (!session.visible && !ReturnPanel(lister))
    return false;
  session.panel->Stop();
  session.panel->SetPlacement(false, session.placement = ++generation);
  session.panel->Hide();
  session.visible = false;
  UpdateVisibility();
  return true;
}

bool TerminalDock::CloseSessions() {
  for (auto &session : sessions) {
    if ((session.visible || session.detached.IsValid()) && !Close(session.placement))
      return false;
  }
  return true;
}

} // namespace Listi
