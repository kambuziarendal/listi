// SPDX-License-Identifier: MIT
#include "FileClipboard.h"
#include "Pane.h"
#include "Shelf.h"
#include <Application.h>
#include <LayoutBuilder.h>
#include <Window.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

int main() {
  using namespace Listi;
  BApplication app("application/x-vnd.Listi-PaneSearchListTest");
  char temp[] = "/tmp/Listi-search-list-XXXXXX", resolved[PATH_MAX];
  if (!mkdtemp(temp) || !realpath(temp, resolved))
    return 2;
  std::string root(resolved), a = root + "/one", b = root + "/two";
  if (mkdir(a.c_str(), 0700) || mkdir(b.c_str(), 0700))
    return 2;
  const std::string leaf = "match.live", left = a + "/" + leaf, right = b + "/" + leaf;
  std::ofstream(left) << "first";
  std::ofstream(right) << "second";

  int failures = 0;
  auto check = [&](bool ok, const char *label) {
    std::cout << (ok ? "PASS " : "FAIL ") << label << std::endl;
    if (!ok) ++failures;
  };
  Preferences prefs;
  auto *pane = new Pane(0, prefs);
  auto *w = new BWindow(BRect(0, 0, 720, 480), "Search list", B_TITLED_WINDOW, 0);
  BLayoutBuilder::Group<>(w, B_VERTICAL).Add(pane);
  w->Show();
  w->Lock();
  auto await = [&](auto done, bigtime_t timeout = 10000000) {
    auto end = system_time() + timeout;
    while (!done() && system_time() < end) {
      w->Unlock();
      snooze(25000);
      w->Lock();
    }
    return done();
  };
  auto ready = [&] { return !pane->IsScanning() && pane->LocationValid(); };
  auto folder = [&] { return ready() && !pane->IsSearchList() && root == pane->Path(); };
  auto matches = [&](size_t n, bigtime_t timeout = 10000000) {
    return await([&] { return ready() && pane->IsSearchList() &&
                               pane->MatchPattern("*.live", false) == n; }, timeout);
  };
  pane->Navigate(root.c_str());
  check(await(folder), "physical folder opens");
  BMessage open(kOpenSearchList);
  SavedSearch saved{"Live documents", {root, "name==\"*.live\"", true}, true};
  saved.WriteTo(open);
  pane->MessageReceived(&open);
  // The first volume-wide BFS query can require cold index reads. Live changes
  // already have a subscription and retain the shorter deadline.
  check(matches(2, 45000000), "real BFS query populates file pane with same-named originals");
  pane->MatchPattern("*.live", true);
  const auto ids = pane->SelectedLocalIdentities();
  check(ids.count(left) && ids.count(right) && ids.size() == 2,
        "both selected originals have their own frozen path and node");
  try {
    const auto items = ReadFileClipboard(MakeFileSelection(pane->Selected(), false, ids));
    Shelf shelf;
    shelf.Add(pane->Selected(), ids);
    check(items.size() == 2 && items[0].path != items[1].path &&
          shelf.Snapshot().entries.size() == 2, "native Copy and Shelf accept the original files");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    check(false, "copy and Shelf");
  }
  const auto selected = pane->CaptureNavigation();
  std::ofstream(a + "/extra.live") << "extra";
  check(matches(3) && pane->Selected().size() == 2 &&
        pane->CaptureNavigation().focused == selected.focused,
        "new live hit preserves multi-selection and focused original");
  rename(left.c_str(), (a + "/old").c_str());
  check(matches(2) && pane->Selected() == std::vector<std::string>{right},
        "no-longer-matching original leaves without selecting its namesake");
  BMessage back(kBack), forward(kForward);
  pane->MessageReceived(&back);
  check(await(folder), "Back returns to physical folder");
  pane->MessageReceived(&forward);
  check(matches(2) && pane->Selected() == std::vector<std::string>{right},
        "Forward restarts exact search criteria and restores its original selection");
  std::ofstream(left) << "replacement";
  pane->Navigate(root.c_str());
  check(await(folder), "leave query before independent restoration");
  pane->RestoreNavigation(selected);
  check(matches(3) && pane->Selected() == std::vector<std::string>{right},
        "restored query rejects replacement at identical path");
  pane->MessageReceived(&back);
  check(await(folder), "restored history has independent cursor and folder");
  saved.request.root = root + "/missing";
  BMessage missing(kOpenSearchList);
  saved.WriteTo(missing);
  pane->MessageReceived(&missing);
  check(await([&] { return !pane->IsScanning(); }) && !pane->LocationValid(),
        "unavailable scope is an error, not an empty successful search");
  // Keep the old inode alive while creating its namesake.
  const std::string original = root + "/original";
  std::ofstream(original) << "old";
  for (int i = 0; i < 80; ++i)
    std::ofstream(root + "/scroll-" + std::to_string(i)) << i;
  pane->Navigate(root.c_str());
  check(await(folder), "history fixture folder opens");
  auto scrolled = pane->CaptureNavigation();
  scrolled.scroll.y = 300;
  pane->RestoreNavigation(scrolled);
  check(await(folder) && pane->CaptureNavigation().scroll.y == 300,
        "fixture starts below the first screen");
  pane->MatchPattern("original", true);
  auto before = pane->CaptureNavigation();
  pane->Navigate(a.c_str());
  check(await(ready), "child visit opens");
  rename(original.c_str(), (a + "/retained").c_str());
  std::ofstream(original) << "new";
  pane->MessageReceived(&back);
  check(await(folder) && pane->Selected().empty() &&
        pane->CaptureNavigation().scroll.y == before.scroll.y,
        "Back retains viewport but rejects same-path replacement");
  pane->MessageReceived(&forward);
  check(await(ready) && a == pane->Path(), "Forward reopens child");
  BMessage up(kParent);
  pane->MessageReceived(&up);
  check(await(folder) && pane->Selected() == std::vector<std::string>{a} &&
        pane->CaptureNavigation().focused == a, "Up selects the child folder");
  w->Quit();
  unlink(original.c_str());
  unlink((a + "/retained").c_str());
  for (int i = 0; i < 80; ++i)
    unlink((root + "/scroll-" + std::to_string(i)).c_str());
  for (const auto &path : {left, right, a + "/extra.live", a + "/old"}) unlink(path.c_str());
  rmdir(a.c_str());
  rmdir(b.c_str());
  rmdir(root.c_str());
  return failures != 0;
}
