// SPDX-License-Identifier: MIT
#include "SearchResultsView.h"
#include "Common.h"
#include "FileSearch.h"
#include <ColumnTypes.h>
#include <List.h>
#include <MenuItem.h>
#include <MessageFilter.h>
#include <PopUpMenu.h>
#include <algorithm>
#include <set>
#include <tuple>

namespace Listi {
struct SearchResultsView::HitRow : BRow {
  SearchHit hit;
  int64 arrival;
  HitRow(const SearchHit &value, int64 sequence)
      : BRow(24), hit(value), arrival(sequence) {
    size_t slash = hit.path.find_last_of('/');
    SetField(new BStringField(hit.path.substr(slash + 1).c_str()), 0);
    SetField(new BStringField(hit.path.substr(0, slash ? slash : 1).c_str()),
             1);
  }
};

class SearchResultsView::ContextClick : public BMessageFilter {
  SearchResultsView *results;

public:
  explicit ContextClick(SearchResultsView *view)
      : BMessageFilter(B_MOUSE_DOWN), results(view) {}

  filter_result Filter(BMessage *message, BHandler **) override {
    if (!(message->GetInt32("buttons", 0) & B_SECONDARY_MOUSE_BUTTON))
      return B_DISPATCH_MESSAGE;
    BPoint point;
    if (message->FindPoint("where", &point) != B_OK)
      return B_SKIP_MESSAGE;
    const auto row = static_cast<HitRow *>(results->RowAt(point));
    if (!row)
      return B_SKIP_MESSAGE;

    const SearchHit hit = row->hit;
    BPopUpMenu menu("Search result actions", false, false);
    menu.AddItem(new BMenuItem("Show original in panel", nullptr));
    if (menu.Go(results->ScrollView()->ConvertToScreen(point), false, true,
                false))
      results->ShowOriginal(hit);
    return B_SKIP_MESSAGE;
  }
};

bool SearchResultsView::Before(HitRow *left, HitRow *right,
                               int32 requestedOrder) const {
  if (requestedOrder != 0) {
    int32 key = requestedOrder <= 2 ? 0 : 1;
    int cmp = columns[key]->CompareFields(left->GetField(key),
                                          right->GetField(key));
    if (cmp != 0)
      return requestedOrder % 2 ? cmp < 0 : cmp > 0;
  }
  return left->arrival < right->arrival;
}

SearchResultsView::SearchResultsView(BMessenger target, int32 which)
    : AdaptiveColumnList("search results", B_WILL_DRAW | B_NAVIGABLE,
                      B_FANCY_BORDER),
      owner(target), pane(which) {
  // Haiku starts an unjoined native sort thread even on initial attachment.
  // It can outlive a rapidly closed window/tab. Sort only on our UI looper.
  SetSortingEnabled(false);
  columns[0] = new BStringColumn("Name", 220, 80, 1000, B_TRUNCATE_MIDDLE);
  columns[1] = new BStringColumn("Original folder", 440, 120, 2000,
                                 B_TRUNCATE_MIDDLE);
  AddColumn(columns[0], 0);
  AddColumn(columns[1], 1);
  SetSelectionMode(B_SINGLE_SELECTION_LIST);
  SetInvocationMessage(new BMessage(kShowSearchResult));
  ScrollView()->AddFilter(new ContextClick(this));
}

void SearchResultsView::ShowOriginal(const SearchHit &hit) {
  // The receiving pane rechecks identity after its asynchronous scan, too.
  BMessage reveal(kRevealSearch);
  reveal.AddInt32("pane", pane);
  reveal.AddString("path", hit.path.c_str());
  reveal.AddInt64("device", hit.device);
  reveal.AddInt64("node", hit.node);
  owner.SendMessage(&reveal);
}

void SearchResultsView::Add(const SearchHit &hit) {
  InvalidateLayout();
  auto row = new HitRow(hit, sequence++);
  if (!order) {
    AddRow(row);
    return;
  }
  int32 first = 0, last = CountRows();
  while (first < last) {
    int32 middle = first + (last - first) / 2;
    if (Before(static_cast<HitRow *>(RowAt(middle)), row, order))
      first = middle + 1;
    else
      last = middle;
  }
  AddRow(row, first);
}

void SearchResultsView::Order(int32 next) {
  if (next == order || next < 0 || next > 4)
    return;
  BList current, ordered;
  std::vector<HitRow *> rows;
  rows.reserve(CountRows());
  for (int32 i = 0; i < CountRows(); ++i) {
    auto row = static_cast<HitRow *>(RowAt(i));
    if (!current.AddItem(row))
      return;
    rows.push_back(row);
  }
  std::sort(rows.begin(), rows.end(), [this, next](HitRow *a, HitRow *b) {
    return Before(a, b, next);
  });
  for (auto row : rows)
    if (!ordered.AddItem(row))
      return;
  order = next;
  auto selected = CurrentSelection();
  auto focus = FocusRow();
  RemoveRows(&current);
  AddRows(&ordered, -1);
  if (focus)
    SetFocusRow(focus, false);
  if (selected)
    AddToSelection(selected);
}

void SearchResultsView::SelectionChanged() {
  BColumnListView::SelectionChanged();
  if (receiver.IsValid())
    receiver.SendMessage(kSearchResultPicked);
}

void SearchResultsView::SetReceiver(BHandler *target) {
  receiver = BMessenger(target);
  SetTarget(target);
}

const SearchHit *SearchResultsView::SelectedHit() {
  const auto row = static_cast<const HitRow *>(CurrentSelection());
  return row ? &row->hit : nullptr;
}

void SearchResultsView::Reconcile(const std::vector<SearchHit> &hits) {
  InvalidateLayout();
  using Key = std::tuple<std::string, int64, int64>;
  std::set<Key> next;
  for (const auto &hit : hits)
    next.emplace(hit.path, hit.device, hit.node);
  for (int32 i = CountRows() - 1; i >= 0; --i) {
    auto row = static_cast<HitRow *>(RowAt(i));
    auto found = next.find(Key(row->hit.path, row->hit.device, row->hit.node));
    if (found == next.end()) {
      RemoveRow(row);
      delete row;
    } else {
      // Retain the row and selection only for an identical path and identity.
      next.erase(found);
    }
  }
  for (const auto &hit : hits)
    if (next.erase(Key(hit.path, hit.device, hit.node)))
      Add(hit);
}

void SearchResultsView::UpdateColors() {
  SetColor(B_COLOR_BACKGROUND, ui_color(B_DOCUMENT_BACKGROUND_COLOR));
  SetColor(B_COLOR_TEXT, ui_color(B_DOCUMENT_TEXT_COLOR));
  SetColor(B_COLOR_SELECTION, ui_color(B_CONTROL_HIGHLIGHT_COLOR));
  SetColor(B_COLOR_SELECTION_TEXT, ui_color(B_CONTROL_TEXT_COLOR));
  SetColor(B_COLOR_NON_FOCUS_SELECTION,
           tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), .8f));
  Invalidate();
}
} // namespace Listi
