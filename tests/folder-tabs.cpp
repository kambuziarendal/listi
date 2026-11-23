// SPDX-License-Identifier: MIT
#include "Common.h"
#include "FolderTabs.h"
#include "MainWindow.h"
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
void Command(BWindow &window, uint32 what, int side = 0) {
  BMessage m(what);
  m.AddInt32("pane", side);
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
  auto *window = CreateMainWindow(prefs, {}, {}, false, {}, false);
  window->Show();
  window->Lock();
  auto &panel = Panel(*window);
  auto &files = panel.Files();
  auto &bar = panel.FolderBar();
  auto &target = Panel(*window, 1).Files();
  files.Navigate(a.c_str());
  target.Navigate(c.c_str());
  Ready(*window);
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
  Command(*window, kCopy);
  Command(*window, kTabPrevious);
  Ready(*window);
  Command(*window, kTabClose);
  Ready(*window);
  Check(bar.Count() == 1 && bar.CanReopen() && bar.ActiveId() == first,
        "closing leaves sibling intact");
  Command(*window, kTabReopen);
  Ready(*window);
  Check(bar.ActiveId() == copy && bar.Snapshot().alias == "Work" &&
        files.SelectedPath() == (b/"two").string(), "reopening preserves name, ID and selected file");
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
  Check(window->QuitRequested(), "original may close independently");
  window->Quit();
  detached->Lock();
  Command(*detached, kTabDuplicate);
  Ready(*detached);
  Check(moved.FolderBar().Count() == 2, "surviving Einn window still accepts tab operations");
  detached->Quit();
  fs::remove_all(root);
}
