// SPDX-License-Identifier: MIT
#include "Common.h"
#include "FolderTabs.h"
#include "MainWindow.h"
#include "WorkingSets.h"
#include "PanelTools.h"
#include "Pane.h"
#include "SavedPlaces.h"
#include "SavedSearches.h"
#include "TerminalPanel.h"
#include <Application.h>
#include <Button.h>
#include <MenuBar.h>
#include <TextView.h>
#include <Window.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace Listi;
namespace fs = std::filesystem;
namespace {
void Check(bool ok, const char *name) {
  std::cout << (ok ? "PASS " : "FAIL ") << name << std::endl;
  if (!ok) std::exit(1);
}
PanelTools &Panel(BWindow &window, int i = 0) {
  return *dynamic_cast<PanelTools *>(window.FindView(i ? "second panel tools" : "first panel tools"));
}
void Command(BWindow &window, uint32 what, int side = 0, int64 tab = -1) {
  BMessage m(what);
  m.AddInt32("pane", side);
  if (tab != -1)
    m.AddInt64("tab", tab);
  window.MessageReceived(&m);
}
void Ready(BWindow &window) {
  window.Unlock();
  const auto until = system_time() + 8000000;
  do {
    snooze(20000);
    window.Lock();
    if (!Panel(window).Files().IsScanning() && !Panel(window, 1).Files().IsScanning())
      return;
    window.Unlock();
  } while (system_time() < until);
  Check(false, "file panes complete navigation in time");
}

template <typename Condition>
void WaitFor(BWindow &window, Condition ready, const char *description) {
  const auto until = system_time() + 8000000;
  while (!ready()) {
    if (system_time() >= until)
      Check(false, description);
    window.Unlock();
    snooze(20000);
    window.Lock();
  }
  Check(true, description);
}

std::string ReadText(const fs::path &path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), {}};
}

bool Enter(TerminalPanel &terminal, const char *command) {
  BMessage text(B_KEY_DOWN), enter(B_KEY_DOWN);
  text.AddString("bytes", command);
  enter.AddString("bytes", "\n");
  return terminal.HandleKey(&text) && terminal.HandleKey(&enter);
}

void CheckManyTabs(BWindow &window, const fs::path &left, const fs::path &right) {
  auto &panel = Panel(window);
  auto &bar = panel.FolderBar();
  auto &files = panel.Files();
  constexpr int count = 128;
  BMessage archive;
  archive.AddInt32("active", 0);
  for (int index = 0; index < count; ++index) {
    BMessage item;
    item.AddString("alias", ("Folder " + std::to_string(index)).c_str());
    item.AddString("location", index % 2 == 0 ? left.c_str() : right.c_str());
    archive.AddMessage("tab", &item);
  }
  bar.RestoreTabs(archive);
  Ready(window);
  const auto first = bar.ActiveId();
  Check(bar.Count() == count && files.Path() == left.string(),
        "restore 128 suspended tabs without opening more windows");
  Command(window, kTabPrevious);
  Ready(window);
  const auto last = bar.ActiveId();
  Check(bar.Index() == count - 1 && last != first && files.Path() == right.string(),
        "switch from the first tab to the end of the overflowing strip");
  for (int index = 0; index < 8; ++index) {
    Command(window, kTabChoose, 0, first);
    Command(window, kTabChoose, 0, last);
  }
  Ready(window);
  files.MatchPattern("two", true);
  Command(window, kTabClose, 0, first);
  Check(bar.Count() == count - 1 && bar.ActiveId() == last &&
        bar.Index() == count - 2 && !files.IsScanning() &&
        files.SelectedPath() == (right/"two").string(),
        "rapid switches and a background close preserve the live tab and selection");
  Command(window, kTabReopen);
  Ready(window);
  Check(bar.Count() == count && bar.ActiveId() == first &&
        bar.Snapshot().alias == "Folder 0" && files.Path() == left.string(),
        "reopen the closed overflow tab by its original identity");
}
}
int main() {
  BApplication app("application/x-vnd.Listi-FolderTabsTest");
  char pattern[] = "/tmp/ListiTabTest-XXXXXX";
  Check(mkdtemp(pattern), "temporary folder created");
  fs::path root = fs::canonical(pattern);
  auto a = root/"alpha", b = root/"beta", c = root/"target";
  fs::create_directories(a); fs::create_directory(b); fs::create_directory(c);
  std::ofstream(a/"one") << "tab payload";
  std::ofstream(b/"two") << "another file";
  Preferences prefs;
  std::optional<BrowserRequest> pending;
  bool accepting = false;
  auto open = [&](BrowserRequest request) {
    if (!accepting)
      return false;
    pending = std::move(request);
    return true;
  };
  auto *window = CreateMainWindow(prefs, {}, {}, false, {}, false, {}, open);
  window->Show();
  window->Lock();
  auto &panel = Panel(*window);
  auto &files = panel.Files();
  auto &bar = panel.FolderBar();
  auto &target = Panel(*window, 1).Files();
  const auto fullFrame = window->Frame();
  auto switchTo = [&](Workspace workspace) {
    BMessage m(kWorkspace);
    m.AddInt32("workspace", static_cast<int32>(workspace));
    window->MessageReceived(&m);
  };
  switchTo(Workspace::einn);
  Check(window->Frame().Width() < fullFrame.Width(), "switching to Einn is compact");
  window->ResizeBy(40, 30);
  const auto resized = window->Frame();
  switchTo(Workspace::tviskipt);
  Check(window->Frame() == fullFrame, "leaving Einn restores the previous frame");
  switchTo(Workspace::einn);
  Check(window->Frame() == resized, "returning to Einn remembers a manual resize");
  switchTo(Workspace::tviskipt);
  files.Navigate(a.c_str());
  target.Navigate(c.c_str());
  Ready(*window);
  window->Layout(true);
  const float leftWidth = files.Bounds().Width(), rightWidth = target.Bounds().Width();
  auto checkWidths = [&](const char *description) {
    window->Layout(true);
    Check(files.Bounds().Width() == leftWidth && target.Bounds().Width() == rightWidth,
          description);
  };
  Check(bar.Count() == 1 && bar.IsHidden(&bar), "single tab starts hidden");
  Command(*window, kTabAlways);
  Check(!bar.IsHidden(&bar), "always-show makes a single tab visible");
  Command(*window, kTabAlways);
  files.MatchPattern("one", true);
  const auto first = bar.ActiveId();
  Command(*window, kTabDuplicate);
  Ready(*window);
  const auto copy = bar.ActiveId();
  Check(first != copy && bar.Count() == 2 && files.SelectedPath() == (a/"one").string(),
        "duplicate has an independent ID and restores selection");
  checkWidths("opening the tab row preserves both file pane widths");
  Check(bar.Bounds().Width() == leftWidth,
        "the strip fills its pane without centering its content in a narrower view");
  files.Navigate(b.c_str());
  Ready(*window);
  files.MatchPattern("two", true);
  bar.Rename("Work");
  Command(*window, kTabLeft);
  Check(bar.Index() == 0 && bar.ActiveId() == copy && files.SelectedPath() == (b/"two").string(),
        "reorder does not navigate or discard selection");
  Command(*window, kTabNext);
  Ready(*window);
  auto firstState = files.CaptureNavigation();
  Check(bar.ActiveId() == first && firstState.location == a.string() &&
        firstState.history.At(firstState.history.Position()) == a.string() &&
        files.SelectedPath() == (a/"one").string(), "independent history and selection");
  checkWidths("tab navigation preserves the split");
  Command(*window, kCopy);
  Command(*window, kTabPrevious);
  Ready(*window);
  Command(*window, kTabClose);
  Ready(*window);
  Check(bar.Count() == 1 && bar.CanReopen() && bar.ActiveId() == first,
        "closing leaves sibling intact");
  checkWidths("closing a tab preserves the split");
  Command(*window, kTabReopen);
  Ready(*window);
  Check(bar.ActiveId() == copy && bar.Snapshot().alias == "Work" &&
        files.SelectedPath() == (b/"two").string(), "reopening preserves name, ID and selected file");
  const auto savedIndex = bar.Index();
  window->Unlock();
  BMessage capture(kCaptureWorkingWindow), snapshot;
  Check(BMessenger(window).SendMessage(&capture, &snapshot, 1000000, 1000000) == B_OK &&
        snapshot.what == kCaptureWorkingWindow, "capture working set window");
  BrowserRequest restore;
  restore.localLayout = true;
  restore.workingSet = snapshot;
  auto *restored = CreateMainWindow(prefs, {}, {}, false, {}, false, std::move(restore));
  restored->Show();
  restored->Lock();
  Ready(*restored);
  auto &restoredTabs = Panel(*restored).FolderBar();
  Check(restoredTabs.Count() == 2 && restoredTabs.Index() == savedIndex &&
        restoredTabs.ActiveId() != copy && restoredTabs.Snapshot().alias == "Work" &&
        restoredTabs.Snapshot().state.location == b.string() &&
        Panel(*restored, 1).Files().CaptureNavigation().location == c.string(),
        "working set restores both panels and active named tab with fresh identity");
  restored->Quit();
  window->Lock();
  std::ofstream(c/"two") << "existing destination";
  // Hold a real file operation at its conflict prompt, not a timing-sensitive
  // large copy. The shell also waits until we send input after detaching.
  Command(*window, kCopy);
  auto *replace = dynamic_cast<BButton *>(window->FindView("Replace"));
  Check(replace, "original window exposes copy conflict controls");
  WaitFor(*window, [&] { return !replace->IsHidden(); },
          "copy waits for a conflict decision before detaching");
  Command(*window, Listi::kRightCLI);
  auto *rightCLI = dynamic_cast<TerminalPanel *>(window->FindView(TerminalName(1)));
  Check(rightCLI && rightCLI->Active(), "start independent opposite CLI");
  Command(*window, Listi::kLeftCLI);
  auto *terminal = dynamic_cast<TerminalPanel *>(window->FindView(TerminalName(0)));
  Check(terminal && terminal->Active() && terminal->StartFolder() == b.string(),
        "start docked CLI in the tab to detach");
  const auto session = terminal->SessionId();
  Check(Enter(*terminal, "token=live; printf ready > ../cli-ready; read answer; "
                         "printf '%s:%s' \"$token\" \"$answer\" > ../cli-answer"),
        "send command that waits for input in the original window");
  WaitFor(*window, [&] { return ReadText(root/"cli-ready") == "ready"; },
          "CLI command has started before detaching");
  Command(*window, kTabDetach);
  Check(bar.ActiveId() == copy && terminal->Active() && !pending,
        "a refused window-open request leaves the tab and running shell in place");
  accepting = true;
  Command(*window, kTabDetach);
  Check(pending && pending->cliPlacement == terminal->Placement(),
        "accepted detach captures the CLI placement before releasing the owner");
  window->Unlock();
  auto *opened = CreateMainWindow(prefs, {}, {}, false, {}, false, std::move(*pending));
  opened->Show();
  pending.reset();
  window->Lock();
  Ready(*window);
  Check(bar.Count() == 1 && bar.ActiveId() == first, "detach leaves original tab active");
  BWindow *detached = nullptr;
  for (int i = 0; i < be_app->CountWindows(); ++i) {
    auto *candidate = be_app->WindowAt(i);
    if (candidate != window && candidate && candidate->Lock()) {
      if (candidate->FindView("first panel tools")) { detached = candidate; break; }
      candidate->Unlock();
    }
  }
  Check(detached, "detached native browser opened");
  Check(detached->Bounds().Width() < window->Bounds().Width() &&
        detached->Bounds().Height() < window->Bounds().Height(),
        "detached tab opens a compact Einn window");
  Ready(*detached);
  auto &moved = Panel(*detached);
  auto value = moved.FolderBar().Snapshot();
  Check(value.id == copy && value.alias == "Work" && value.state.location == b.string() &&
        moved.Files().IsEinn() && moved.Files().SelectedPath() == (b/"two").string(),
        "detached Einn retains original tab state");
  auto *status = dynamic_cast<BTextView *>(moved.Files().FindView("status"));
  Check(status && std::string(status->Text()) ==
                      "File operations continue in the original window.",
        "detached window explains where file operations stayed");
  auto *childCLI = dynamic_cast<TerminalPanel *>(detached->FindView(TerminalName(0)));
  Check(childCLI == terminal && childCLI->Active() && childCLI->SessionId() == session &&
        window->FindView(TerminalName(1)) == rightCLI && rightCLI->Active(),
        "the live terminal follows its lister without moving the opposite terminal");
  for (auto workspace : {Workspace::tviskipt, Workspace::askr, Workspace::smidja,
                         Workspace::einn}) {
    BMessage change(kWorkspace);
    change.AddInt32("workspace", static_cast<int32>(workspace));
    detached->MessageReceived(&change);
    Ready(*detached);
    BMessage query(B_GET_PROPERTY), reply;
    query.AddSpecifier("Workspace");
    const BMessenger child(detached);
    detached->Unlock();
    const auto queried = child.SendMessage(&query, &reply, 1000000, 1000000);
    detached->Lock();
    Check(queried == B_OK && std::string(reply.GetString("result", "")) ==
                                  WorkspaceName(workspace) &&
          moved.FolderBar().ActiveId() == copy &&
          moved.Files().SelectedPath() == (b/"two").string(),
          "detached workspace switch preserves the tab and selection");
  }
  Command(*detached, kInlineCancel);
  Check(!replace->IsHidden() && terminal->Active() && terminal->SessionId() == session &&
        ReadText(c/"two") == "existing destination" && !fs::exists(root/"cli-answer"),
        "cancelling in the detached window leaves the original queue and moved CLI waiting");
  Check(Enter(*childCLI, "answer"), "continue the waiting command in the detached lister");
  detached->Unlock();
  Check(replace->Invoke() == B_OK, "continue the file operation in the original window");
  WaitFor(*window, [&] {
    return ReadText(c/"two") == "another file" &&
           ReadText(root/"cli-answer") == "live:answer" && replace->IsHidden();
  }, "copy and transferred CLI complete with the chosen destination and input");
  BMessage closeCLI(Listi::kCloseTerminal);
  detached->Lock();
  closeCLI.AddInt64("terminal placement", childCLI->Placement());
  detached->MessageReceived(&closeCLI);
  detached->Unlock();
  closeCLI.SetInt64("terminal placement", rightCLI->Placement());
  window->MessageReceived(&closeCLI);
  window->Unlock();
  auto until = system_time() + 5000000;
  while (!fs::exists(c/"one") && system_time() < until) snooze(15000);
  std::string bytes;
  std::ifstream(c/"one") >> bytes;
  Check(bytes == "tab" && !fs::exists(b/"one"), "queued copy retained its frozen destination");
  window->Lock();
  auto &rightBar = Panel(*window, 1).FolderBar();
  const auto rightFirst = rightBar.ActiveId();
  Command(*window, kTabDuplicate, 1);
  Ready(*window);
  const auto rightSecond = rightBar.ActiveId();
  target.Navigate(b.c_str());
  Ready(*window);
  target.MatchPattern("two", true);
  Command(*window, kTabClose, 1, rightFirst);
  Check(rightBar.Count() == 1 && rightBar.Index() == 0 &&
        rightBar.ActiveId() == rightSecond && rightBar.CanReopen() &&
        target.SelectedPath() == (b/"two").string() && bar.ActiveId() == first,
        "closing a background tab in the right pane preserves both live tabs and selection");
  Command(*window, kTabClose, 1, rightSecond);
  Check(rightBar.Count() == 1 && rightBar.ActiveId() == rightSecond,
        "the last tab cannot be closed");
  Command(*window, kTabReopen, 1);
  Ready(*window);
  Check(rightBar.Count() == 2 && rightBar.ActiveId() == rightFirst &&
        target.CaptureNavigation().location == c.string(),
        "reopen restores the targeted background tab");
  Command(*window, kTabClose, 1, first);
  Check(rightBar.Count() == 2 && rightBar.ActiveId() == rightFirst,
        "a close target from another pane never falls back to the active tab");
  checkWidths("targeted closes and reopen in the right pane preserve the split");
  Check(rightBar.DescribeTab(rightSecond) == "Tab 1: beta",
        "inactive tab description includes its current index and label");
  Command(*window, kTabOpposite, 1, rightSecond);
  Ready(*window);
  Check(rightBar.ActiveId() == rightFirst && rightBar.Count() == 1 &&
        target.Path() == c.string() && bar.ActiveId() == rightSecond && bar.Count() == 2 &&
        files.SelectedPath() == (b/"two").string(),
        "opposite-side action moves the clicked background tab without replacing either live tab");
  Command(*window, kTabOpposite, 1, rightSecond);
  Check(rightBar.Count() == 1 && bar.Count() == 2,
        "a stale opposite-side target does not move a different tab");
  Command(*window, kLeftCLI);
  auto *movingCLI = dynamic_cast<TerminalPanel *>(window->FindView(TerminalName(0)));
  Check(movingCLI && movingCLI->Active(), "start a CLI for the tab moving between panes");
  const auto movingSession = movingCLI->SessionId();
  Command(*window, kRightCLI);
  Command(*window, kTabOpposite, 0, rightSecond);
  Check(bar.ActiveId() == rightSecond && rightBar.ActiveId() == rightFirst &&
        movingCLI->SessionId() == movingSession && rightCLI->Active(),
        "busy destination leaves both tabs and their CLI sessions intact");
  closeCLI.SetInt64("terminal placement", rightCLI->Placement());
  window->MessageReceived(&closeCLI);
  Command(*window, kTabChoose, 0, first);
  Command(*window, kTabOpposite, 0, rightSecond);
  Ready(*window);
  Check(bar.ActiveId() == first && rightBar.ActiveId() == rightSecond &&
        window->FindView(TerminalName(1)) == movingCLI &&
        movingCLI->Active() && movingCLI->SessionId() == movingSession,
        "moving a background tab carries its live CLI to the opposite pane");
  Command(*window, kTabDetach, 1, rightFirst);
  Check(pending && pending->detached && pending->detached->id == rightFirst &&
        !pending->cliPlacement && rightBar.ActiveId() == rightSecond && movingCLI->Active(),
        "detaching a clicked background tab never takes another tab's CLI");
  pending.reset();
  closeCLI.SetInt64("terminal placement", movingCLI->Placement());
  window->MessageReceived(&closeCLI);
  Check(window->QuitRequested(), "original may close independently");
  window->Quit();
  detached->Lock();
  Command(*detached, kTabDuplicate);
  Ready(*detached);
  Check(moved.FolderBar().Count() == 2, "surviving Einn window still accepts tab operations");
  BMessage openSearch(kOpenSearchList);
  SavedSearch search{"Saved search", {b.string(), "two", false}, false};
  search.WriteTo(openSearch);
  moved.Files().MessageReceived(&openSearch);
  Ready(*detached);
  Check(moved.Files().IsSearchList(), "working-set fixture opens a search");
  detached->Unlock();
  BMessage searchSnapshot;
  Check(BMessenger(detached).SendMessage(&capture, &searchSnapshot, 1000000, 1000000) == B_OK,
        "save working set with a search");
  BrowserRequest restoreSearch;
  restoreSearch.localLayout = true;
  restoreSearch.workingSet = searchSnapshot;
  auto *searchWindow = CreateMainWindow(prefs, {}, {}, false, {}, false, std::move(restoreSearch));
  searchWindow->Show();
  searchWindow->Lock();
  Ready(*searchWindow);
  auto &restoredSearch = Panel(*searchWindow).Files();
  auto state = restoredSearch.CaptureNavigation();
  Check(restoredSearch.IsSearchList() && restoredSearch.LocationValid() &&
        state.location.rfind("@set-query:", 0) == 0 && state.history.RecentFolders().empty(),
        "restored working-set search never enters Recent folders");
  restoredSearch.Navigate(a.c_str());
  Ready(*searchWindow);
  BMessage back(kBack);
  restoredSearch.MessageReceived(&back);
  Ready(*searchWindow);
  Check(restoredSearch.IsSearchList() && restoredSearch.LocationValid() &&
        restoredSearch.CaptureNavigation().history.RecentFolders() == std::vector<std::string>{a.string()},
        "Back reaches restored search while Recent keeps only the visited folder");
  searchWindow->Quit();
  detached->Lock();
  detached->Quit();
  prefs.workspace = Workspace::einn;
  auto *standalone = CreateMainWindow(prefs, {}, {}, false, {}, false, {});
  standalone->Show();
  standalone->Lock();
  Check(standalone->Frame().Width() < fullFrame.Width() && Panel(*standalone).Files().IsEinn(),
        "opening a standalone Einn uses the compact frame");
  Ready(*standalone);
  auto &single = Panel(*standalone);
  const auto singleId = single.FolderBar().ActiveId();
  Command(*standalone, kTabNext);
  Command(*standalone, kTabPrevious);
  Check(single.FolderBar().ActiveId() == singleId && !single.Files().IsScanning(),
        "cycling a single tab does not restart the directory scan");
  CheckManyTabs(*standalone, a, b);
  standalone->Quit();
  fs::remove_all(root);
}
