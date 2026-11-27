// SPDX-License-Identifier: MIT
#include "Pane.h"
#include "UnavailableVolume.h"
#include "FileDetails.h"
#include "FileGrid.h"
#include "MillerColumns.h"
#include "SearchWorker.h"
#include "VirtualFile.h"
#include <MessageRunner.h>
#include <ScrollBar.h>
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
  return mode == MillerMode ? columns->ScrollPosition() : grid->Bounds().LeftTop();
}

PaneEntry *Pane::ScrollRow(const std::string *path, float &top) {
  if (mode == 0) return list->ScrollRow(path, top);
  return mode == MillerMode ? columns->ScrollRow(path, top) : grid->ScrollRow(path, top);
}

Listi::VisitPosition Pane::CaptureVisit(size_t selectionLimit) {
  Listi::VisitPosition visit;
  auto scroll = ScrollPosition();
  visit.x = scroll.x;
  visit.y = scroll.y;
  float top;
  if (auto *row = ScrollRow(nullptr, top)) {
    visit.anchor = row->path;
    visit.anchorId = IdentityOf(*row);
    visit.offset = scroll.y - top;
  }
  if (auto *row = entries.Focused()) {
    visit.focus = row->path;
    visit.focusId = IdentityOf(*row);
    if (entries.IsSelected(row->path))
      visit.selected.emplace(row->path, visit.focusId);
  }
  for (auto *row : entries.Visible()) {
    if (visit.selected.size() >= selectionLimit) break;
    if (entries.IsSelected(row->path))
      visit.selected.emplace(row->path, IdentityOf(*row));
  }
  return visit;
}

void Pane::RestoreVisit() {
  if (!pendingVisit) return;
  const auto &visit = *pendingVisit;
  std::vector<std::string> selection;
  std::string focus;
  for (auto *row : entries.Visible()) {
    auto selected = visit.selected.find(row->path);
    if (selected != visit.selected.end() && selected->second == IdentityOf(*row))
      selection.push_back(row->path);
    if (visit.focus == row->path && visit.focusId == IdentityOf(*row))
      focus = row->path;
  }
  entries.SetSelection(selection, focus);
  SendSelection();
  float top;
  auto *anchor = ScrollRow(&visit.anchor, top);
  RestoreScroll({visit.x, anchor && IdentityOf(*anchor) == visit.anchorId
                              ? top + visit.offset : visit.y});
  pendingVisit.reset();
}

void Pane::RestoreScroll(BPoint position) {
  if (mode == 0)
    list->RestoreScroll(position);
  else if (mode == MillerMode)
    columns->RestoreScroll(position);
  else if (auto *bar = grid->ScrollBar(B_VERTICAL))
    bar->SetValue(position.y);
}

Listi::PaneNavigationState Pane::CaptureNavigation() {
  if (restoring)
    return *restoring;
  Listi::PaneNavigationState state;
  state.location = path.String();
  state.folderDevice = folderIdentity.first;
  state.displayLabel = IsSearchList() ? LocationLabel() : Listi::Leaf(state.location);
  state.history = history;
  state.pendingVisit = pendingVisit;
  state.queries = queries;
  state.filter = filter->Text();
  state.view = mode;
  state.workingSetView = workingSetView >= 0;
  state.folderView = restoredView >= 0 ? restoredView : prefs.PaneMode(id);
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
  restoredView = state.folderView >= 0 ? state.folderView : state.view;
  workingSetView = state.workingSetView ? state.view : -1;
  restoring = state;
  const int index = history.PendingTarget() >= 0 ? history.PendingTarget() : history.Position();
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
    receivedRows = {};
    queryRows.clear();
    return true;
  }
  if (message.what == kSearchRows) {
    receivedRows.Read(message, "hit", [&](const BMessage &hit) {
      auto row = std::make_unique<PaneEntry>();
      row->path = hit.GetString("path", "");
      row->name = Leaf(row->path);
      row->device = hit.GetInt64("device", -1);
      if (Listi::InterruptedVolumes().ContainsDevice(row->device) ||
          Listi::InterruptedVolumes().Contains(row->path))
        return;
      row->node = hit.GetInt64("node", -1);
      row->kind = hit.GetInt32("kind", 0);
      row->directory = hit.GetBool("directory", false);
      row->size = hit.GetInt64("size", 0);
      row->modifiedKnown = hit.HasInt64("date");
      row->modified = hit.GetInt64("date", 0);
      row->attributeCells.push_back({Listi::ParentLocation(row->path), B_STRING_TYPE});
      queryRows.push_back(std::move(row));
    });
    return true;
  }
  const bool initial = scanning;
  auto state = CaptureNavigation();
  std::string error = message.GetString("error", "");
  if (error.empty() && !message.GetBool("cancelled", false) && !receivedRows.Complete(message))
    error = "Incomplete search results. Reload to try again.";
  if (!error.empty() || message.GetBool("cancelled", false)) {
    Listi::RetireReadWorker(queryWorker);
    ClearRows();
    queryRows.clear();
    savedSelection.clear();
    restoring.reset();
    pendingVisit.reset();
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
  const bool rebuild = initial || !entries.MatchesSnapshot(queryRows);
  if (rebuild) {
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
    RestoreVisit();
  }
  queryRows.clear();
  if (message.GetBool("limited", false) || message.GetInt64("skipped", 0) > 0)
    ShowStatus("Partial search results. Some entries were skipped or a search limit was reached.");
  else if (rebuild && !initial && entries.Selected().size() < state.selection.size())
    ShowStatus("A selected original disappeared, changed identity or no longer matches.");
  if (!message.GetBool("live", false)) {
    scanTimer.reset();
    Listi::RetireReadWorker(queryWorker);
  }
  if (refreshPending)
    Reload();
  return true;
}
