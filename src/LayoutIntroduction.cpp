// SPDX-License-Identifier: MIT
#include "LayoutIntroduction.h"
#include "WrappedLabel.h"
#include <Button.h>
#include <LayoutBuilder.h>
#include <RadioButton.h>
#include <Window.h>

namespace Listi {
namespace {
constexpr uint32 kRaiseGuide = 'wgup', kPick = 'wgpk', kUse = 'wgus', kSkip = 'wgsk';
struct Choice { Workspace layout; const char *description; };
constexpr Choice choices[] = {
    {Workspace::einn, "Browse one folder at a time in a single file panel."},
    {Workspace::askr, "Choose a folder in the tree and browse its contents beside it."},
    {Workspace::tviskipt, "Keep source and destination folders side by side for copying and moving."},
    {Workspace::smidja, "Use two file panels with room for Search and Preview in the workbench."}
};

class Introduction : public BWindow {
  BMessenger owner;
  Workspace chosen;
  WrappedLabel *sample;
  bool firstRun;
public:
  Introduction(BWindow &window, Workspace current, bool fresh)
      : BWindow(BRect(0, 0, 450, 310), "Choose a Listi layout", B_TITLED_WINDOW,
                B_AUTO_UPDATE_SIZE_LIMITS | B_NOT_ZOOMABLE),
        owner(&window), chosen(current), firstRun(fresh) {
    auto *options = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
    sample = new WrappedLabel("layout description", "");
    for (const auto &choice : choices) {
      auto *pick = new BMessage(kPick);
      pick->AddInt32("workspace", int32(choice.layout));
      auto *radio = new BRadioButton(WorkspaceName(choice.layout), WorkspaceName(choice.layout), pick);
      options->GroupLayout()->AddView(radio);
      if (current == choice.layout) {
        radio->SetValue(B_CONTROL_ON);
        sample->SetMessage(choice.description);
      }
    }
    auto *apply = new BButton("apply layout", "Use this layout", new BMessage(kUse));
    BLayoutBuilder::Group<>(this, B_VERTICAL)
        .SetInsets(B_USE_WINDOW_SPACING)
        .Add(new WrappedLabel("layout help", "Choose how to browse files. You can switch later in View > Workspace."))
        .Add(options).Add(sample).AddGlue()
        .AddGroup(B_HORIZONTAL).AddGlue()
        .Add(new BButton("skip layout", fresh ? "Skip" : "Cancel", new BMessage(kSkip)))
        .Add(apply).End();
    AddShortcut(B_ESCAPE, B_NO_COMMAND_KEY, new BMessage(kSkip));
    apply->MakeDefault(true);
    CenterIn(window.Frame());
    MoveOnScreen();
  }
  bool QuitRequested() override {
    // Closing is a skip. Never commit a highlighted layout by accident.
    if (firstRun) {
      BMessage dismissed(kGuideChoice);
      dismissed.AddBool("firstRun", true);
      owner.SendMessage(&dismissed);
    }
    return true;
  }
  void MessageReceived(BMessage *m) override {
    switch (m->what) {
    case kRaiseGuide:
      if (IsMinimized())
        Minimize(false);
      Activate();
      break;
    case kPick:
      for (const auto &choice : choices)
        if (m->GetInt32("workspace", -1) == int32(choice.layout)) {
          chosen = choice.layout;
          sample->SetMessage(choice.description);
          break;
        }
      break;
    case kUse: {
      BMessage use(kGuideChoice);
      use.AddInt32("workspace", int32(chosen));
      use.AddBool("firstRun", firstRun);
      owner.SendMessage(&use);
      firstRun = false;
      PostMessage(B_QUIT_REQUESTED);
      break;
    }
    case kSkip:
      PostMessage(B_QUIT_REQUESTED);
      break;
    default:
      BWindow::MessageReceived(m);
    }
  }
};
}

BMessenger OpenLayoutIntroduction(BWindow &owner, BMessenger previous,
                                  Workspace current, bool firstRun) {
  BMessage raise(kRaiseGuide);
  if (previous.IsValid() &&
      previous.SendMessage(&raise, static_cast<BHandler *>(nullptr), 50000) == B_OK)
    return previous;
  auto *window = new Introduction(owner, current, firstRun);
  BMessenger messenger(window);
  window->Show();
  return messenger;
}
}
