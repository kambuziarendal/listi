// SPDX-License-Identifier: MIT
#include "LayoutIntroduction.h"
#include "WrappedLabel.h"
#include "Common.h"
#include <Button.h>
#include <CheckBox.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
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

// A miniature panel map previews the structure without touching the owner.
class LayoutMap : public BView {
  Workspace workspace;
public:
  explicit LayoutMap(Workspace value)
      : BView("layout preview", B_WILL_DRAW), workspace(value) {
    SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
    SetHighUIColor(B_PANEL_TEXT_COLOR);
    SetExplicitMinSize(BSize(110, 35));
  }
  void Select(Workspace value) {
    workspace = value;
    Invalidate();
  }
  void Draw(BRect) override {
    BRect frame(2, 2, 108, 32);
    StrokeRect(frame);
    if (workspace != Workspace::einn) {
      float split = workspace == Workspace::askr ? 26 : 55;
      StrokeLine(BPoint(split, frame.top), BPoint(split, frame.bottom));
    }
    if (workspace == Workspace::smidja)
      StrokeLine(BPoint(82, frame.top), BPoint(82, frame.bottom));
  }
};

class Introduction : public BWindow {
  BMessenger owner;
  Workspace chosen;
  WrappedLabel *sample;
  LayoutMap *map;
  BMenuField *sidebarField;
  Preferences initial;
  BPopUpMenu *opening = new BPopUpMenu("folder opening");
  BPopUpMenu *sidebar = new BPopUpMenu("sidebar choice");
  BCheckBox *preview, *command, *buttons;
  bool firstRun;

  void RefreshTools() {
    initial.workspace = chosen;
    map->Select(chosen);
    sidebarField->SetEnabled(!initial.IsEinn());
    sidebar->ItemAt(0)->SetMarked(true);
    sidebar->ItemAt(3)->SetEnabled(initial.HasConfigurableSidebar());
    preview->SetValue(initial.Preview());
    command->SetValue(initial.command);
    buttons->SetValue(initial.ActionButtonsVisible());
    for (auto *control : {preview, command, buttons})
      control->SetEnabled(!initial.SinglePane());
  }
public:
  Introduction(BWindow &window, const Preferences &settings, bool fresh)
      : BWindow(BRect(0, 0, 450, 310), "Listi setup", B_TITLED_WINDOW,
                B_AUTO_UPDATE_SIZE_LIMITS | B_NOT_ZOOMABLE),
        owner(&window), chosen(settings.workspace), initial(settings), firstRun(fresh) {
    auto *options = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
    sample = new WrappedLabel("layout description", "");
    for (const auto &choice : choices) {
      auto *pick = new BMessage(kPick);
      pick->AddInt32("workspace", int32(choice.layout));
      auto *radio = new BRadioButton(WorkspaceName(choice.layout), WorkspaceName(choice.layout), pick);
      options->GroupLayout()->AddView(radio);
      if (chosen == choice.layout) {
        radio->SetValue(B_CONTROL_ON);
        sample->SetMessage(choice.description);
      }
    }
    for (const char *label : Preferences::FolderOpeningLabels)
      opening->AddItem(new BMenuItem(label, nullptr));
    opening->ItemAt(initial.folderOpening)->SetMarked(true);
    for (const char *label : {"Keep layout sidebars", "Hidden", "Folder tree", "Shelf"})
      sidebar->AddItem(new BMenuItem(label, nullptr));
    preview = new BCheckBox("preview", "Preview", nullptr);
    command = new BCheckBox("command", "Command line", nullptr);
    buttons = new BCheckBox("actions", "Action buttons", nullptr);
    map = new LayoutMap(chosen);
    sidebarField = new BMenuField("sidebar", "Sidebar:", sidebar);
    RefreshTools();
    auto *apply = new BButton("apply layout", "Apply setup", new BMessage(kUse));
    BLayoutBuilder::Group<>(this, B_VERTICAL)
        .SetInsets(B_USE_WINDOW_SPACING)
        .Add(new WrappedLabel("layout help", "Choose how to browse files. You can switch later in View > Workspace."))
        .Add(options).Add(map).Add(sample)
        .Add(new BMenuField("opening", "Open external folders in:", opening))
        .Add(new WrappedLabel("windows help", "Normal folder navigation stays unchanged. With independent windows, use Haiku Stack & Tile to group them."))
        .Add(sidebarField)
        .AddGroup(B_HORIZONTAL).Add(preview).Add(command).Add(buttons).End()
        .AddGlue()
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
          RefreshTools();
          sample->SetMessage(choice.description);
          break;
        }
      break;
    case kUse: {
      BMessage use(kGuideChoice);
      use.AddInt32("workspace", int32(chosen));
      use.AddInt32("folderOpening", opening->IndexOf(opening->FindMarked()));
      use.AddInt32("sidebarChoice", sidebar->IndexOf(sidebar->FindMarked()));
      if (preview->IsEnabled()) {
        use.AddBool("preview", preview->Value());
        use.AddBool("command", command->Value());
        use.AddBool("buttons", buttons->Value());
      }
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
                                  const Preferences &settings, bool firstRun) {
  BMessage raise(kRaiseGuide);
  if (previous.IsValid() &&
      previous.SendMessage(&raise, static_cast<BHandler *>(nullptr), 50000) == B_OK)
    return previous;
  auto *window = new Introduction(owner, settings, firstRun);
  BMessenger messenger(window);
  window->Show();
  return messenger;
}
}
