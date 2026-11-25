// SPDX-License-Identifier: MIT
#pragma once
#include "FolderTabs.h"
#include <Messenger.h>
#include <functional>

class BWindow;
struct Preferences;
namespace Listi {
class SavedPlaces;
class SavedSearches;
}

constexpr uint32 kExternalTab = 'extb';
constexpr uint32 kQuickView = 'qvpr', kInlineNotice = 'inot';

constexpr uint32 kNewBrowser = 'wbne', kSelectedBrowser = 'wbsl',
                 kQuitListi = 'wqap', kBrowserClosed = 'wbcl';
struct BrowserRequest {
  std::string path;
  std::optional<Listi::FolderTab> detached;
  std::optional<BPoint> position;
  bool localLayout = false;
  bool external = false;
  std::optional<BMessage> workingSet = {};
  std::string detachedWorkNotice = {};
};
using BrowserOpener = std::function<bool(BrowserRequest)>;
bool CheckSharedQuitBlockers();
bool CheckBrowserQuitBlockers(BWindow *);
bool BrowserHasCLIs(BWindow *);
bool CloseBrowserCLIs(BWindow *);
void SuspendBrowserOperationsWindow(BWindow *, bool suspend);
void ShowQuitBlocker(const char *text);
BMessenger ConfirmClosingCLIs(BMessenger recipient, uint32 command, bool wholeApp);
BWindow *CreateMainWindow(const Preferences &, BMessenger service,
                          const Listi::SavedPlaces &, bool placesWritable,
                          const Listi::SavedSearches &, bool searchesWritable, BrowserRequest request = {}, BrowserOpener open = {});
void PostMainWindowNotice(const char *text);
