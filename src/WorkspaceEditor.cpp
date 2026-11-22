// SPDX-License-Identifier: MIT
#include "WorkspaceEditor.h"
#include "WorkspaceActions.h"
#include "Common.h"
#include "WorkspaceLayout.h"

#include <Button.h>
#include <CheckBox.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include "AdaptiveScrollView.h"
#include <StringItem.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>

#include <algorithm>
#include <exception>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr uint32 kActionSelection = 'wasl';
constexpr uint32 kAddAction = 'waad';
constexpr uint32 kRemoveAction = 'warm';
constexpr uint32 kActionEarlier = 'waup';
constexpr uint32 kActionLater = 'wadn';
constexpr uint32 kProfileSelection = 'wpsl';
constexpr uint32 kAddProfile = 'wpad';
constexpr uint32 kLoadProfile = 'wpld';
constexpr uint32 kUpdateProfile = 'wpup';
constexpr uint32 kRenameProfile = 'wprn';
constexpr uint32 kRemoveProfile = 'wprm';
constexpr uint32 kReset = 'wrst';
constexpr uint32 kSave = 'wsav';

BScrollView *ScrollList(const char *name, BListView *list) {
  auto scroll = new Listi::AdaptiveScrollView(name, list, true);
  scroll->SetExplicitMinSize(BSize(155, 125));
  scroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
  return scroll;
}

void ClearList(BListView *list) {
  while (auto item = list->RemoveItem(int32(0)))
    delete item;
}

// BListView does not own the BListItems added to it.
class WorkspaceList : public BListView {
public:
  using BListView::BListView;
  ~WorkspaceList() override { ClearList(this); }
};

void AddLabel(BListView *list, const char *label) {
  list->AddItem(new BStringItem(label));
}

void SelectRow(BListView *list, int32 index) {
  if (list->Parent())
    list->Parent()->InvalidateLayout();
  if (index >= 0 && index < list->CountItems()) {
    list->Select(index);
    list->ScrollToSelection();
  }
}

BMenuField *Choice(const char *label,
                   std::initializer_list<const char *> choices) {
  auto menu = new BPopUpMenu(label, true, true);
  for (auto choice : choices)
    menu->AddItem(new BMenuItem(choice, nullptr));
  return new BMenuField(label, label, menu);
}

int32 SelectedChoice(BMenuField *field) {
  auto menu = field->Menu();
  return menu->IndexOf(menu->FindMarked());
}

void SetChoice(BMenuField *field, int32 choice) {
  field->Menu()->ItemAt(choice)->SetMarked(true);
}

class WorkspaceEditor : public BWindow {
  BMessenger target;
  WorkspaceLayout layout;
  WorkspaceProfiles profiles;
  BListView *available, *actions, *profileList;
  BButton *add, *remove, *earlier, *later;
  BButton *loadProfile, *updateProfile, *renameProfile, *removeProfile;
  BMenuField *panelArrangement, *bankZone, *commandZone, *previewZone;
  BMenuField *treeZone, *filterZone, *leftSidebar, *rightSidebar;
  BCheckBox *showBank, *showCommand, *adaptive, *fullBank;
  BTextControl *profileName;
  BStringView *workspaceLabel, *status;
  std::vector<int32> availableActions;

  void UpdateActionButtons() {
    int32 index = actions->CurrentSelection();
    add->SetEnabled(available->CurrentSelection() >= 0);
    remove->SetEnabled(index >= 0);
    earlier->SetEnabled(index > 0);
    later->SetEnabled(index >= 0 && index + 1 < actions->CountItems());
  }

  void RefreshActions(int32 selected = -1, int32 remaining = -1) {
    ClearList(available);
    ClearList(actions);
    availableActions.clear();
    for (auto id : layout.actionButtons)
      AddLabel(actions, kWorkspaceActions[id].description);
    for (int32 id = 0; id < WorkspaceLayout::ActionButtonCount; ++id) {
      if (std::find(layout.actionButtons.begin(), layout.actionButtons.end(),
                    id) == layout.actionButtons.end()) {
        availableActions.push_back(id);
        AddLabel(available, kWorkspaceActions[id].description);
      }
    }
    SelectRow(actions, selected);
    SelectRow(available, remaining);
    UpdateActionButtons();
  }

  void EditActions(uint32 action) {
    auto &order = layout.actionButtons;
    int32 index = actions->CurrentSelection();
    if (action == kAddAction) {
      int32 from = available->CurrentSelection();
      if (from < 0 || from >= int32(availableActions.size()))
        return;
      order.push_back(availableActions[from]);
      RefreshActions(int32(order.size()) - 1,
                     std::min(from, int32(availableActions.size()) - 2));
      return;
    }
    if (index < 0 || index >= int32(order.size()))
      return;
    if (action == kRemoveAction) {
      order.erase(order.begin() + index);
      RefreshActions(std::min(index, int32(order.size()) - 1),
                     available->CurrentSelection());
      return;
    }
    const int32 next = index + (action == kActionEarlier ? -1 : 1);
    if (next >= 0 && next < int32(order.size())) {
      std::swap(order[index], order[next]);
      RefreshActions(next, available->CurrentSelection());
    }
  }

  void ShowModules() {
    // All placement fields are retained independently of their visibility.
    SetChoice(panelArrangement, layout.panelsStacked ? 1 : 0);
    SetChoice(bankZone, layout.bankTop ? 1 : 0);
    SetChoice(commandZone, layout.commandTop ? 1 : 0);
    SetChoice(previewZone, layout.inspectorLeft ? 1 : 0);
    SetChoice(treeZone, layout.treeRight ? 1 : 0);
    SetChoice(filterZone, layout.filterBottom ? 1 : 0);
    SetChoice(leftSidebar, static_cast<int32>(layout.smidjaSidebar[0]));
    SetChoice(rightSidebar, static_cast<int32>(layout.smidjaSidebar[1]));
    showBank->SetValue(layout.buttons ? B_CONTROL_ON : B_CONTROL_OFF);
    fullBank->SetValue(layout.fullActionButtons ? B_CONTROL_ON : B_CONTROL_OFF);
    showCommand->SetValue(layout.command ? B_CONTROL_ON : B_CONTROL_OFF);
    adaptive->SetValue(layout.adaptive ? B_CONTROL_ON : B_CONTROL_OFF);
  }

  void ReadModules() {
    layout.panelsStacked = SelectedChoice(panelArrangement) == 1;
    layout.bankTop = SelectedChoice(bankZone) == 1;
    layout.commandTop = SelectedChoice(commandZone) == 1;
    layout.inspectorLeft = SelectedChoice(previewZone) == 1;
    layout.treeRight = SelectedChoice(treeZone) == 1;
    layout.filterBottom = SelectedChoice(filterZone) == 1;
    layout.SetSidebar(Workspace::smidja, 0,
                      static_cast<SidebarMode>(SelectedChoice(leftSidebar)));
    layout.SetSidebar(Workspace::smidja, 1,
                      static_cast<SidebarMode>(SelectedChoice(rightSidebar)));
    layout.buttons = showBank->Value() == B_CONTROL_ON;
    layout.fullActionButtons = fullBank->Value() == B_CONTROL_ON;
    layout.command = showCommand->Value() == B_CONTROL_ON;
    layout.adaptive = adaptive->Value() == B_CONTROL_ON;
  }

  void UpdateProfileButtons() {
    const bool selected = profileList->CurrentSelection() >= 0;
    for (auto button :
         {loadProfile, updateProfile, renameProfile, removeProfile})
      button->SetEnabled(selected);
  }

  void RefreshProfiles(int32 selected = -1) {
    ClearList(profileList);
    for (const auto &profile : profiles.Items())
      AddLabel(profileList, profile.name.c_str());
    SelectRow(profileList, selected);
    UpdateProfileButtons();
  }

  void ShowLayout() {
    RefreshActions();
    ShowModules();
    workspaceLabel->SetText(WorkspaceMenuLabel(layout.workspace));
  }

  void EditProfiles(uint32 action) {
    ReadModules();
    const auto &items = profiles.Items();
    const int32 selected = profileList->CurrentSelection();
    if (action != kAddProfile &&
        (selected < 0 || selected >= int32(items.size())))
      return;
    try {
      switch (action) {
      case kAddProfile:
        profiles.Add(profileName->Text(), layout);
        RefreshProfiles(int32(items.size()) - 1);
        profileName->SetText("");
        status->SetText("Profile created. Click Save to keep it.");
        break;
      case kLoadProfile:
        layout = items[selected].layout;
        ShowLayout();
        status->SetText(
            "Profile loaded into the draft. Click Save to apply it.");
        break;
      case kUpdateProfile:
        profiles.Update(items[selected].name, layout);
        status->SetText("Profile updated. Click Save to keep the change.");
        break;
      case kRenameProfile: {
        const std::string oldName = items[selected].name;
        profiles.Rename(oldName, profileName->Text());
        RefreshProfiles(selected);
        profileName->SetText("");
        status->SetText("Profile renamed. Click Save to keep the change.");
        break;
      }
      case kRemoveProfile: {
        const std::string name = items[selected].name;
        profiles.Remove(name);
        RefreshProfiles(std::min(selected, int32(items.size()) - 1));
        status->SetText(
            "Profile removed from the draft. Click Save to delete it.");
        break;
      }
      }
    } catch (const std::exception &error) {
      status->SetText(error.what());
      if (action == kAddProfile || action == kRenameProfile)
        profileName->MakeFocus();
    }
  }

  void Save() {
    ReadModules();
    BMessage apply(kApplyWorkspaceEditor), savedLayout, savedProfiles;
    layout.Archive(savedLayout);
    profiles.Archive(savedProfiles);
    if (apply.AddMessage("layout", &savedLayout) == B_OK &&
        apply.AddMessage("profiles", &savedProfiles) == B_OK &&
        target.SendMessage(&apply) == B_OK) {
      PostMessage(B_QUIT_REQUESTED);
    } else {
      status->SetText("Could not send workspace changes. Try again or cancel.");
    }
  }

  BGroupView *ActionPage() {
    available = new WorkspaceList("available actions", B_SINGLE_SELECTION_LIST);
    actions = new WorkspaceList("visible actions", B_SINGLE_SELECTION_LIST);
    for (auto list : {available, actions}) {
      list->SetSelectionMessage(new BMessage(kActionSelection));
      list->SetTarget(this);
    }
    available->SetInvocationMessage(new BMessage(kAddAction));
    actions->SetInvocationMessage(new BMessage(kRemoveAction));
    add = new BButton("add action", "Add →", new BMessage(kAddAction));
    remove =
        new BButton("remove action", "← Remove", new BMessage(kRemoveAction));
    earlier =
        new BButton("move action up", "Move up", new BMessage(kActionEarlier));
    later = new BButton("move action down", "Move down",
                        new BMessage(kActionLater));

    fullBank = new BCheckBox("Use full action buttons", nullptr);
    auto page = new BGroupView("Action buttons", B_VERTICAL);
    BLayoutBuilder::Group<>(page, B_VERTICAL, 8)
        .SetInsets(B_USE_WINDOW_SPACING)
        .Add(fullBank)
        .AddGroup(B_HORIZONTAL, 8)
        .AddGroup(B_VERTICAL, 5)
        .Add(new BStringView("available label", "Available actions"))
        .Add(ScrollList("available actions scroll", available))
        .End()
        .AddGroup(B_VERTICAL, 5)
        .AddGlue()
        .Add(add)
        .Add(remove)
        .AddGlue()
        .End()
        .AddGroup(B_VERTICAL, 5)
        .Add(new BStringView("visible label", "Button order"))
        .Add(ScrollList("visible actions scroll", actions))
        .End()
        .AddGroup(B_VERTICAL, 5)
        .AddGlue()
        .Add(earlier)
        .Add(later)
        .AddGlue()
        .End()
        .End();
    RefreshActions();
    return page;
  }

  BGroupView *ModulePage() {
    showBank = new BCheckBox("Show action buttons", nullptr);
    showCommand = new BCheckBox("Show command field", nullptr);
    panelArrangement =
        Choice("File panel arrangement:", {"Side by side", "Stacked"});
    bankZone =
        Choice("Action buttons:", {"Below file panels", "Above file panels"});
    commandZone =
        Choice("Command field:", {"Below file panels", "Above file panels"});
    commandZone->SetToolTip("This field runs a command in an external terminal. "
                           "Move the Listi CLI with its own position button.");
    previewZone =
        Choice("Preview position:", {"After file panels", "Before file panels"});
    previewZone->SetToolTip(
        "After means right (side by side) or bottom (stacked).");
    treeZone = Choice("Navigation sidebar side:",
                      {"Left within panel", "Right within panel"});
    filterZone = Choice("Name filter:", {"Above file list", "Below file list"});
    const auto sidebarChoices = {SidebarModeLabel(SidebarMode::Hidden),
                                 SidebarModeLabel(SidebarMode::FolderTree),
                                 SidebarModeLabel(SidebarMode::Favorites),
                                 SidebarModeLabel(SidebarMode::ShortcutsAndTree),
                                 SidebarModeLabel(SidebarMode::Shelf),
                                 SidebarModeLabel(SidebarMode::RecentFolders)};
    leftSidebar = Choice("Smiðja first sidebar:", sidebarChoices);
    rightSidebar = Choice("Smiðja second sidebar:", sidebarChoices);
    adaptive =
        new BCheckBox("adaptive workspace",
                      "Wrap full action buttons in narrow windows", nullptr);
    adaptive->SetToolTip(
        "Uses three columns and shorter button labels without moving panels.");

    auto page = new BGroupView("Modules", B_VERTICAL);
    auto choices = BLayoutBuilder::Group<>(page, B_VERTICAL, 8)
                       .SetInsets(B_USE_WINDOW_SPACING)
                       .AddGroup(B_HORIZONTAL, 15)
                       .Add(showBank)
                       .Add(showCommand)
                       .AddGlue()
                       .End()
                       .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
    int32 row = 0;
    for (auto field : {panelArrangement, bankZone, commandZone, previewZone,
                       treeZone, filterZone, leftSidebar, rightSidebar}) {
      choices.Add(field->CreateLabelLayoutItem(), 0, row)
          .Add(field->CreateMenuBarLayoutItem(), 1, row);
      ++row;
    }
    choices.End().Add(adaptive).AddGlue();
    ShowModules();
    return page;
  }

  BGroupView *ProfilePage() {
    profileList = new WorkspaceList("saved profiles", B_SINGLE_SELECTION_LIST);
    profileList->SetSelectionMessage(new BMessage(kProfileSelection));
    profileList->SetInvocationMessage(new BMessage(kLoadProfile));
    profileList->SetTarget(this);
    profileName =
        new BTextControl("profile name", "Profile name:", "", nullptr);
    profileName->TextView()->SetMaxBytes(128);
    auto addProfile =
        new BButton("create profile", "Create", new BMessage(kAddProfile));
    loadProfile =
        new BButton("load profile", "Load", new BMessage(kLoadProfile));
    updateProfile =
        new BButton("update profile", "Update", new BMessage(kUpdateProfile));
    renameProfile =
        new BButton("rename profile", "Rename", new BMessage(kRenameProfile));
    removeProfile =
        new BButton("delete profile", "Delete", new BMessage(kRemoveProfile));
    addProfile->SetToolTip("Save the current draft as a new named profile.");
    loadProfile->SetToolTip(
        "Replace the current draft with the selected profile.");
    updateProfile->SetToolTip(
        "Replace the selected profile's layout with this draft.");
    renameProfile->SetToolTip(
        "Give the selected profile the name entered above.");

    auto page = new BGroupView("Profiles", B_VERTICAL);
    BLayoutBuilder::Group<>(page, B_VERTICAL, 8)
        .SetInsets(B_USE_WINDOW_SPACING)
        .Add(ScrollList("profiles scroll", profileList))
        .AddGroup(B_HORIZONTAL, 8)
        .Add(profileName)
        .Add(addProfile)
        .Add(renameProfile)
        .End()
        .AddGroup(B_HORIZONTAL, 8)
        .Add(loadProfile)
        .Add(updateProfile)
        .Add(removeProfile)
        .AddGlue()
        .End();
    RefreshProfiles();
    return page;
  }

public:
  WorkspaceEditor(BMessenger owner, const Preferences &prefs, bool showModules)
      : BWindow(BRect(0, 0, 625, 430), "Customize workspace", B_MODAL_WINDOW,
                B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS |
                    B_NOT_ZOOMABLE),
        target(owner), layout(prefs.CaptureLayout()), profiles(prefs.profiles) {
    auto tabs = new BTabView("workspace tabs", B_WIDTH_FROM_LABEL);
    tabs->AddTab(ActionPage());
    tabs->AddTab(ModulePage());
    tabs->AddTab(ProfilePage());
    workspaceLabel = new BStringView("draft workspace",
                                     WorkspaceMenuLabel(layout.workspace));
    status = new BStringView("workspace status", "");
    status->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
    status->SetTruncation(B_TRUNCATE_END);
    auto reset = new BButton("Reset layout", new BMessage(kReset));
    reset->SetToolTip("Restore the default layout without deleting profiles.");
    BLayoutBuilder::Group<>(this, B_VERTICAL, 8)
        .SetInsets(B_USE_WINDOW_SPACING)
        .Add(workspaceLabel)
        .Add(tabs)
        .Add(status)
        .AddGroup(B_HORIZONTAL, 8)
        .Add(reset)
        .AddGlue()
        .Add(new BButton("cancel workspace", "Cancel",
                         new BMessage(B_QUIT_REQUESTED)))
        .Add(new BButton("save workspace", "Save", new BMessage(kSave)))
        .End();
    GetLayout()->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    AddShortcut(B_ESCAPE, B_NO_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
    if (showModules)
      tabs->Select(1);
    CenterOnScreen();
    Show();
  }

  void MessageReceived(BMessage *message) override {
    switch (message->what) {
    case kActionSelection:
      UpdateActionButtons();
      break;
    case kAddAction:
    case kRemoveAction:
    case kActionEarlier:
    case kActionLater:
      EditActions(message->what);
      break;
    case kProfileSelection:
      UpdateProfileButtons();
      break;
    case kAddProfile:
    case kLoadProfile:
    case kUpdateProfile:
    case kRenameProfile:
    case kRemoveProfile:
      EditProfiles(message->what);
      break;
    case kReset:
      layout = WorkspaceLayout();
      ShowLayout();
      status->SetText("Default layout restored in draft. Profiles unchanged.");
      break;
    case kSave:
      Save();
      break;
    default:
      BWindow::MessageReceived(message);
    }
  }
};
}

void OpenWorkspaceEditor(BMessenger target, const Preferences &prefs,
                         bool showModules) {
  new WorkspaceEditor(target, prefs, showModules);
}
