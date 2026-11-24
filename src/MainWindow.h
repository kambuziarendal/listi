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

constexpr uint32 kQuickView = 'qvpr', kInlineNotice = 'inot';

constexpr uint32 kNewBrowser = 'wbne', kSelectedBrowser = 'wbsl',
                 kQuitListi = 'wqap', kBrowserClosed = 'wbcl';
struct BrowserRequest {
  std::string path;
  std::optional<Listi::FolderTab> detached;
  std::optional<BPoint> position;
  bool localLayout = false;
};
using BrowserOpener = std::function<bool(BrowserRequest)>;
bool BrowserAllowsQuit(BWindow *);
BWindow *CreateMainWindow(const Preferences &, BMessenger service,
                          const Listi::SavedPlaces &, bool placesWritable,
                          const Listi::SavedSearches &, bool searchesWritable, BrowserRequest request = {}, BrowserOpener open = {});
void PostMainWindowNotice(const char *text);
