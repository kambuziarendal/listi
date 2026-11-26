// SPDX-License-Identifier: MIT
// Exercise the actual window and menu without writing real user preferences.
#include "../src/ArchiveActions.h"
#include "../src/Common.h"
#include "../src/FunctionButtonBank.h"
#include "../src/ImagePreview.h"
#include "../src/InspectorEditor.h"
#include "../src/FileClipboard.h"
#include "../src/FolderTabs.h"
#include "../src/MainWindow.h"
#include "../src/MainWindowMenu.h"
#include "../src/Pane.h"
#include "../src/UnavailableVolume.h"
#include "../src/PanelTools.h"
#include "../src/PaneSidebar.h"
#include "../src/SavedPlaces.h"
#include "../src/SavedSearches.h"
#include "../src/TerminalPanel.h"
#include "../src/WindowKeyFilter.h"
#include "../src/WorkspaceSplit.h"
#include "../src/WorkspaceEditor.h"
#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <ColumnListView.h>
#include <GridView.h>
#include <Layout.h>
#include <LayoutItem.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <NodeMonitor.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>
#include <List.h>
#include <ListView.h>
#include <StringItem.h>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

static void CheckFunctionButtons() {
  MainWindowMenu menu;
  auto *recovery = menu.FindItem(kReviewRecovery);
  assert(recovery && std::string(recovery->Label()) == "Review interrupted operations…");
  Listi::FunctionButtonBank bank(menu);
  auto *fallbackItem = menu.FindItem("Function actions");
  assert(fallbackItem && fallbackItem->Submenu());
  auto *fallback = fallbackItem->Submenu();

  auto grid = [&]() -> BGridView * {
    for (int32 i = 0; i < bank.CountChildren(); ++i)
      if (auto *view = dynamic_cast<BGridView *>(bank.ChildAt(i)))
        return view;
    return nullptr;
  };
  auto button = [&](int32 index, uint32 action, const char *label) {
    assert(grid());
    auto *control = dynamic_cast<BButton *>(grid()->ChildAt(index));
    assert(control && std::string(control->Label()) == label);
    assert(control->Message()->what == action);
    assert(fallback->ItemAt(index)->Message()->what == action);
  };

  const std::vector<int32> actions = {2, -1, 7, 99, 1};
  bank.SetActions(actions, false);
  assert(!bank.IsCompact());
  assert(grid() && grid()->CountChildren() == 3 && fallback->CountItems() == 3);
  button(0, kCopy, "Copy [Shift+C]");
  button(1, kNone, "None");
  button(2, kEdit, "Edit [E]");
  assert(std::string(fallback->ItemAt(0)->Label()) == "Copy to other panel");

  bank.SetActions(actions, true);
  assert(bank.IsCompact());
  button(0, kCopy, "Copy");
  button(1, kNone, "None");
  button(2, kEdit, "Edit");
  bank.SetDestination("←", "/boot/home/Downloads");
  button(0, kCopy, "Copy ← Downloads");
  bank.SetDestination("→", "/other/Downloads");
  button(0, kCopy, "Copy → Downloads");
  bank.SetActions({}, true);
  assert(grid() && grid()->CountChildren() == 0 && fallback->CountItems() == 0);
}

static void CheckToolPlacement(BWindow *window) {
  auto *top = window->FindView("top workspace tools");
  auto *bottom = window->FindView("bottom workspace tools");
  auto *bank = window->FindView("workspace action buttons");
  auto *transfer = window->FindView("panel transfer target");
  assert(top && bottom && bank && transfer && transfer->Parent() == top &&
         bottom->CountChildren() == 2);
  auto *command = dynamic_cast<BTextControl *>(bottom->ChildAt(1));
  assert(command);
  command->SetText("echo retained draft");
  for (bool above : {true, false}) {
    WorkspaceLayout layout;
    layout.bankTop = layout.commandTop = above;
    layout.command = true;
    layout.fullActionButtons = true;
    layout.actionButtons = {2};
    BMessage apply(kApplyWorkspaceEditor), saved, profiles;
    layout.Archive(saved);
    apply.AddMessage("layout", &saved);
    apply.AddMessage("profiles", &profiles);
    window->MessageReceived(&apply);
    auto *target = above ? top : bottom;
    auto *empty = above ? bottom : top;
    assert(target->CountChildren() == (above ? 3 : 2) &&
           empty->CountChildren() == (above ? 0 : 1));
    assert(target->GetLayout()->ItemAt(0)->View() == bank && bank->Parent() == target &&
           command->Parent() == target);
    assert(std::string(command->Text()) == "echo retained draft");
    assert(!command->IsHidden(target) && !target->IsHidden(target));
    assert(empty->IsHidden(empty) == above);
  }

  auto *menu = static_cast<BMenuBar *>(window->FindView("menu"));
  auto *toggle = menu->FindItem(kToggleBank);
  auto *full = menu->FindItem(kToggleFullBank);
  auto *fallback = menu->FindItem("Function actions");
  assert(full && full->IsEnabled() && full->IsMarked());
  BMessage compact(kToggleFullBank);
  window->MessageReceived(&compact);
  assert(!full->IsMarked());
  auto *compactGrid = dynamic_cast<BGridView *>(bank->ChildAt(0));
  assert(compactGrid && compactGrid->CountChildren() == 5);
  const uint32 expected[] = {kView, kCopy, kMove, kRename, kMkdir};
  for (int32 i = 0; i < 5; ++i) {
    auto *button = dynamic_cast<BButton *>(compactGrid->ChildAt(i));
    assert(button && button->Message()->what == expected[i]);
  }
  assert(toggle && fallback && toggle->IsEnabled() && toggle->IsMarked());
  assert(!bank->IsHidden(bank) && !fallback->IsEnabled());
  BMessage notice(kInlineNotice);
  notice.AddString("error", "Non-blocking file error");
  window->MessageReceived(&notice);
  assert(!window->FindView("question") && !bank->IsHidden(bank));
  BMessage prompt(Listi::kTabRename);
  window->MessageReceived(&prompt);
  assert(bank->IsHidden(bank) && !command->IsHidden(command));
  assert(!bottom->IsHidden(bottom) && toggle->IsMarked() && !fallback->IsEnabled());
  BMessage cancel(kInlineCancel);
  window->MessageReceived(&cancel);
  assert(!bank->IsHidden(bank));

  auto selectWorkspace = [&](Workspace workspace) {
    BMessage change(kWorkspace);
    change.AddInt32("workspace", int32(workspace));
    window->MessageReceived(&change);
  };
  BMessage hideBank(kToggleBank);
  window->MessageReceived(&hideBank);
  assert(!toggle->IsMarked() && bank->IsHidden(bank));
  selectWorkspace(Workspace::askr);
  assert(!toggle->IsEnabled() && command->IsHidden(command));
  assert(bottom->IsHidden(bottom));
  selectWorkspace(Workspace::smidja);
  assert(toggle->IsEnabled() && toggle->IsMarked() && !command->IsHidden(command));
  selectWorkspace(Workspace::tviskipt);
  assert(toggle->IsEnabled() && !toggle->IsMarked() && bank->IsHidden(bank));
  assert(std::string(command->Text()) == "echo retained draft");
  window->MessageReceived(&hideBank);
}

static void CheckWorkspaceSplit(Listi::WorkspaceSplit &split, BView *first) {
  auto *inspector = &split.Inspector();
  auto near = [](float a, float b) { return std::fabs(a - b) < 0.0001f; };

  WorkspaceLayout layout;
  layout.panelsStacked = true;
  layout.splitWeights[0] = 0.3f;
  layout.splitWeights[1] = 0.5f;
  layout.splitWeights[2] = 0.2f;
  split.PlaceInspector(true, layout.splitWeights[2]);
  split.ApplyGeometry(layout, false, 0);
  assert(split.Orientation() == B_VERTICAL);
  assert(split.IsCollapsible(0) && !split.IsCollapsible(1) &&
         !split.IsCollapsible(2));
  assert(near(split.ItemWeight(0), 0.2f));
  assert(near(split.ItemWeight(1), 0.3f));
  assert(near(split.ItemWeight(2), 0.5f));

  split.SetInspectorVisible(false);
  assert(inspector->IsHidden(inspector));
  split.SetItemWeight(1, 0.6f, true);
  split.SetItemWeight(2, 0.4f, true);
  split.RememberWeights(layout, false);
  assert(near(layout.splitWeights[0], 0.48f));
  assert(near(layout.splitWeights[1], 0.32f));
  assert(near(layout.splitWeights[2], 0.2f));

  layout.workspace = Workspace::einn;
  split.ApplyGeometry(layout, false, 1);
  assert(split.Spacing() == 0 && split.SplitterSize() == 0);
  split.SetInspectorVisible(true);
  split.SetPaneVisible(0, false);
  split.ApplyGeometry(layout, true, 1);
  assert(split.Spacing() > 0 && split.SplitterSize() > 0);
  assert(!inspector->IsHidden(inspector) && first->IsHidden(first));
  assert(near(split.ItemWeight(0), 0.48f));
  split.RememberWeights(layout, true);
  assert(near(layout.splitWeights[2], 0.2f));
}

static void CheckFloatingInspector(BApplication &app) {
  char directory[] = "/tmp/Listi-floating-inspector-XXXXXX";
  assert(mkdtemp(directory));
  const auto root = std::filesystem::canonical(directory);
  const std::string first = (root / "first.txt").string();
  const std::string second = (root / "second.txt").string();
  const std::string content = "Second file\n";
  std::ofstream(first) << "First file\n";
  std::ofstream(second) << content;
  Preferences prefs;
  prefs.workspace = Workspace::tviskipt;
  prefs.SetPreview(true);
  BrowserRequest request;
  request.path = root.string();
  auto *browser = CreateMainWindow(prefs, {}, {}, false, {}, false, request);
  auto *other = CreateMainWindow(prefs, {}, {}, false, {}, false, request);
  BMessenger owner(browser), independent(other);
  browser->Show();
  other->Show();
  auto with = [](const BMessenger &target, auto check) {
    if (target.LockTargetWithTimeout(500000) != B_OK)
      return false;
    BLooper *looper = nullptr;
    target.Target(&looper);
    const bool result = check(*static_cast<BWindow *>(looper));
    looper->Unlock();
    return result;
  };
  auto wait = [](auto check, const char *label) {
    const auto until = system_time() + 8000000;
    while (!check()) {
      if (system_time() >= until) {
        fprintf(stderr, "FAIL %s\n", label);
        std::exit(1);
      }
      snooze(20000);
    }
    printf("PASS %s\n", label);
  };
  auto select = [&](const BMessenger &target, const char *name) {
    return with(target, [&](BWindow &window) {
      auto *panel = dynamic_cast<Listi::PanelTools *>(window.FindView("first panel tools"));
      if (!panel || !panel->Files().LocationValid() || panel->Files().IsScanning())
        return false;
      panel->Files().MatchPattern(name, true);
      return !panel->Files().SelectedPath().empty();
    });
  };
  auto find = [&](const char *title) {
    for (int32 i = 0; i < app.CountWindows(); ++i) {
      auto *window = app.WindowAt(i);
      if (window && window->Title() && std::string(window->Title()) == title)
        return BMessenger(window);
    }
    return BMessenger{};
  };
  auto workspace = [&] {
    BMessage request(B_GET_PROPERTY), reply;
    request.AddSpecifier("Workspace");
    if (owner.SendMessage(&request, &reply, 1000000, 1000000) != B_OK)
      return std::string{};
    return std::string(reply.GetString("result", ""));
  };
  ImagePreview *original = nullptr;
  wait([&] { return select(owner, "first.txt") && with(owner, [&](BWindow &window) {
    original = dynamic_cast<ImagePreview *>(window.FindView("inspector"));
    return original && !original->IsHidden();
  }); }, "browser exposes a docked Inspector");
  assert(owner.SendMessage(Listi::kDetachInspector) == B_OK);
  BMessenger floating;
  wait([&] {
    floating = find("Listi: Inspector");
    return with(floating, [&](BWindow &window) {
      return window.FindView("inspector") == original && original->File() == first;
    });
  }, "detach the same Inspector and preview the browser selection");
  assert(with(floating, [&](BWindow &window) {
    window.ResizeTo(640, 460);
    return window.Bounds().Width() == 640 && window.Bounds().Height() == 460;
  }));
  assert(owner.SendMessage(kQuickView) == B_OK);
  std::string observedWorkspace;
  wait([&] {
    observedWorkspace = workspace();
    return !observedWorkspace.empty();
  }, "owner answers the workspace query after Quick view");
  assert(observedWorkspace == WorkspaceName(Workspace::tviskipt));
  wait([&] { return with(owner, [&](BWindow &window) {
    auto *right = window.FindView("second panel tools");
    return right && !right->IsHidden() && !window.FindView("inspector");
  }); }, "Quick view keeps both panels docked and raises the floating Inspector");
  wait([&] { return select(owner, "second.txt") && select(independent, "first.txt"); },
       "change selections in both browsers");
  wait([&] { return with(floating, [&](BWindow &) { return original->File() == second; }); },
       "Inspector follows its owner, not the other browser");
  assert(owner.SendMessage(kEdit) == B_OK);
  BTextView *body = nullptr;
  wait([&] { return with(floating, [&](BWindow &window) {
    body = dynamic_cast<BTextView *>(window.FindView("editor text"));
    if (!body)
      return false;
    body->Insert("Unsaved draft ");
    return original->HasEditor();
  }); }, "Edit opens the Inspector's embedded editor while floating");
  BMessage change(kWorkspace);
  change.AddInt32("workspace", int32(Workspace::smidja));
  assert(owner.SendMessage(&change) == B_OK);
  wait([&] { return workspace() == WorkspaceName(Workspace::smidja) &&
    with(floating, [&](BWindow &window) { return window.FindView("editor text") == body; });
  }, "Smiðja keeps the detached Inspector and its draft");
  assert(!InspectorEditorsAllowQuit());
  assert(owner.SendMessage(B_QUIT_REQUESTED) == B_OK);
  wait([&] {
    return with(find("Cannot close Listi"), [&](BWindow &window) {
      auto *alert = dynamic_cast<BAlert *>(&window);
      return owner.IsValid() && alert && alert->ButtonAt(0)->Invoke() == B_OK;
    });
  }, "dirty floating editor blocks closing its owner and remains open");
  assert(independent.SendMessage(B_QUIT_REQUESTED) == B_OK);
  wait([&] { return !independent.IsValid() && floating.IsValid(); },
       "dirty floating editor does not block an unrelated browser from closing");
  assert(floating.SendMessage(B_QUIT_REQUESTED) == B_OK);
  wait([&] { return !floating.IsValid() && with(owner, [&](BWindow &window) {
    auto *editorText = dynamic_cast<BTextView *>(window.FindView("editor text"));
    return window.FindView("inspector") == original && editorText == body &&
           std::string(editorText->Text()).find("Unsaved draft ") != std::string::npos;
  }); }, "closing the floating window redocks the same unsaved draft");
  assert(owner.SendMessage(Listi::kDetachInspector) == B_OK);
  wait([&] {
    floating = find("Listi: Inspector");
    return with(floating, [&](BWindow &window) {
      if (window.FindView("editor text") != body)
        return false;
      body->SetText(content.c_str());
      return true;
    });
  }, "detach the existing draft again without creating another Inspector");
  assert(InspectorEditorsAllowQuit());
  assert(owner.SendMessage(B_QUIT_REQUESTED) == B_OK);
  wait([&] { return !owner.IsValid() && !floating.IsValid(); },
       "closing the clean owner also closes its detached Inspector");
  assert(unlink(first.c_str()) == 0 && unlink(second.c_str()) == 0 && rmdir(directory) == 0);
}

static void CheckWindowCLIQuit(BApplication &app) {
  auto *window = CreateMainWindow(Preferences{}, BMessenger{}, {}, false, {}, false);
  auto *other = CreateMainWindow(Preferences{}, BMessenger{}, {}, false, {}, false);
  auto startCLI = [](BWindow &owner, uint32 action, int lister) {
    BMessage open(action);
    open.AddInt32("terminal lister", lister);
    assert(owner.PostMessage(&open) == B_OK);
  };
  BMessenger owner(window), survivor(other), detached;
  window->Show();
  other->Show();
  startCLI(*window, Listi::kLeftCLI, 0);
  startCLI(*window, Listi::kShowFloatingTerminal, 1);
  startCLI(*other, Listi::kLeftCLI, 0);

  auto wait = [](auto condition) {
    for (int i = 0; i < 250; ++i) {
      if (condition())
        return;
      snooze(20000);
    }
    assert(false && "Timed out waiting for CLI quit state");
  };
  auto findWindow = [&](const std::string &title, bool prefix = false) {
    for (int32 i = 0; i < app.CountWindows(); ++i) {
      auto *candidate = app.WindowAt(i);
      if (candidate && candidate->Title() && (prefix ? std::string(candidate->Title()).find(title) == 0 : candidate->Title() == title))
        return BMessenger(candidate);
    }
    return BMessenger{};
  };
  auto hasCLI = [](BMessenger target, int lister) {
    if (target.LockTargetWithTimeout(500000) != B_OK)
      return false;
    BLooper *looper = nullptr;
    target.Target(&looper);
    auto *terminal = dynamic_cast<Listi::TerminalPanel *>(
        static_cast<BWindow *>(looper)->FindView(Listi::TerminalName(lister)));
    const bool active = terminal && terminal->Active() && !terminal->IsHidden();
    looper->Unlock();
    return active;
  };
  wait([&] {
    detached = findWindow(std::string(Listi::TerminalName(1)) + " | Tab 1: ", true);
    return hasCLI(owner, 0) && hasCLI(detached, 1) && hasCLI(survivor, 0);
  });

  auto withTabs = [](BMessenger target, auto action) {
    if (target.LockTargetWithTimeout(500000) != B_OK)
      return false;
    BLooper *looper = nullptr;
    target.Target(&looper);
    auto &browser = *static_cast<BWindow *>(looper);
    auto *panel = dynamic_cast<Listi::PanelTools *>(browser.FindView("first panel tools"));
    const bool result = panel && action(browser, panel->FolderBar());
    looper->Unlock();
    return result;
  };
  int64 activeTab = -1;
  assert(withTabs(owner, [&](BWindow &browser, Listi::FolderTabs &tabs) {
    const auto background = tabs.ActiveId();
    for (int i = 0; i < 12; ++i) {
      BMessage duplicate(Listi::kTabDuplicate);
      duplicate.AddInt32("pane", 0);
      browser.MessageReceived(&duplicate);
    }
    activeTab = tabs.ActiveId();
    BMessage close(Listi::kTabClose);
    close.AddInt32("pane", 0);
    close.AddInt64("tab", background);
    browser.MessageReceived(&close);
    return tabs.Count() == 12 && tabs.CanReopen() && tabs.ActiveId() == activeTab;
  }));
  const auto tabsIntact = [&](BWindow &, Listi::FolderTabs &tabs) {
    return tabs.Count() == 12 && tabs.CanReopen() && tabs.ActiveId() == activeTab;
  };
  assert(hasCLI(owner, 0) && hasCLI(detached, 1) && hasCLI(survivor, 0));
  puts("Tab overflow and background close leave docked and detached CLIs active");

  for (int answer : {0, 1}) {
    assert(owner.SendMessage(B_QUIT_REQUESTED) == B_OK);
    BMessenger alert;
    wait([&] {
      alert = findWindow("Close CLI sessions?");
      return alert.IsValid();
    });
    assert(alert.LockTargetWithTimeout(500000) == B_OK);
    BLooper *looper = nullptr;
    alert.Target(&looper);
    auto *dialog = dynamic_cast<BAlert *>(looper);
    assert(dialog && dialog->DefaultButton() == dialog->ButtonAt(0));
    assert(std::string(dialog->ButtonAt(answer)->Label()) ==
           (answer == 0 ? "Cancel" : "Close CLI and window"));
    assert(dialog->ButtonAt(answer)->Invoke() == B_OK);
    looper->Unlock();
    wait([&] { return !alert.IsValid(); });
    if (answer == 0) {
      assert(hasCLI(owner, 0) && hasCLI(detached, 1) && hasCLI(survivor, 0));
      assert(withTabs(owner, tabsIntact));
    }
  }
  wait([&] { return !owner.IsValid() && !detached.IsValid(); });
  assert(hasCLI(survivor, 0));
  assert(other->Lock());
  other->Quit();
  puts("Window quit: Cancel preserves CLIs; confirm closes this window's CLIs only");
}

int main(int argc, char **argv) {
  BApplication app("application/x-vnd.Listi-MainWindowRoutingTest");
  CheckFunctionButtons();
  auto *window = CreateMainWindow(Preferences{}, BMessenger{}, {}, false, {}, false);
  CheckToolPlacement(window);
  auto *menu = dynamic_cast<BMenuBar *>(window->FindView("menu"));
  assert(menu);
  auto item = [&](uint32 action) {
    auto *entry = menu->FindItem(action);
    assert(entry);
    return entry;
  };
  uint32 quickModifiers, archiveModifiers;
  const char quick = item(kQuickView)->Shortcut(&quickModifiers);
  const char archive = item(kCreateArchive)->Shortcut(&archiveModifiers);
  assert(std::toupper(quick) == 'P' && (quickModifiers & B_SHIFT_KEY));
  assert(std::toupper(archive) != 'P' || quickModifiers != archiveModifiers);

  auto send = [&](uint32 what) {
    BMessage message(what);
    window->MessageReceived(&message);
  };
  auto workspace = [&](Workspace value) {
    BMessage message(kWorkspace);
    message.AddInt32("workspace", static_cast<int32>(value));
    window->MessageReceived(&message);
  };
  // Exercise the installed common filter, not a standalone key mapping.
  Listi::WindowKeyFilter *keys = nullptr;
  auto *filters = window->CommonFilterList();
  for (int32 i = 0; filters && i < filters->CountItems(); ++i) {
    auto *filter = static_cast<BMessageFilter *>(filters->ItemAt(i));
    if ((keys = dynamic_cast<Listi::WindowKeyFilter *>(filter)))
      break;
  }
  assert(keys);
  auto key = [&](BView *focus, const char *bytes, int32 modifiers) {
    focus->MakeFocus();
    auto *targetView = window->CurrentFocus();
    auto *ancestor = targetView;
    while (ancestor && ancestor != focus)
      ancestor = ancestor->Parent();
    assert(ancestor == focus);
    BMessage message(B_KEY_DOWN);
    message.AddString("bytes", bytes);
    message.AddInt32("modifiers", modifiers);
    BHandler *target = targetView;
    return keys->Filter(&message, &target);
  };
  auto *files = window->FindView("files");
  auto *filter = dynamic_cast<BTextControl *>(window->FindView("filter"));
  assert(files && filter);
  assert(key(files, "d", B_COMMAND_KEY) == B_DISPATCH_MESSAGE);
  assert(key(filter->TextView(), "d", B_COMMAND_KEY) == B_SKIP_MESSAGE);
  assert(key(filter->TextView(), "f", B_COMMAND_KEY) == B_DISPATCH_MESSAGE);
  assert(item(kSelectPattern)->IsEnabled());
  assert(item(kSwitch)->IsEnabled() && item(kCopy)->IsEnabled());
  send(kQuickView);
  assert(item(kQuickView)->IsMarked() && !item(kSwitch)->IsEnabled());
  workspace(Workspace::einn);
  assert(!item(kQuickView)->IsMarked() && !item(kSwitch)->IsEnabled());
  assert(!item(kCopy)->IsEnabled() && !item(kMove)->IsEnabled());
  workspace(Workspace::askr);
  assert(item(kSwitch)->IsEnabled() && !item(kSame)->IsEnabled());
  assert(std::string(item(kSwitch)->Label()) == "Switch tree/list focus");
  workspace(Workspace::smidja);
  assert(item(kSwitch)->IsEnabled() && item(kSame)->IsEnabled());
  assert(std::string(item(kSwitch)->Label()) == "Switch panel");
  auto *left = dynamic_cast<Listi::PanelTools *>(window->FindView("first panel tools"));
  auto *right = dynamic_cast<Listi::PanelTools *>(window->FindView("second panel tools"));
  assert(left && right);
  using Page = Listi::PanelPage;
  auto *original = left->Page(Page::Files);
  auto *leftPane = static_cast<Pane *>(original);
  auto *rightPane = static_cast<Pane *>(right->Page(Page::Files));
  const SidebarMode originalRight = rightPane->Sidebar();
  auto changeSidebar = [&](SidebarMode mode) {
    BMessage change(kSidebarMode);
    change.AddInt32("mode", static_cast<int32>(mode));
    leftPane->MessageReceived(&change);
  };
  Listi::SavedPlaces places;
  places.Add("Home shortcut", "/boot/home");
  leftPane->SetPlaces(places, true);
  changeSidebar(SidebarMode::Favorites);
  auto *sidebar = leftPane->FindView("navigation sidebar");
  auto *favorites = dynamic_cast<BListView *>(leftPane->FindView("favorite destinations"));
  assert(sidebar && favorites && !favorites->IsHidden(sidebar));
  auto *shortcut = dynamic_cast<BStringItem *>(favorites->ItemAt(0));
  assert(shortcut && std::string(shortcut->Text()) == "Home shortcut");
  changeSidebar(SidebarMode::Shelf);
  auto *shelf = leftPane->FindView("sidebar shelf");
  assert(shelf && !shelf->IsHidden(sidebar) && favorites->IsHidden(sidebar));
  changeSidebar(SidebarMode::Hidden);
  assert(sidebar->IsHidden(sidebar) && leftPane->SidebarCanBeRestored());
  BMessage restore(kRestoreSidebar);
  leftPane->MessageReceived(&restore);
  assert(leftPane->Sidebar() == SidebarMode::Shelf && !shelf->IsHidden(sidebar));
  assert(rightPane->Sidebar() == originalRight);
  auto *tabs = left->FindView("panel tabs");
  assert(tabs && tabs->IsHidden(tabs));
  auto select = [&](Page page) {
    BMessage change(Listi::kPanelPage);
    change.AddInt32("pane", 0);
    change.AddInt32("page", static_cast<int32>(page));
    window->MessageReceived(&change);
  };
  send(kSearch);
  auto *search = left->Page(Page::Search);
  assert(search && left->Current() == Page::Search && !tabs->IsHidden(tabs));
  auto *query = dynamic_cast<BTextControl *>(search->FindView("search expression"));
  assert(query);
  query->SetText("left-only");
  select(Page::Files);
  assert(left->Page(Page::Files) == original && left->Current() == Page::Files);
  send(kSearch);
  assert(left->Page(Page::Search) == search && left->Current() == Page::Search);
  assert(std::string(query->Text()) == "left-only");
  send(kSwitch);
  send(kSearch);
  auto *rightSearch = right->Page(Page::Search);
  assert(rightSearch && rightSearch != search);
  auto *rightQuery = dynamic_cast<BTextControl *>(rightSearch->FindView("search expression"));
  assert(rightQuery && std::string(rightQuery->Text()).empty());
  rightQuery->SetText("right-only");
  assert(std::string(query->Text()) == "left-only");
  send(kSwitch);
  BMessage close(Listi::kClosePanelPage);
  close.AddInt32("pane", 0);
  window->MessageReceived(&close);
  assert(!left->Page(Page::Search) && left->Current() == Page::Files);
  assert(tabs->IsHidden(tabs));
  send(kQuickView);
  assert(right->Current() == Page::Preview && left->Page(Page::Files) == original);
  send(kQuickView);
  assert(right->Current() == Page::Files && right->Page(Page::Search) == rightSearch);
  assert(std::string(rightQuery->Text()) == "right-only");
  // A tool may close immediately, before the result list's first draw.
  for (int i = 0; i < 12; ++i) {
    send(kSearch);
    window->MessageReceived(&close);
    assert(!left->Page(Page::Search));
  }
  // The Inspector's Edit action must not point at a selection on a hidden Files tab.
  char temp[] = "/boot/home/Listi-inspector-routing-XXXXXX";
  assert(mkdtemp(temp));
  const std::string file = std::string(temp) + "/visible.txt";
  assert(std::ofstream(file) << "visible selection");
  // Create both folders before navigation installs its monitor. Only the
  // explicit navigation below should invalidate the pattern preview.
  const auto otherFolder = std::string(temp) + "/elsewhere";
  assert(mkdir(otherFolder.c_str(), 0700) == 0);
  const auto marker = otherFolder + "/another.zip";
  const char emptyZip[22] = {'P', 'K', 5, 6};
  auto writeArchive = [&] {
    assert(std::ofstream(marker, std::ios::binary).write(emptyZip, sizeof(emptyZip)));
  };
  writeArchive();
  auto *pane = static_cast<Pane *>(original);
  pane->Navigate(temp);
  window->Show();
  assert(window->Lock());
  auto waitFor = [&](auto condition) {
    for (int i = 0; i < 200 && !condition(); ++i) {
      window->Unlock();
      snooze(25000);
      assert(window->Lock());
    }
    assert(condition());
  };
  waitFor([&] { return !pane->IsScanning(); });
  assert(pane->MatchPattern("visible.txt", true) == 1);
  // Haiku can resolve /tmp through an alias when loading the folder.
  const auto selected = pane->Selected();
  assert(selected == std::vector<std::string>{
                         std::string(pane->Path()) + "/visible.txt"});
  // Use the actual File view menu in each workspace. A recursive snapshot
  // must replace only the active file list, never open another window.
  for (auto mode : {Workspace::einn, Workspace::askr, Workspace::tviskipt,
                    Workspace::smidja}) {
    workspace(mode);
    for (int index = 0; index < (mode == Workspace::tviskipt ? 2 : 1); ++index) {
      if (index == 1) {
        send(kSwitch);
        rightPane->Navigate(temp);
        waitFor([&] { return !rightPane->IsScanning(); });
      }
      auto *active = index == 0 ? left : right;
      auto *other = index == 0 ? right : left;
      const auto otherPage = other->Current();
      const auto windows = app.CountWindows();
      auto *flat = menu->FindItem("Flat View");
      auto *modeField = dynamic_cast<BMenuField *>(active->FindView("display mode"));
      assert(modeField && modeField->Menu()->FindItem("Flat View"));
      if (index == 1)
        flat = modeField->Menu()->FindItem("Flat View");
      assert(flat && flat->IsEnabled() && static_cast<BInvoker *>(flat)->Invoke() == B_OK);
      waitFor([&] {
        auto *rows = dynamic_cast<BColumnListView *>(active->FindView("flat results"));
        return rows && !rows->IsHidden() && rows->CountRows() == 2;
      });
      assert(app.CountWindows() == windows && other->Current() == otherPage);
      assert(!other->FindView("flat results") && active->Files().IsHidden());
      if (mode == Workspace::tviskipt && index == 0) {
        auto *split = dynamic_cast<Listi::WorkspaceSplit *>(active->Parent());
        assert(split);
        const bool inspectorWasVisible = split->InspectorVisible();
        if (!inspectorWasVisible)
          send(kPreviewToggle);
        assert(split->InspectorVisible());
        auto *filter = dynamic_cast<BTextControl *>(active->FindView("flat filter"));
        assert(filter && filter->ModificationMessage());
        filter->SetText("another");
        assert(window->PostMessage(filter->ModificationMessage(),
                                   filter->Target()) == B_OK);
        auto *rows = static_cast<BColumnListView *>(active->FindView("flat results"));
        waitFor([&] { return rows->CountRows() == 1; });
        rows->SetFocusRow(rows->RowAt(0), true);
        assert(rows->CurrentSelection());
        const auto nestedPath = std::string(active->Files().Path()) + "/elsewhere/another.zip";
        waitFor([&] { return split->Inspector().File() == nestedPath; });

        char copyTarget[] = "/boot/home/Listi-flat-copy-XXXXXX";
        assert(mkdtemp(copyTarget));
        const std::string priorTarget = rightPane->Path();
        rightPane->Navigate(copyTarget);
        waitFor([&] { return !rightPane->IsScanning(); });
        assert(static_cast<BInvoker *>(item(kCopy))->Invoke() == B_OK);
        const std::string copy = std::string(copyTarget) + "/another.zip";
        waitFor([&] {
          char bytes[sizeof(emptyZip)];
          std::ifstream file(copy, std::ios::binary);
          return file.read(bytes, sizeof(bytes)) && !memcmp(bytes, emptyZip, sizeof(bytes)) &&
                 file.peek() == std::ifstream::traits_type::eof();
        });
        assert(access((std::string(copyTarget) + "/visible.txt").c_str(), F_OK) != 0);
        rightPane->Navigate(priorTarget.c_str());
        waitFor([&] { return !rightPane->IsScanning(); });
        assert(unlink(copy.c_str()) == 0 && rmdir(copyTarget) == 0);

        const auto newFile = otherFolder + "/another-new.txt";
        assert(std::ofstream(newFile) << "new since the snapshot");
        assert(static_cast<BInvoker *>(item(kRefresh))->Invoke() == B_OK);
        auto *status = static_cast<BStringView *>(active->FindView("flat status"));
        assert(status);
        waitFor([&] {
          return rows->CountRows() == 2 &&
                 std::string(status->Text()) == "2 of 3 files shown. Scan complete.";
        });
        assert(std::string(filter->Text()) == "another");
        assert(!rows->CurrentSelection() && !rows->FocusRow());
        waitFor([&] { return split->Inspector().File().empty(); });
        if (!inspectorWasVisible)
          send(kPreviewToggle);
        assert(unlink(newFile.c_str()) == 0);
      }
      auto *modeFieldFlat = dynamic_cast<BMenuField *>(active->FindView("flat display mode"));
      assert(modeFieldFlat && modeFieldFlat->Menu()->FindItem("Flat View")->IsMarked());
      auto *restore = modeFieldFlat->Menu()->ItemAt(active->Files().CurrentView());
      assert(static_cast<BInvoker *>(restore)->Invoke() == B_OK);
      waitFor([&] { return active->Current() == Page::Files; });
      assert(active->Current() == Page::Files && !active->Files().IsHidden());
      assert(!active->FindView("flat results"));
      // Transfer notifications can reload the underlying pane asynchronously.
      waitFor([&] { return !pane->IsScanning(); });
      assert(pane->Selected() == selected);
      if (index == 1)
        send(kSwitch);
    }
  }
  select(Page::Files);
  send(kPreviewOtherPanel);
  auto *inspect = right->Page(Page::Preview);
  assert(inspect && right->Current() == Page::Preview);
  send(kPreviewOtherPanel);
  assert(right->Page(Page::Preview) == inspect &&
         right->Current() == Page::Preview);
  auto textVisible = [&] {
    auto *scroll = inspect->FindView("preview scroll");
    return scroll && !scroll->IsHidden();
  };
  waitFor(textVisible);
  send(kSearch);
  assert(left->Current() == Page::Search && !textVisible());
  select(Page::Files);
  waitFor(textVisible);
  BMessage focusPreview(Listi::kPanelPage);
  focusPreview.AddInt32("pane", 1);
  focusPreview.AddInt32("page", static_cast<int32>(Page::Preview));
  auto verifyClose = [&] {
    assert(!right->Page(Page::Preview) && right->Current() == Page::Files);
    assert(pane->Selected() == selected);
    auto *focus = window->CurrentFocus();
    while (focus && focus != pane)
      focus = focus->Parent();
    assert(focus == pane);
  };
  for (uint32 closeCommand : {uint32(kInlineCancel), kQuickView}) {
    window->MessageReceived(&focusPreview);
    send(closeCommand);
    verifyClose();
    send(kPreviewOtherPanel);
    assert(right->Current() == Page::Preview);
  }
  // A tab change away from an embedded draft must honour the editor's answer.
  send(kEdit);
  auto *editor = dynamic_cast<BTextView *>(right->FindView("editor text"));
  assert(editor && std::string(editor->Text()) == "visible selection");
  editor->SetText("unsaved draft");
  BMessage switchTab(Listi::kPanelPage);
  switchTab.AddInt32("pane", 1);
  switchTab.AddInt32("page", static_cast<int32>(Page::Search));
  for (const char *answer : {"Cancel", "Discard"}) {
    assert(window->PostMessage(&switchTab) == B_OK);
    window->Unlock();
    BAlert *prompt = nullptr;
    for (int i = 0; i < 250 && !prompt; ++i) {
      for (int32 j = 0; j < app.CountWindows() && !prompt; ++j)
        prompt = dynamic_cast<BAlert *>(app.WindowAt(j));
      if (!prompt)
        snooze(20000);
    }
    assert(prompt && prompt->Lock());
    auto *button = prompt->ButtonAt(std::string(answer) == "Cancel" ? 1 : 0);
    assert(button && std::string(button->Label()) == answer);
    assert(button->Invoke() == B_OK);
    prompt->Unlock();
    BMessage queryTitle(B_GET_PROPERTY), reply;
    queryTitle.AddSpecifier("Title");
    assert(BMessenger(window).SendMessage(&queryTitle, &reply, 2000000, 2000000) == B_OK);
    assert(window->Lock());
    if (std::string(answer) == "Cancel") {
      assert(right->Current() == Page::Preview && right->FindView("editor text") == editor);
      assert(std::string(editor->Text()) == "unsaved draft");
    }
  }
  assert(right->Current() == Page::Search && !right->FindView("editor text"));
  assert(pane->Selected() == selected && std::string(rightQuery->Text()) == "right-only");
  std::string onDisk;
  assert(std::getline(std::ifstream(file), onDisk) && onDisk == "visible selection");
  BMessage closePreview(Listi::kClosePanelPage);
  closePreview.AddInt32("pane", 1);
  window->MessageReceived(&focusPreview);
  window->MessageReceived(&closePreview);
  verifyClose();

  select(Page::Files);
  workspace(Workspace::einn);
  waitFor([&] { return !pane->IsScanning(); });
  assert(pane->MatchPattern("visible.txt", true) == 1);
  send(kPreviewOtherPanel);
  assert(item(kQuickView)->IsMarked());
  send(kPreviewOtherPanel);
  assert(item(kQuickView)->IsMarked());
  send(kCloseQuickView);
  send(kCloseQuickView);
  assert(!item(kQuickView)->IsMarked());
  assert(pane->Selected() == selected);
  waitFor([&] { return !rightPane->IsScanning(); });
  const std::string sourcePath = pane->Path();
  const std::string destinationPath = rightPane->Path();
  BMessenger windowScript(window);
  window->Unlock();
  auto property = [&](uint32 command, const char *name, status_t expected,
                      const char *data = nullptr) {
    BMessage request(command), reply;
    assert(request.AddSpecifier(name) == B_OK);
    if (data)
      assert(request.AddString("data", data) == B_OK);
    assert(windowScript.SendMessage(&request, &reply, 1000000, 1000000) == B_OK);
    assert(reply.what == B_REPLY && reply.GetInt32("error", B_ERROR) == expected);
    return reply;
  };
  assert(std::string(property(B_GET_PROPERTY, "Workspace", B_OK)
                         .GetString("result", "")) == WorkspaceName(Workspace::einn));
  assert(std::string(property(B_GET_PROPERTY, "SourcePath", B_OK)
                         .GetString("result", "")) == sourcePath);
  assert(std::string(property(B_GET_PROPERTY, "DestinationPath", B_OK)
                         .GetString("result", "")) == destinationPath);
  assert(property(B_GET_PROPERTY, "PathReady", B_OK).GetBool("result", false));
  assert(!property(B_GET_PROPERTY, "Busy", B_OK).GetBool("result", true));
  property(B_SET_PROPERTY, "Busy", B_NOT_ALLOWED);
  property(B_SET_PROPERTY, "SourcePath", B_BAD_VALUE);
  property(B_SET_PROPERTY, "DestinationPath", B_OK, temp);
  assert(window->Lock());
  waitFor([&] { return !rightPane->IsScanning(); });
  assert(sourcePath == rightPane->Path());
  // A delayed confirmation must use the folder where the question was opened.
  const auto elsewhere = sourcePath + "/elsewhere";
  Listi::SavedPlaces bookmarks;
  bookmarks.Add("Original", sourcePath);
  bookmarks.Add("Elsewhere", elsewhere);
  pane->SetPlaces(bookmarks, true);
  auto *go = dynamic_cast<BMenuField *>(pane->FindView("go"));
  assert(go);
  auto *firstPlace = go->Menu()->FindItem("Original");
  auto *nextPlace = go->Menu()->FindItem("Elsewhere");
  assert(firstPlace && firstPlace->IsMarked() && nextPlace && !nextPlace->IsMarked());
  // A folder change updates selection without replacing the command object.
  assert(firstPlace->Message()->AddBool("identity probe", true) == B_OK);
  auto promptButton = [&](const char *label) -> BButton * {
    auto *question = window->FindView("question");
    if (!question)
      return nullptr;
    auto *prompt = question->Parent();
    auto *buttons = prompt->ChildAt(prompt->CountChildren() - 1);
    for (int32 i = 0; i < buttons->CountChildren(); ++i) {
      auto *button = dynamic_cast<BButton *>(buttons->ChildAt(i));
      if (button && std::string(button->Label()) == label)
        return button;
    }
    return nullptr;
  };

  // A pattern preview must not authorize replacing a new folder's selection,
  // even when the pattern still matches entries there.
  send(kSelectPattern);
  auto *preview = promptButton("Preview");
  assert(preview && preview->Invoke() == B_OK);
  waitFor([&] { return promptButton("Select") != nullptr; });
  pane->Navigate(elsewhere.c_str());
  waitFor([&] { return !pane->IsScanning(); });
  assert(pane->MatchPattern("*", false) == 1 && pane->Selected().empty());
  auto *applyPattern = promptButton("Select");
  assert(applyPattern && applyPattern->Invoke() == B_OK);
  auto *paneStatus = dynamic_cast<BTextView *>(pane->FindView("status"));
  assert(paneStatus);
  waitFor([&] {
    return std::string(paneStatus->Text()) ==
           "Panel contents changed. Start Select by pattern again.";
  });
  assert(pane->Selected().empty());
  // The window lock keeps monitor notifications queued. Simulate a file
  // replaced after the scan but before its selected row is acted on.
  assert(pane->MatchPattern("another.zip", true) == 1);
  auto archiveDialog = [&](const char *title) -> BWindow * {
    for (int32 i = 0; i < app.CountWindows(); ++i) {
      auto *candidate = app.WindowAt(i);
      if (candidate && candidate->Title() && !std::strcmp(candidate->Title(), title))
        return candidate;
    }
    return nullptr;
  };
  send(kCreateArchive);
  auto *create = archiveDialog("Listi: Create archive");
  assert(create && create->Lock());
  create->Quit();
  {
    Listi::ArchiveActions actions;
    actions.ShowAddPicker(*pane, BMessenger(window));
  }
  send(kDelete);
  waitFor([&] { return promptButton("Move to Trash"); });
  send(kInlineCancel);
  assert(!promptButton("Move to Trash"));
  const auto originalMarker = marker + ".original";
  assert(rename(marker.c_str(), originalMarker.c_str()) == 0);
  writeArchive();
  // The fixture's BApplication does not dispatch service errors; independently
  // check that no earlier question can block the following commands.
  assert(!window->FindView("question"));
  send(kDelete);
  assert(!promptButton("Move to Trash"));
  assert(!window->FindView("question"));
  send(kDuplicate);
  assert(!promptButton("Duplicate"));
  assert(!window->FindView("question"));
  send(kCreateArchive);
  assert(!archiveDialog("Listi: Create archive"));
  send(kExtractArchive);
  assert(!archiveDialog("Listi: Extract archive"));
  Listi::ArchiveActions actions;
  auto refusesReplacement = [&](auto open) {
    try {
      open();
    } catch (const std::runtime_error &error) {
      return std::string(error.what()).find("selected entry was replaced") !=
             std::string::npos;
    }
    return false;
  };
  for (bool extract : {false, true})
    assert(refusesReplacement([&] {
      actions.ShowJobWindow(extract, *pane, *rightPane, BMessenger(window));
    }));
  assert(refusesReplacement([&] {
    actions.ShowAddPicker(*pane, BMessenger(window));
  }));
  assert(access(originalMarker.c_str(), F_OK) == 0);
  assert(unlink(originalMarker.c_str()) == 0);
  assert(unlink(marker.c_str()) == 0);
  pane->Navigate(sourcePath.c_str());
  waitFor([&] { return !pane->IsScanning(); });
  firstPlace = go->Menu()->FindItem("Original");
  assert(firstPlace && firstPlace->IsMarked());

  send(kMkdir);
  auto *confirm = promptButton("Create folder");
  assert(confirm);
  pane->Navigate(elsewhere.c_str());
  waitFor([&] { return !pane->IsScanning(); });
  assert(elsewhere == pane->Path());
  firstPlace = go->Menu()->FindItem("Original");
  nextPlace = go->Menu()->FindItem("Elsewhere");
  assert(firstPlace && firstPlace->Message()->GetBool("identity probe", false));
  assert(!firstPlace->IsMarked() && nextPlace && nextPlace->IsMarked());
  assert(favorites->CurrentSelection() == 1);
  assert(confirm->Invoke() == B_OK);
  const auto created = sourcePath + "/New folder";
  waitFor([&] { return access(created.c_str(), F_OK) == 0; });
  assert(access((elsewhere + "/New folder").c_str(), F_OK) != 0);
  // Run last: saving and deleting files triggers directory-monitor reloads.
  // Explicit mode only: this interoperability test uses the system clipboard.
  if (argc == 2 && std::string(argv[1]) == "--clipboard") {
    assert(be_clipboard->Lock());
    BMessage savedClipboard(*be_clipboard->Data());
    be_clipboard->Unlock();
    Listi::PutPathsAsText({"first clipboard text"});
    send(kClipboardPaste);
    auto *save = promptButton("Save");
    assert(save);
    // Close the question under the window lock, leaving its reply queued.
    // A second Paste must not swap the first reply's data or reserve a Cut.
    BMessage accept(*save->Message());
    window->FindView("question")->Parent()->MessageReceived(&accept);
    assert(!promptButton("Save"));
    Listi::PutPathsAsText({"second clipboard text"});
    send(kClipboardPaste);
    assert(!promptButton("Save"));
    const auto clipboardFile = std::string(pane->Path()) + "/Clipboard.txt";
    waitFor([&] { return access(clipboardFile.c_str(), F_OK) == 0; });
    std::string contents;
    assert(std::getline(std::ifstream(clipboardFile), contents));
    assert(contents == "first clipboard text");
    assert(unlink(clipboardFile.c_str()) == 0);
    send(kClipboardPaste);
    assert(promptButton("Save"));
    send(kInlineCancel);
    assert(!promptButton("Save"));
    writeArchive();
    const auto archiveRoot = "archive://" + marker + "!/";
    pane->Navigate(archiveRoot.c_str());
    waitFor([&] { return !pane->IsScanning(); });
    Listi::PutFileClipboard({file}, true);
    send(kClipboardPaste);
    auto *move = promptButton("Move entries");
    assert(move);
    Listi::InterruptedVolumes().Block(marker);
    assert(move->Invoke() == B_OK);
    waitFor([&] {
      assert(be_clipboard->Lock());
      const bool released = !be_clipboard->Data()->HasString("listi:paste-lease");
      be_clipboard->Unlock();
      return released;
    });
    assert(access(file.c_str(), F_OK) == 0);
    pane->Navigate(elsewhere.c_str());
    waitFor([&] { return !pane->IsScanning(); });
    Listi::PutPathsAsText({"retry"});
    send(kClipboardPaste);
    assert(promptButton("Save"));
    send(kInlineCancel);
    assert(unlink(marker.c_str()) == 0);
    assert(be_clipboard->Lock());
    *be_clipboard->Data() = savedClipboard;
    assert(be_clipboard->Commit(true) == B_OK);
    be_clipboard->Unlock();
    puts("Clipboard: queued confirmation retained original bytes; next paste available");
  }

  // Optional native fixture on a disposable ext4 volume, not a mock.
  if (const char *ext4 = getenv("LISTI_TEST_EXT4_DIR")) {
    std::string fixture = std::string(ext4) + "/Listi-trash-XXXXXX";
    assert(mkdtemp(fixture.data()));
    const auto torrent = fixture + "/test.torrent";
    const auto original = fixture + "/original";
    auto writeTorrent = [&] { assert(std::ofstream(torrent) << "fixture"); };
    writeTorrent();
    Listi::Job job;
    job.action = Listi::Action::Trash;
    job.sources = {torrent};
    const auto direct = Listi::Run(job, {});
    assert(direct.completed == 0 && direct.errors.size() == 1);
    assert(direct.errors.front().find("Haiku has no Trash on this ext4 volume") !=
           std::string::npos);
    assert(access(torrent.c_str(), F_OK) == 0);

    pane->Navigate(fixture.c_str());
    waitFor([&] { return !pane->IsScanning(); });
    auto ask = [&] {
      assert(pane->MatchPattern("*.torrent", true) == 1);
      send(kDelete);
      waitFor([&] { return promptButton("Delete permanently"); });
      const auto *question = dynamic_cast<BTextView *>(window->FindView("question"));
      assert(question && std::string(question->Text()).find("No Trash on ext4 volumes") !=
                         std::string::npos);
      assert(promptButton("Cancel") && !promptButton("Move to Trash"));
    };
    ask();
    send(kInlineCancel);
    assert(!promptButton("Delete permanently"));
    assert(access(torrent.c_str(), F_OK) == 0);

    ask();
    assert(rename(torrent.c_str(), original.c_str()) == 0);
    writeTorrent();
    assert(promptButton("Delete permanently")->Invoke() == B_OK);
    waitFor([&] { return !promptButton("Delete permanently"); });
    // The old inode and the new occupant must both survive this confirmation.
    pane->Reload();
    waitFor([&] { return !pane->IsScanning(); });
    assert(access(torrent.c_str(), F_OK) == 0 && access(original.c_str(), F_OK) == 0);

    ask();
    assert(promptButton("Delete permanently")->Invoke() == B_OK);
    waitFor([&] { return access(torrent.c_str(), F_OK) != 0; });
    assert(access(original.c_str(), F_OK) == 0);
    pane->Navigate(sourcePath.c_str());
    waitFor([&] { return !pane->IsScanning(); });
    assert(unlink(original.c_str()) == 0 && rmdir(fixture.c_str()) == 0);
    puts("ext4 Trash: no silent fallback, Cancel, stale identity and explicit permanent delete OK");
  }

  // Simulate a lost mount with normal directories. Neither a stale pane
  // nor a separate pane may start reading another folder below that mount.
  pane->Navigate(elsewhere.c_str());
  rightPane->Navigate(sourcePath.c_str());
  waitFor([&] { return !pane->IsScanning() && !rightPane->IsScanning(); });
  assert(pane->LocationValid() && rightPane->LocationValid());
  // An ordinary unmount is not an I/O quarantine. Invalidate stale rows
  // immediately, including when no Listi file operation has failed.
  assert(rightPane->MatchPattern("visible.txt", true) == 1);
  assert(rightPane->Selected().size() == 1);
  const auto mountedDevice = rightPane->FolderIdentity().first;
  assert(mountedDevice >= 0);
  BMessage unmounted(B_NODE_MONITOR);
  unmounted.AddInt32("opcode", B_DEVICE_UNMOUNTED);
  unmounted.AddInt32("device", mountedDevice + 1);
  rightPane->MessageReceived(&unmounted);
  assert(sourcePath == rightPane->Path() && rightPane->LocationValid());
  unmounted.ReplaceInt32("device", mountedDevice);
  rightPane->MessageReceived(&unmounted);
  assert(std::string(rightPane->Path()) == "@disks" && rightPane->Selected().empty());
  waitFor([&] { return !rightPane->IsScanning(); });
  rightPane->Navigate(sourcePath.c_str());
  waitFor([&] { return !rightPane->IsScanning(); });
  assert(sourcePath == rightPane->Path() && rightPane->LocationValid());
  puts("Unmount notification invalidates only the affected pane without quarantining a clean mount");

  Listi::InterruptedVolumes().Block(pane->Path());
  BMessage volumeUnavailable(kVolumeUnavailable);
  window->MessageReceived(&volumeUnavailable);
  assert(!pane->LocationValid() && !pane->IsScanning() && pane->Selected().empty());
  waitFor([&] { return !rightPane->IsScanning(); });
  assert(rightPane->LocationValid());
  pane->Reload();
  assert(!pane->IsScanning());
  rightPane->Navigate((elsewhere + "/subfolder").c_str());
  assert(!rightPane->IsScanning() && sourcePath == rightPane->Path());
  pane->Navigate(sourcePath.c_str());
  waitFor([&] { return !pane->IsScanning(); });
  assert(pane->LocationValid() && sourcePath == pane->Path());

  assert(rmdir(created.c_str()) == 0 && rmdir(elsewhere.c_str()) == 0);
  assert(unlink(file.c_str()) == 0 && rmdir(temp) == 0);
  auto *split = dynamic_cast<Listi::WorkspaceSplit *>(left->Parent());
  assert(split);
  CheckWorkspaceSplit(*split, left);
  window->Quit();
  CheckFloatingInspector(app);
  CheckWindowCLIQuit(app);
  puts("Main window: menu shortcuts and workspace routing OK");
}
