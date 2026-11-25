// SPDX-License-Identifier: MIT
#include "ReadWorker.h"
#include "ApplicationServices.h"
#include "FolderHistory.h"
#include "ArchiveEditor.h"
#include "ArchivePasswords.h"
#include "ArchivePasswordWindow.h"
#include "Common.h"
#include "FileOperation.h"
#include "InspectorEditor.h"
#include "MainWindow.h"
#include "WindowOverview.h"
#include "MessageStore.h"
#include "OpenWith.h"
#include "PropertiesWindow.h"
#include "SavedPlaces.h"
#include "SavedSearches.h"
#include <Application.h>
#include <Alert.h>
#include <Window.h>
#include <Directory.h>
#include <Path.h>
#include <map>
#include <mutex>
#include <vector>
#include <unistd.h>
#include <cstring>
#include <exception>

class App : public BApplication {
  BLooper *services = nullptr;
  Preferences prefs;
  Listi::SavedPlaces places;
  Listi::SavedSearches searches;
  bool placesWritable = false, searchesWritable = false;
  std::map<int64, BMessenger> browsers;
  std::mutex requestLock;
  std::vector<BrowserRequest> requests;
  bool ready = false;
  std::vector<std::string> initialHistory;

  void Broadcast(BMessage *message) {
    for (auto &entry : browsers)
      entry.second.SendMessage(message);
  }
  BWindow *Open(BrowserRequest request) {
    request.localLayout = !browsers.empty();
    auto *window = CreateMainWindow(prefs, BMessenger(services), places,
        placesWritable, searches, searchesWritable, std::move(request),
        [this](BrowserRequest next) { return Queue(std::move(next)); });
    auto *working = static_cast<Listi::WorkingWindow *>(window);
    browsers.emplace(working->WindowId(), BMessenger(window));
    window->Show();
    return window;
  }
  void Drain() {
    std::vector<BrowserRequest> pending;
    {
      std::lock_guard<std::mutex> guard(requestLock);
      pending.swap(requests);
    }
    for (auto &request : pending) {
      if (request.external && !browsers.empty()) {
        auto opening = prefs.folderOpening;
        if (opening == Preferences::Ask) {
          std::string question = "Open " + request.path + " in a new window or a folder tab?";
          int32 choice = (new BAlert("Open folder", question.c_str(), "Cancel",
                                     "Window", "Tab"))->Go();
          if (choice < 1) continue;
          opening = choice == 2 ? Preferences::NewTab : Preferences::NewWindow;
        }
        if (opening == Preferences::NewTab) {
          BMessage tab(kExternalTab);
          tab.AddString("path", request.path.c_str());
          if (browsers.begin()->second.SendMessage(&tab) == B_OK)
            continue;
        }
      }
      Open(std::move(request));
    }
  }
  void LaunchDirectory(const char *path, bool forceWindow = false) {
    BDirectory directory(path);
    BEntry entry;
    BPath normalized;
    if (directory.GetEntry(&entry) == B_OK && entry.GetPath(&normalized) == B_OK)
      Queue({normalized.Path(), {}, {}, false, !forceWindow});
  }

public:
  bool Queue(BrowserRequest request) {
    std::lock_guard<std::mutex> guard(requestLock);
    requests.emplace_back(std::move(request));
    if (PostMessage(kNewBrowser) == B_OK)
      return true;
    requests.pop_back();
    return false;
  }
  void ArgvReceived(int32 argc, char **argv) override {
    int index = argc > 1 && !strcmp(argv[1], "--new-window") ? 2 : 1;
    if (argc > index) {
      const char *cwd = CurrentMessage() ? CurrentMessage()->GetString("cwd", nullptr) : nullptr;
      BPath path(argv[index]);
      if (cwd && argv[index][0] != '/')
        path.SetTo(cwd, argv[index], true);
      if (path.Path())
        LaunchDirectory(path.Path(), index == 2);
    } else if (index == 2)
      Queue({});
  }
  void RefsReceived(BMessage *message) override {
    entry_ref ref;
    for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; ++i) {
      BEntry entry(&ref, true);
      BPath path;
      if (entry.GetPath(&path) == B_OK)
        LaunchDirectory(path.Path());
    }
  }
  App() : BApplication(kSignature) {}
  ~App() {
    Listi::StopArchivePasswords();
    if (services && services->Lock())
      services->Quit();
    // Windows and write-capable services have finished their normal shutdown.
    // Do not run kit/static destructors underneath a stranded read thread.
    if (!Listi::DrainReadWorkers(500000))
      _exit(0);
  }
  void ReadyToRun() override {
    InstallArchivePasswordWindow();
    BMessage settings;
    std::string settingsError;
    try {
      settings = Listi::LoadMessageStore(SettingsPath().String());
      prefs.Load(settings);
    } catch (const std::exception &e) {
      settingsError = e.what();
    }
    services = CreateApplicationServices(BMessenger(this), settingsError.empty());
    services->Run();
    std::string placesError;
    try {
      places = Listi::SavedPlaces::Load(SettingsPath("Lister_places").String());
    } catch (const std::exception &e) {
      placesError = e.what();
    }
    std::string searchesError;
    try {
      searches =
          Listi::SavedSearches::Load(SettingsPath("Lister_searches").String());
    } catch (const std::exception &e) {
      searchesError = e.what();
    }
    placesWritable = placesError.empty();
    searchesWritable = searchesError.empty();
    ready = true;
    Drain();
    if (browsers.empty())
      Open({});
    for (const auto &path : initialHistory)
      Listi::OpenFolderHistory(path.c_str());
    initialHistory.clear();
    auto &firstWindow = browsers.begin()->second;
    if (settingsError.empty() && settings.what == 'none') {
      BMessage guide(Listi::kShowLayoutGuide);
      guide.AddBool("firstRun", true);
      firstWindow.SendMessage(&guide);
    }
    if (!settingsError.empty()) {
      settingsError =
          "Could not load preferences. Changes apply only to this session; the original settings file is preserved: " +
          settingsError;
      PostMainWindowNotice(settingsError.c_str());
    }
    if (!searchesError.empty()) {
      searchesError =
          "Saved searches could not be loaded. The file has been preserved; changes are disabled. " +
          searchesError;
      PostMainWindowNotice(searchesError.c_str());
    }
    if (!placesError.empty()) {
      placesError =
          "Saved places could not be loaded. The file has been preserved; changes are disabled. " +
          placesError;
      PostMainWindowNotice(placesError.c_str());
    }
  }
  bool QuitRequested() override {
    if (Listi::FolderHistoryBusy()) {
      PostMainWindowNotice("Finish or cancel the folder history operation before quitting Listi.");
      return false;
    }
    if (CurrentMessage() && CurrentMessage()->GetBool("lastBrowser", false)) {
      Drain();
      if (!browsers.empty())
        return false;
    }
    for (const auto &entry : browsers) {
      auto target = entry.second;
      if (!target.IsValid())
        continue;
      if (target.LockTargetWithTimeout(500000) != B_OK)
        return false;
      BLooper *looper = nullptr;
      target.Target(&looper);
      const bool allowed = BrowserAllowsQuit(static_cast<BWindow *>(looper));
      looper->Unlock();
      if (!allowed)
        return false;
    }
    if (services) {
      BMessage fence(kDrainServices), reply;
      if (BMessenger(services).SendMessage(&fence, &reply, 500000, 2000000) != B_OK)
        return false;
    }
    return InspectorEditorsAllowQuit() && FilePropertiesAllowQuit() && OpenWithAllowQuit() &&
           ArchiveEditorsAllowQuit() && BApplication::QuitRequested();
  }
  void MessageReceived(BMessage *m) override {
    if (m->what == Listi::kFolderHistory) {
      const auto *path = m->GetString("source", "");
      if (*path == '/') {
        if (ready) Listi::OpenFolderHistory(path);
        else initialHistory.emplace_back(path);
      }
      return;
    }
    if (m->what == kNewBrowser) {
      if (ready)
        Drain();
      return;
    }
    if (m->what == B_SILENT_RELAUNCH && prefs.separateWindows) {
      Queue({});
      return;
    }
    if (m->what == kBrowserClosed) {
      browsers.erase(m->GetInt64("id", -1));
      if (browsers.empty()) {
        BMessage last(B_QUIT_REQUESTED);
        last.AddBool("lastBrowser", true);
        PostMessage(&last);
      }
      return;
    }
    if (m->what == kSharedPreferences || m->what == kSavePreferences) {
      if (m->what == kSharedPreferences) {
        prefs.ApplyShared(*m);
        Broadcast(m);
      } else {
        // Only the primary window supplies layout. Its copy of global fields
        // may predate another window's edit still waiting for broadcast.
        BMessage shared = prefs.SharedChanges();
        prefs.Load(*m);
        prefs.ApplyShared(shared);
      }
      BMessage saved = prefs.Save();
      saved.what = kSavePreferences;
      BMessenger origin;
      if (m->FindMessenger("origin", &origin) == B_OK)
        saved.AddMessenger("origin", origin);
      BMessenger(services).SendMessage(&saved);
      return;
    }
    if (m->what == 'isav') {
      SendNotices(m->what, m);
      return;
    }
    if (m->what == kPlacesSaved || m->what == kSearchesSaved) {
      BMessage snapshot;
      if (m->FindMessage("snapshot", &snapshot) == B_OK) {
        if (m->what == kPlacesSaved)
          places = Listi::SavedPlaces::FromMessage(snapshot);
        else
          searches = Listi::SavedSearches::FromMessage(snapshot);
        Broadcast(m);
      } else {
        BMessenger origin;
        if (m->FindMessenger("origin", &origin) == B_OK)
          origin.SendMessage(m);
      }
      return;
    }
    if (m->what == kServiceError) {
      BMessage error(*m);
      error.what = kInlineNotice;
      BMessenger origin;
      if (m->FindMessenger("origin", &origin) == B_OK)
        origin.SendMessage(&error);
      else
        Broadcast(&error);
      return;
    }
    BApplication::MessageReceived(m);
  }
};
namespace Listi {
int VirtualTest(const std::string &, const std::string &);
}
int main(int argc, char **argv) {
  if (argc == 4 && !strcmp(argv[1], "--virtual-test"))
    return Listi::VirtualTest(argv[2], argv[3]);
  if (argc > 1 && !strcmp(argv[1], "--self-test"))
    return Listi::SelfTest();
  App app;
  app.Run();
  return 0;
}
