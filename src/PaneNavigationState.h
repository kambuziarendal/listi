// SPDX-License-Identifier: MIT
#pragma once
#include "NavigationHistory.h"
#include "PaneModel.h"
#include "SavedSearches.h"
#include <Point.h>
#include <map>

namespace Listi {
// Value state only. Workers, messengers, row pointers and open files belong to
// the visible pane, never to a suspended tab or detached navigation snapshot.
struct PaneNavigationState {
  std::string location = "/boot/home", displayLabel, filter, focused;
  NavigationHistory history;
  std::map<std::string, SavedSearch> queries;
  std::map<std::string, std::pair<int64_t, int64_t>> selection;
  std::pair<int64_t, int64_t> focusIdentity{-1, -1};
  PaneSort sorting;
  int32 view = -1, folderView = -1;
  BPoint scroll{0, 0};
};
constexpr uint32 kOpenSearchList = 'osli';
}
