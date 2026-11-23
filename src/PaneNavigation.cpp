// SPDX-License-Identifier: MIT
#include "Pane.h"
#include "FileDetails.h"
#include "FileGrid.h"
#include "MillerColumns.h"
#include "SearchWorker.h"
#include "VirtualFile.h"
#include <MessageRunner.h>
#include <algorithm>

namespace {
using Identity = std::pair<int64_t, int64_t>;
Identity IdentityOf(const PaneEntry &entry) { return {entry.device, entry.node}; }
}

std::string Pane::LocationLabel() const {
  auto query = queries.find(path.String());
  if (query == queries.end())
    return path.String();
  const auto &search = query->second;
  return "Search: " + (search.name.empty() ? search.request.expression : search.name) +
         " in " + search.request.root;
}

BPoint Pane::ScrollPosition() {
  if (mode == 0)
    return list->ScrollPosition();
  return mode == MillerMode ? columns->Bounds().LeftTop() : grid->Bounds().LeftTop();
}

void Pane::RestoreScroll(BPoint position) {
  if (mode == 0)
    list->RestoreScroll(position);
  else if (mode == MillerMode)
    columns->ScrollTo(position);
  else
    grid->ScrollTo(position);
}

Listi::PaneNavigationState Pane::CaptureNavigation() {
  if (restoring)
    return *restoring;
  Listi::PaneNavigationState state;
  state.location = path.String();
  state.displayLabel = IsSearchList() ? LocationLabel() : Listi::Leaf(state.location);
  state.history = history;
  state.queries = queries;
  state.filter = filter->Text();
  state.view = mode;
  state.sorting = Sorting();
  state.scroll = ScrollPosition();
  state.selection = savedSelection;
  for (auto *row : entries.Visible())
    if (entries.IsSelected(row->path))
      state.selection[row->path] = IdentityOf(*row);
  if (auto *row = entries.Focused()) {
    state.focused = row->path;
    state.focusIdentity = IdentityOf(*row);
  }
  return state;
}

void Pane::RestoreNavigation(const Listi::PaneNavigationState &state) {
  if (!Window() || state.location.empty())
    return;
  history = state.history;
  queries = state.queries;
  restoredView = state.view;
  restoring = state;
  const int index = history.Position();
  StartNavigation(state.location, index >= 0 && history.At(index) == state.location ? index : -1,
                  false, true);
}

void Pane::OpenSearchList(const Listi::SavedSearch &search) {
  if (search.request.root.empty() || search.request.root.front() != '/' ||
      Listi::IsVirtual(search.request.root)) {
    ShowStatus("Choose a local folder for the search.");
    return;
  }
  static std::atomic<uint64_t> next{0};
  auto key = "@query:" + std::to_string(++next);
  // A failed search is retryable until the user leaves it; only successful
  // visits can be reached again through Back or Forward.
  for (auto it = queries.begin(); it != queries.end();) {
    bool reachable = it->first == path.String();
    for (size_t i = 0; !reachable && i < history.Size(); ++i)
      reachable = history.At(i) == it->first;
    if (!reachable)
      it = queries.erase(it);
    else
      ++it;
  }
  queries.emplace(key, search);
  StartNavigation(key);
}

bool Pane::HandleQueryReply(BMessage &message) {
  using namespace Listi;
  if (message.what != kSearchBegin && message.what != kSearchRows &&
      message.what != kSearchDone)
    return false;
  if (!IsSearchList() || message.GetInt64("serial", -1) != serial || !queryWorker)
    return true;
  if (message.what == kSearchBegin) {
    queryRows.clear();
    return true;
  }
  if (message.what == kSearchRows) {
    BMessage hit;
    for (int i = 0; message.FindMessage("hit", i, &hit) == B_OK; ++i) {
      auto row = std::make_unique<PaneEntry>();
      row->path = hit.GetString("path", "");
      row->name = Leaf(row->path);
      row->device = hit.GetInt64("device", -1);
      row->node = hit.GetInt64("node", -1);
      row->kind = hit.GetInt32("kind", 0);
      row->directory = hit.GetBool("directory", false);
      row->size = hit.GetInt64("size", 0);
      row->modified = hit.GetInt64("date", 0);
      row->attributeCells.push_back({Listi::ParentLocation(row->path), B_STRING_TYPE});
      queryRows.push_back(std::move(row));
    }
    return true;
  }
  const bool initial = scanning;
  auto state = CaptureNavigation();
  const std::string error = message.GetString("error", "");
  if (!error.empty() || message.GetBool("cancelled", false)) {
    Listi::RetireReadWorker(queryWorker);
    ClearRows();
    queryRows.clear();
    savedSelection.clear();
    restoring.reset();
    scanTimer.reset();
    scanning = locationValid = false;
    history.Complete(serial, path.String(), false);
    UpdateNavigation();
    SetNavigationError(error.empty() ? "Search cancelled. Reload to retry." : error.c_str());
    SendSelection();
    return true;
  }
  // A live query reconciles periodically even without a notification. Do
  // not discard native rows, selection gestures or scroll on an unchanged list.
  if (!initial && entries.MatchesSnapshot(queryRows)) {
    queryRows.clear();
    return true;
  }
  ClearRows();
  std::string focus;
  for (auto &row : queryRows) {
    const auto selected = state.selection.find(row->path);
    const auto identity = IdentityOf(*row);
    if (row->path == state.focused && identity == state.focusIdentity)
      focus = row->path;
    entries.AddScanned(std::move(row), einn ? "" : filter->Text(),
                       selected != state.selection.end() && selected->second == identity);
  }
  queryRows.clear();
  entries.SetSelection(entries.Selected(), focus);
  scanning = false;
  locationValid = true;
  savedSelection.clear();
  restoring.reset();
  history.Complete(serial, path.String(), true);
  UpdateNavigation();
  SetDisplayMode(0);
  Sort(state.sorting);
  RestoreScroll(state.scroll);
  if (!initial && entries.Selected().size() < state.selection.size())
    ShowStatus("A selected original disappeared, changed identity or no longer matches.");
  else if (message.GetBool("limited", false) || message.GetInt64("skipped", 0) > 0)
    ShowStatus("Partial search results. Some entries were skipped or a search limit was reached.");
  if (!message.GetBool("live", false)) {
    scanTimer.reset();
    Listi::RetireReadWorker(queryWorker);
  }
  if (refreshPending)
    Reload();
  return true;
}
