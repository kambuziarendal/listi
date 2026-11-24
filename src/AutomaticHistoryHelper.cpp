// SPDX-License-Identifier: MIT
#include "AutomaticHistory.h"
#include "FolderHistory.h"
#include "HistorySettings.h"
#include "MessageStore.h"

#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <Deskbar.h>
#include <Entry.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <Path.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

namespace Listi {
namespace {
namespace fs = std::filesystem;
constexpr uint32 kEnable = 'aHen', kDisable = 'aHds', kRun = 'aHrn',
    kTick = 'aHtk', kFinished = 'aHfn', kState = 'aHst', kClosed = 'aHcl',
    kShow = 'aHsw', kHelp = 'aHhp',
    kPick = 'aHpk', kPicked = 'aHpd';
constexpr bigtime_t kSecond = 1000000;
constexpr bigtime_t kFirstDelay = 60 * kSecond, kInterval = 3600 * kSecond;

bool Overlaps(const fs::path &first, const fs::path &second) {
  auto relative = first.lexically_relative(second);
  return !relative.empty() && *relative.begin() != "..";
}

class SettingsWindow : public BWindow {
public:
  SettingsWindow(const BMessage &state, int64 id)
      : BWindow(BRect(140, 130, 820, 490), "Automatic folder history",
                B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS),
        id(id) {
    enable = new BButton("enable", "Enable automatic history", new BMessage(kEnable));
    disable = new BButton("disable", "Disable", new BMessage(kDisable));
    run = new BButton("run", "Run now", new BMessage(kRun));
    auto *help = new BButton("help", "How it works", new BMessage(kHelp));
    status = new BStringView("status", "");
    status->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    status->SetTruncation(B_TRUNCATE_END);
    BLayoutBuilder::Group<> layout(this, B_VERTICAL, B_USE_DEFAULT_SPACING);
    layout.SetInsets(B_USE_WINDOW_SPACING);
    layout.Add(new BStringView("instructions",
        "Choose up to three local BFS sources and a separate history folder for each."));
    for (size_t i = 0; i < kHistoryPairLimit; ++i) {
      auto number = std::to_string(i + 1);
      sources[i] = new BTextControl(("source" + number).c_str(),
          ("Source " + number + ":").c_str(), "", nullptr);
      stores[i] = new BTextControl(("store" + number).c_str(), "History folder:", "", nullptr);
      AddFolderRow(layout, i * 2, sources[i]);
      AddFolderRow(layout, i * 2 + 1, stores[i]);
    }
    layout.AddGroup(B_HORIZONTAL).Add(enable).Add(disable).Add(run)
        .AddGlue().Add(help).End().Add(status);
    SetState(state, true);
  }

  bool QuitRequested() override {
    BMessage closed(kClosed);
    closed.AddInt64("id", id);
    BMessenger(be_app).SendMessage(&closed);
    return true;
  }

  void MessageReceived(BMessage *message) override {
    switch (message->what) {
    case kState:
      SetState(*message, false);
      break;
    case kShow:
      Activate();
      break;
    case kPick: {
      auto field = message->GetInt32("field", -1);
      if (field < 0 || size_t(field) >= browse.size() || !browse[field]->IsEnabled()) break;
      BMessenger target(this);
      BMessage picked(kPicked);
      picked.AddInt32("field", field);
      picker = std::make_unique<BFilePanel>(B_OPEN_PANEL, &target, nullptr,
                                            B_DIRECTORY_NODE, false, &picked);
      const auto *path = Field(field)->Text();
      entry_ref initial;
      if (*path && get_ref_for_path(path, &initial) == B_OK)
        picker->SetPanelDirectory(&initial);
      picker->Show();
      break;
    }
    case kPicked: {
      auto field = message->GetInt32("field", -1);
      entry_ref ref;
      BPath path;
      if (field >= 0 && size_t(field) < browse.size() && browse[field]->IsEnabled() &&
          message->FindRef("refs", &ref) == B_OK &&
          BEntry(&ref, true).GetPath(&path) == B_OK)
        Field(field)->SetText(path.Path());
      break;
    }
    case kEnable: {
      BMessage request(kEnable);
      for (size_t i = 0; i < kHistoryPairLimit; ++i) {
        request.AddString("source", sources[i]->Text());
        request.AddString("store", stores[i]->Text());
      }
      BMessenger(be_app).SendMessage(&request);
      break;
    }
    case kDisable: case kRun:
      BMessenger(be_app).SendMessage(message);
      break;
    case kHelp:
      (new BAlert("Automatic folder history",
          "1. Enter an existing local BFS source and a separate, empty or already connected "
          "history folder. Leave unused pairs blank.\n\n"
          "2. Click Enable. The clock icon in Deskbar opens these settings. Listi can be closed. "
          "The helper starts after one minute and checks about once per hour. Run now is optional.\n\n"
          "3. In the source folder, click History to browse saved dates. Right-click the folder "
          "background for a preview beside your files. In the history window, restore one file or the folder as a new copy. Disable stops future saves without deleting versions.\n\n"
          "Only changed trees get points. Automatic saves stop at the configured safety limits "
          "(including 24 points or about 512 MiB per history folder). Nothing is removed "
          "automatically. This is not an independent backup or a live filesystem snapshot.",
          "Close"))->Go(nullptr);
      break;
    default:
      BWindow::MessageReceived(message);
    }
  }

private:
  void AddFolderRow(BLayoutBuilder::Group<> &layout, size_t field, BTextControl *control) {
    auto *pick = new BMessage(kPick);
    pick->AddInt32("field", field);
    browse[field] = new BButton("choose", "Choose…", pick);
    layout.AddGroup(B_HORIZONTAL).Add(control).Add(browse[field]).End();
  }

  BTextControl *Field(int32 field) {
    return field % 2 == 0 ? sources[field / 2] : stores[field / 2];
  }

  void SetState(const BMessage &state, bool loadPaths) {
    const bool enabled = state.GetBool("enabled", false);
    const bool working = state.GetBool("working", false);
    if (loadPaths) {
      auto pairs = HistoryPairs(state);
      for (size_t i = 0; i < pairs.size(); ++i) {
        sources[i]->SetText(pairs[i].GetString("path", 0, ""));
        stores[i]->SetText(pairs[i].GetString("path", 1, ""));
      }
    }
    for (size_t i = 0; i < kHistoryPairLimit; ++i) {
      sources[i]->SetEnabled(!enabled && !working);
      stores[i]->SetEnabled(!enabled && !working);
    }
    for (auto *button : browse) button->SetEnabled(!enabled && !working);
    enable->SetEnabled(!enabled && !working);
    disable->SetEnabled(enabled);
    run->SetEnabled(enabled && !working && state.GetBool("tile", false));
    const char *text = state.GetString("status", "");
    status->SetText(text);
    status->SetToolTip(text);
  }

  std::array<BTextControl *, kHistoryPairLimit> sources{}, stores{};
  const int64 id;
  std::array<BButton *, kHistoryPairLimit * 2> browse{};
  std::unique_ptr<BFilePanel> picker;
  BButton *enable{}, *disable{}, *run{};
  BStringView *status{};
};

class HistoryHelper : public BApplication {
public:
  explicit HistoryHelper(bool openSettings)
      : BApplication(kHistoryHelperSignature), showOnStart(openSettings) {}

  ~HistoryHelper() override {
    canceled = true;
    if (worker.joinable()) worker.join();
  }

  void ArgvReceived(int32 argc, char **argv) override {
    if (argc > 1 && strcmp(argv[1], "--settings") == 0) {
      if (ready) ShowSettings();
      else showOnStart = true;
    }
  }

  void ReadyToRun() override {
    try {
      auto stored = ReadHistorySettings();
      if (stored.what != 'none') {
        auto pairs = HistoryPairs(stored);
        if (stored.GetBool("enabled", false) && pairs.empty())
          throw std::runtime_error("No automatic history folders are configured.");
        config = stored;
      }
      if (Enabled()) {
        status = "Enabled. First automatic check in one minute.";
        StartTimer();
      }
    } catch (const std::exception &error) {
      status = error.what();
    }
    ready = true;
    if (showOnStart) ShowSettings();
    if (!Enabled() && !showOnStart) PostMessage(B_QUIT_REQUESTED);
  }

  void MessageReceived(BMessage *message) override {
    try {
      switch (message->what) {
      case kEnable:
        if (!Enabled() && !worker.joinable()) Enable(*message);
        break;
      case kDisable:
        if (Enabled()) Disable();
        break;
      case kTick:
        if (!Enabled()) break;
        if (!BDeskbar().HasItem(kHistoryDeskbarItem)) {
          Disable();
          break;
        }
        if (!worker.joinable() && system_time() >= next)
          StartRun();
        break;
      case kRun:
        if (Enabled() && !worker.joinable() && BDeskbar().HasItem(kHistoryDeskbarItem))
          StartRun();
        break;
      case kFinished:
        if (worker.joinable()) worker.join();
        if (Enabled()) status = message->GetString("status", "Check finished.");
        PushState();
        MaybeQuit();
        break;
      case kClosed:
        if (message->GetInt64("id", -1) == settingsId)
          settings = BMessenger();
        MaybeQuit();
        break;
      default:
        BApplication::MessageReceived(message);
      }
    } catch (const std::exception &error) {
      status = error.what();
      PushState();
    }
  }

private:
  bool Enabled() const { return config.GetBool("enabled", false); }

  BMessage State() const {
    BMessage state(config);
    state.what = kState;
    state.AddBool("working", worker.joinable());
    state.AddBool("tile", BDeskbar().HasItem(kHistoryDeskbarItem));
    state.AddString("status", status.c_str());
    return state;
  }

  void ShowSettings() {
    if (settings.SendMessage(kShow) == B_OK) return;
    auto *window = new SettingsWindow(State(), ++settingsId);
    settings = BMessenger(window);
    window->Show();
    window->Activate();
  }

  void PushState() {
    if (settings.IsValid()) {
      auto state = State();
      settings.SendMessage(&state);
    }
  }

  void MaybeQuit() {
    if (!Enabled() && !settings.IsValid() && !worker.joinable())
      PostMessage(B_QUIT_REQUESTED);
  }

  void StartTimer() {
    next = system_time() + kFirstDelay;
    const BMessage tick(kTick);
    timer = std::make_unique<BMessageRunner>(BMessenger(this), &tick, 15 * kSecond);
    if (timer->InitCheck() != B_OK)
      throw std::runtime_error("Cannot schedule automatic history.");
  }

  void Enable(const BMessage &request) {
    BMessage desired(kHistoryConfigFormat);
    std::vector<fs::path> paths;
    for (size_t i = 0; i < kHistoryPairLimit; ++i) {
      const fs::path source(request.GetString("source", i, ""));
      const fs::path store(request.GetString("store", i, ""));
      if (source.empty() && store.empty()) continue;
      if (!source.is_absolute() || !store.is_absolute())
        throw std::runtime_error("Each used row needs two absolute folder paths.");
      HistoryStore history(source, store);
      auto pair = history.FolderIdentity();
      for (int32 j = 0; j < 2; ++j) {
        fs::path path(pair.GetString("path", j, ""));
        for (const auto &existing : paths) {
          if (Overlaps(path, existing) || Overlaps(existing, path))
            throw std::runtime_error("Selected sources and history folders must not overlap.");
        }
        paths.push_back(path);
      }
      if (desired.AddMessage("pair", &pair) != B_OK)
        throw std::runtime_error("Cannot save selected folders.");
    }
    if (paths.empty())
      throw std::runtime_error("Enter at least one source and its history folder.");
    if (desired.AddBool("enabled", true) != B_OK)
      throw std::runtime_error("Cannot encode automatic history settings.");

    BDeskbar deskbar;
    entry_ref ref;
    if (!deskbar.HasItem(kHistoryDeskbarItem) &&
        (get_ref_for_path(kHistoryHelperPath, &ref) != B_OK ||
         deskbar.AddItem(&ref) != B_OK || !deskbar.HasItem(kHistoryDeskbarItem)))
      throw std::runtime_error("Cannot add Listi Folder History to Deskbar.");
    try {
      StartTimer();
      SaveMessageStore(HistorySettingsFile(), desired);
    } catch (...) {
      timer.reset();
      deskbar.RemoveItem(kHistoryDeskbarItem);
      throw;
    }
    config = desired;
    status = "Enabled. First automatic check in one minute.";
    PushState();
  }

  void Disable() {
    canceled = true;
    timer.reset();
    BMessage desired(config);
    desired.SetBool("enabled", false);
    // Stop this process even if the disk write fails. Remove the Deskbar
    // item too, so a stale enabled flag cannot start the helper at login.
    config = desired;
    BDeskbar deskbar;
    auto remove = deskbar.HasItem(kHistoryDeskbarItem)
        ? deskbar.RemoveItem(kHistoryDeskbarItem) : B_OK;
    SaveMessageStore(HistorySettingsFile(), desired);
    status = remove == B_OK
        ? "Disabled. Existing points were kept."
        : "Disabled. Existing points were kept, but Deskbar could not remove the icon.";
    PushState();
    MaybeQuit();
  }

  void StartRun() {
    auto pairs = HistoryPairs(config);
    canceled = false;
    next = system_time() + kInterval;
    const BMessenger reply(this);
    worker = std::thread([this, pairs = std::move(pairs), reply] {
      set_thread_priority(find_thread(nullptr), B_LOW_PRIORITY);
      std::string result;
      for (const auto &pair : pairs) {
        if (canceled) break;
        const char *source = pair.GetString("path", 0, "");
        if (!result.empty()) result += " · ";
        result += fs::path(source).filename().string() + ": ";
        try {
          HistoryStore history(source, pair.GetString("path", 1, ""));
          result += history.SaveAutomatically(canceled, pair)
              ? "point saved" : "unchanged";
        } catch (const std::exception &error) {
          result += error.what();
        }
      }
      BMessage finished(kFinished);
      finished.AddString("status", result.c_str());
      reply.SendMessage(&finished);
    });
    status = "Checking selected folders…";
    PushState();
  }

  BMessage config{kHistoryConfigFormat};
  BMessenger settings;
  int64 settingsId = 0;
  std::unique_ptr<BMessageRunner> timer;
  std::thread worker;
  std::atomic_bool canceled{false};
  std::string status = "Off. No folders are monitored.";
  bigtime_t next = 0;
  bool ready = false;
  bool showOnStart = false;
};
}
}

int main(int argc, char **argv) {
  Listi::HistoryHelper helper(argc > 1 && strcmp(argv[1], "--settings") == 0);
  helper.Run();
  return 0;
}
