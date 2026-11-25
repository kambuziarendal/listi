// SPDX-License-Identifier: MIT
#include "Common.h"
#include "FolderTabs.h"
#include "MainWindow.h"
#include "WorkingSets.h"
#include "PanelTools.h"
#include "Pane.h"
#include "SavedPlaces.h"
#include "SavedSearches.h"
#include <Application.h>
#include <MenuBar.h>
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
  auto open = [&](BrowserRequest request) {
    auto *child = CreateMainWindow(prefs, {}, {}, false, {}, false, std::move(request));
    child->Show();
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
  Command(*window, kTabDetach);
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
  window->Unlock();
  detached->Unlock();
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
  standalone->Quit();
  fs::remove_all(root);
}
