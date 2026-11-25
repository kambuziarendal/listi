// SPDX-License-Identifier: MIT
// Opt-in native benchmark. Use an existing, dedicated flat directory of files.
#include "Pane.h"
#include <Application.h>
#include <LayoutBuilder.h>
#include <Window.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits.h>
#include <unistd.h>

class TimedPane : public Pane {
public:
  using Pane::Pane;
  std::atomic<bigtime_t> first{0}, done{0};
  bool measuring = false;
  void MessageReceived(BMessage *message) override {
    Pane::MessageReceived(message);
    if (!measuring)
      return;
    if (!first && message->what == kRows) {
      Window()->UpdateIfNeeded();
      first = system_time();
    }
    if (message->what == kScanDone && !IsScanning())
      done = system_time();
  }
};

size_t Resident(team_id team) {
  ssize_t cookie = 0;
  area_info area;
  size_t bytes = 0;
  while (get_next_area_info(team, &cookie, &area) == B_OK)
    bytes += area.ram_size;
  return bytes;
}

int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "Usage: directory-scaling FOLDER EXACT_COUNT MODE ICONS\n");
    return 2;
  }
  char folder[PATH_MAX];
  if (!realpath(argv[1], folder))
    return 2;
  const size_t count = std::stoul(argv[2]);
  const int mode = std::stoi(argv[3]), icons = std::stoi(argv[4]);
  BApplication app("application/x-vnd.Listi-ScanScaling");
  Preferences prefs;
  prefs.SetPaneMode(0, mode);
  prefs.icons = icons;
  auto window = new BWindow(BRect(10, 10, 800, 560), "Directory scan benchmark",
                            B_TITLED_WINDOW, 0);
  auto pane = new TimedPane(0, prefs);
  BLayoutBuilder::Group<>(window, B_VERTICAL).Add(pane);
  window->Show();
  window->Lock();
  const auto team = app.Team();
  team_usage_info before{}, after{};
  if (get_team_usage_info(team, B_TEAM_USAGE_SELF, &before) != B_OK)
    return 2;
  auto base = Resident(team), peak = base;
  auto start = system_time();
  pane->Navigate(folder);
  pane->measuring = true;
  window->Unlock();
  while (!pane->done && system_time() - start < 300000000) {
    peak = std::max(peak, Resident(team));
    snooze(10000);
  }
  if (!pane->done)
    return 3;
  window->Lock();
  peak = std::max(peak, Resident(team));
  if (get_team_usage_info(team, B_TEAM_USAGE_SELF, &after) != B_OK)
    return 2;
  const auto first = pane->first.load(), done = pane->done.load();
  const bool correct = pane->LocationValid() && pane->CurrentView() == mode &&
      std::string(pane->Path()) == folder && pane->MatchPattern("*", false) == count;
  auto leave = system_time();
  pane->first = pane->done = 0;
  pane->Navigate("/");
  window->Unlock();
  while (!pane->done && system_time() - leave < 300000000)
    snooze(10000);
  window->Lock();
  const bool navigated = pane->done && pane->LocationValid() && std::string(pane->Path()) == "/";
  const auto away = pane->done.load() - leave;
  window->Quit();
  printf("rows=%zu mode=%d icons=%d first_ms=%.3f total_ms=%.3f cpu_ms=%.3f "
         "baseline_KiB=%zu peak_KiB=%zu navigate_away_ms=%.3f correct=%d\n",
         count, mode, icons, (first-start)/1000., (done-start)/1000.,
         (after.kernel_time+after.user_time-before.kernel_time-before.user_time)/1000.,
         base/1024, peak/1024, away/1000., int(correct && navigated));
  return correct && navigated && first >= start && first <= done ? 0 : 1;
}
