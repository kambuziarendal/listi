// SPDX-License-Identifier: MIT
#include "Common.h"
#include "WorkspaceEditor.h"
#include <Button.h>
#include <Application.h>
#include <CardLayout.h>
#include <CardView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <TextControl.h>
#include <Window.h>
#include <cstring>
#include <iostream>

class Receiver : public BLooper {
public:
  sem_id saved = create_sem(0, "settings saved");
  sem_id layout = create_sem(0, "layout shortcut");
  BMessage result;
  ~Receiver() {
    delete_sem(layout);
    delete_sem(saved);
  }
  void MessageReceived(BMessage *message) override {
    if (message->what == kApplyPreferences) {
      result = *message;
      release_sem(saved);
    } else if (message->what == kOpenWorkspaceEditor && message->GetBool("modules", false)) {
      release_sem(layout);
    } else
      BLooper::MessageReceived(message);
  }
};

int main() {
  BApplication app("application/x-vnd.Listi-SettingsWindowTest");
  auto receiver = new Receiver;
  receiver->Run();
  Preferences prefs;
  int failures = 0;
  auto check = [&](bool ok, const char *label) {
    std::cout << (ok ? "PASS " : "FAIL ") << label << std::endl;
    failures += !ok;
  };
  for (bool save : {false, true}) {
    OpenPreferences(BMessenger(receiver), prefs);
    auto window = app.WindowAt(0);
    if (!window || !window->Lock())
      return 2;
    BMessenger dialog(window);
    auto category =
        dynamic_cast<BMenuField *>(window->FindView("settings category"));
    auto pages = dynamic_cast<BCardView *>(window->FindView("settings pages"));
    if (!category || !pages || category->Menu()->CountItems() != 4)
      return 2;
    auto select = [&](int32 index, const char *label) {
      auto *item = category->Menu()->ItemAt(index);
      if (std::strcmp(item->Label(), label) != 0 ||
          static_cast<BInvoker *>(item)->Invoke() != B_OK)
        return false;
      for (int i = 0; i < 100; ++i) {
        if (item->IsMarked() && pages->CardLayout()->VisibleIndex() == index) {
          window->Layout(true);
          return true;
        }
        window->Unlock();
        snooze(10000);
        window->Lock();
      }
      return false;
    };
    const auto inside = [](BRect screenBounds, BView *view) {
      return view && !view->IsHidden() &&
             screenBounds.Contains(view->ConvertToScreen(view->Bounds()));
    };
    const auto targetBounds = [](BScrollView *scroll) {
      return scroll->Target()->ConvertToScreen(scroll->Target()->Bounds());
    };
    const auto actionsVisible = [&] {
      const BRect bounds = window->ConvertToScreen(window->Bounds());
      return inside(bounds, category) &&
             inside(bounds, window->FindView("settings save")) &&
             inside(bounds, window->FindView("settings cancel"));
    };
    window->ResizeTo(408, 180);
    window->Layout(true);
    check(window->Bounds().Width() <= 408 && window->Bounds().Height() <= 180 &&
              actionsVisible(),
          "short, narrow settings keeps the chooser and actions visible");
    check(select(1, "Appearance"), "Appearance can be opened at narrow width");
    auto linkColor = window->FindView("settings link color");
    auto appearance = dynamic_cast<BScrollView *>(
        pages->CardLayout()->VisibleItem()->View());
    auto bar = appearance->ScrollBar(B_VERTICAL);
    float minimum, maximum;
    bar->GetRange(&minimum, &maximum);
    check(maximum > minimum && !inside(targetBounds(appearance), linkColor),
          "Appearance can be scrolled when its controls exceed the window");
    const float scrollToField =
        linkColor->ConvertToScreen(linkColor->Bounds()).bottom -
        targetBounds(appearance).bottom;
    bar->SetValue(scrollToField);
    check(inside(targetBounds(appearance), linkColor),
          "scrolling reveals the last Appearance field");
    bar->SetValue(maximum);
    check(inside(targetBounds(appearance), window->FindView("color format")),
          "scrolling to the bottom reveals the appearance format hint");
    check(select(2, "Workflow"), "Workflow can be opened at narrow width");
    auto *operations = dynamic_cast<BMenuField *>(
        window->FindView("operations presentation"));
    check(operations && operations->Menu()->CountItems() == 2 &&
              operations->Menu()->ItemAt(0)->IsMarked(),
          "Workflow defaults to integrated file operations");
    if (operations)
      operations->Menu()->ItemAt(1)->SetMarked(true);
    check(select(3, "Applications"),
          "Applications can be opened at narrow width");
    auto editor =
        dynamic_cast<BTextControl *>(window->FindView("settings editor"));
    auto terminal = window->FindView("settings terminal");
    auto applications = dynamic_cast<BScrollView *>(
        pages->CardLayout()->VisibleItem()->View());
    check(editor && inside(targetBounds(applications), editor) &&
              inside(targetBounds(applications), terminal),
          "both application fields fit the short, narrow settings window");
    if (editor)
      editor->SetText("Settings test editor");
    window->ResizeTo(408, 515);
    window->Layout(true);
    check(select(1, "Appearance"), "Appearance remains reachable after resize");
    bar->GetRange(&minimum, &maximum);
    check(maximum == minimum && inside(targetBounds(appearance), linkColor) &&
              actionsVisible(),
          "at 408 by 515 the appearance fields and actions are visible without scrolling");
    window->ResizeTo(560, 515);
    window->Layout(true);
    check(select(0, "Workspaces"), "Workspaces remains reachable");
    auto field = dynamic_cast<BMenuField *>(window->FindView("einn"));
    auto choice = field ? field->Menu()->ItemAt(1) : nullptr;
    check(field && field->Menu()->CountItems() == ViewModeCount &&
              field->Menu()->ItemAt(0)->IsMarked(),
          "Einn starts in Details and all file views remain available");
    auto customize = dynamic_cast<BButton *>(window->FindView("workspace layout"));
    check(customize && customize->Invoke() == B_OK &&
              acquire_sem_etc(receiver->layout, 1, B_RELATIVE_TIMEOUT, 1000000) == B_OK,
          "Workspaces shortcut requests layout modules without saving settings");
    if (choice) {
      choice->SetMarked(true);
    }
    window->Unlock();
    OpenPreferences(BMessenger(receiver), prefs);
    check(app.CountWindows() == 1, "reopening activates the existing dialog");
    dialog.SendMessage(save ? uint32(kApplyPreferences)
                            : uint32(B_QUIT_REQUESTED));
    check((acquire_sem_etc(receiver->saved, 1, B_RELATIVE_TIMEOUT,
                           save ? 3000000 : 250000) == B_OK) == save,
          save ? "Save submits settings" : "Cancel does not submit settings");
    for (int i = 0; i < 100 && dialog.IsValid(); ++i)
      snooze(20000);
    check(!dialog.IsValid(), "dialog closed");
  }
  receiver->Lock();
  prefs.ApplyDialog(receiver->result);
  receiver->Quit();
  check(prefs.DefaultMode(Workspace::einn) == 1 &&
            prefs.DefaultMode(Workspace::askr) == 0,
        "only chosen default changed");
  check(prefs.editor == "Settings test editor",
        "an application setting survives category changes and Save");
  check(prefs.operationsInWindow, "the separate operations option survives Settings Save");
  return failures ? 1 : 0;
}
