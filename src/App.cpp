// SPDX-License-Identifier: MIT
#include "ReadWorker.h"
#include "ApplicationServices.h"
#include "ArchiveEditor.h"
#include "ArchivePasswords.h"
#include "ArchivePasswordWindow.h"
#include "Common.h"
#include "FileOperation.h"
#include "InspectorEditor.h"
#include "MainWindow.h"
#include "MessageStore.h"
#include "OpenWith.h"
#include "PropertiesWindow.h"
#include "SavedPlaces.h"
#include "SavedSearches.h"
#include <Application.h>
#include <Window.h>
#include <unistd.h>
#include <cstring>
#include <exception>

class App : public BApplication {
  BLooper *services = nullptr;
  BMessenger mainWindow;

public:
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
    Preferences prefs;
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
    Listi::SavedPlaces places;
    std::string placesError;
    try {
      places = Listi::SavedPlaces::Load(SettingsPath("Lister_places").String());
    } catch (const std::exception &e) {
      placesError = e.what();
    }
    Listi::SavedSearches searches;
    std::string searchesError;
    try {
      searches =
          Listi::SavedSearches::Load(SettingsPath("Lister_searches").String());
    } catch (const std::exception &e) {
      searchesError = e.what();
    }
    auto *window = CreateMainWindow(prefs, BMessenger(services), places,
                                    placesError.empty(), searches, searchesError.empty());
    mainWindow = BMessenger(window);
    window->Show();
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
    return InspectorEditorsAllowQuit() && FilePropertiesAllowQuit() && OpenWithAllowQuit() &&
           ArchiveEditorsAllowQuit() && BApplication::QuitRequested();
  }
  void MessageReceived(BMessage *m) override {
    if (m->what == 'isav') {
      SendNotices(m->what, m);
      return;
    }
    if (m->what == kPlacesSaved || m->what == kSearchesSaved) {
      mainWindow.SendMessage(m);
      return;
    }
    if (m->what == kServiceError) {
      BMessage error(*m);
      error.what = kInlineNotice;
      mainWindow.SendMessage(&error);
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
