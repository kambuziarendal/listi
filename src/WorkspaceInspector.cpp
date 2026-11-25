// SPDX-License-Identifier: MIT
#include "WorkspaceSplit.h"
#include "Common.h"

#include "ImagePreview.h"
#include "InspectorEditor.h"
#include "PanelTools.h"
#include "ViewVisibility.h"
#include "WindowOverview.h"
#include <Application.h>
#include <GroupLayout.h>
#include <GroupView.h>

namespace Listi {
namespace {
constexpr uint32 kInspectFile = 'isfl', kRaiseInspector = 'isra';

class FloatingInspector : public WorkingWindow {
public:
  explicit FloatingInspector(BWindow *browser)
      : WorkingWindow(BRect(100, 85, 900, 665), "Listi: Inspector", "Inspector",
                      B_AUTO_UPDATE_SIZE_LIMITS), owner(browser) {
    SetLayout(new BGroupLayout(B_VERTICAL, 0));
    CenterIn(browser->Frame());
    MoveOnScreen();
  }

  void DispatchMessage(BMessage *message, BHandler *target) override {
    if ((message->what == B_QUIT_REQUESTED || message->what == kDockInspector) &&
        target == this) {
      RequestDock();
      return;
    }
    if (message->what == B_KEY_DOWN) {
      const auto modifiers = message->GetInt32("modifiers", 0);
      const auto key = message->GetInt32("raw_char", 0);
      if ((modifiers & (B_COMMAND_KEY | B_CONTROL_KEY | B_OPTION_KEY | B_SHIFT_KEY)) ==
          B_COMMAND_KEY) {
        if (key == 'w' || key == 'W') {
          RequestDock();
          return;
        }
        if (key == 'q' || key == 'Q') {
          be_app->PostMessage(B_QUIT_REQUESTED);
          return;
        }
      }
      if (HandleInspectorEditorKey(CurrentFocus(), *message))
        return;
    }
    WorkingWindow::DispatchMessage(message, target);
  }

  void MessageReceived(BMessage *message) override {
    switch (message->what) {
    case kInspectFile:
      Preview().ShowFile(message->GetString("file", ""));
      break;
    case kRaiseInspector:
      if (IsMinimized())
        Minimize(false);
      Activate();
      if (const char *path = message->GetString("edit", nullptr))
        Preview().BeginEditing(path);
      break;
    default:
      WorkingWindow::MessageReceived(message);
    }
  }

private:
  const BMessenger owner;
  ImagePreview &Preview() const {
    return *static_cast<ImagePreview *>(ChildAt(0));
  }
  void RequestDock() {
    BMessage dock(kDockInspector);
    dock.AddMessenger("inspector", BMessenger(this));
    // The browser owns placement. Never wait for its lock from this window.
    owner.SendMessage(&dock, static_cast<BHandler *>(nullptr), 0);
  }
};

BWindow *MessengerWindow(const BMessenger &target) {
  BLooper *looper = nullptr;
  target.Target(&looper);
  return static_cast<BWindow *>(looper);
}
}

WorkspaceSplit::~WorkspaceSplit() {
  if (floatingInspector && floatingInspector->LockTarget())
    MessengerWindow(*floatingInspector)->Quit();
}

bool WorkspaceSplit::DetachInspector() {
  if (floatingInspector) {
    OpenFloatingInspector({});
    return false;
  }
  auto *host = new FloatingInspector(Window());
  if (!inspectorSlot->GroupLayout()->RemoveView(inspector)) {
    host->Quit();
    return false;
  }
  // Finish deleting the browser's native view tokens before remounting.
  Window()->Sync();
  if (!host->GetLayout()->AddView(inspector)) {
    inspectorSlot->GroupLayout()->AddView(inspector);
    host->Quit();
    return false;
  }
  floatingInspector.emplace(host);
  quickView = false;
  SetInspectorVisible(false);
  SetViewVisible(inspector, true);
  inspector->SetQuickViewCloseVisible(false);
  inspector->SetDockControl(true);
  floatingSelection = Panel(source).SelectedFile();
  inspector->ShowFile(floatingSelection);
  host->Show();
  return true;
}

bool WorkspaceSplit::DockInspector(const BMessage &message) {
  if (!floatingInspector)
    return false;
  BMessenger origin;
  if (message.FindMessenger("inspector", &origin) == B_OK && origin != *floatingInspector)
    return false;
  if (floatingInspector->LockTargetWithTimeout(500000) != B_OK)
    return false;
  auto *host = MessengerWindow(*floatingInspector);
  if (!host->GetLayout()->RemoveView(inspector)) {
    host->Unlock();
    return false;
  }
  host->Sync();
  if (!inspectorSlot->GroupLayout()->AddView(inspector)) {
    host->GetLayout()->AddView(inspector);
    host->Unlock();
    return false;
  }
  floatingInspector.reset();
  inspector->SetDockControl(false);
  quickView = prefs.SinglePane() || !prefs.Preview() || HasPanelPreview();
  host->Quit();
  Window()->Activate();
  return true;
}

void WorkspaceSplit::OpenFloatingInspector(const std::string &editPath) {
  if (!floatingInspector)
    return;
  BMessage raise(kRaiseInspector);
  if (!editPath.empty())
    raise.AddString("edit", editPath.c_str());
  floatingInspector->SendMessage(&raise, static_cast<BHandler *>(nullptr), 0);
}

bool WorkspaceSplit::FloatingInspectorAllowsQuit() {
  if (!floatingInspector || !floatingInspector->IsValid())
    return true;
  if (floatingInspector->LockTargetWithTimeout(500000) != B_OK)
    return false;
  auto *host = MessengerWindow(*floatingInspector);
  const bool allowed = InspectorEditorsAllowQuit(host);
  host->Unlock();
  return allowed;
}

void WorkspaceSplit::UpdatePreviews() {
  Panel(0).UpdatePreview(Panel(1));
  Panel(1).UpdatePreview(Panel(0));
  if (!floatingInspector) {
    Inspector().ShowFile(InspectorVisible() ? Panel(source).SelectedFile() : "");
    return;
  }
  auto file = Panel(source).SelectedFile();
  if (floatingSelection == file)
    return;
  BMessage inspect(kInspectFile);
  inspect.AddString("file", file.c_str());
  if (floatingInspector->SendMessage(&inspect, static_cast<BHandler *>(nullptr), 0) == B_OK)
    floatingSelection = std::move(file);
}
}
