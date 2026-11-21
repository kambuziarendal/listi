// SPDX-License-Identifier: MIT
#pragma once
#include "AdaptiveColumnList.h"
#include <Messenger.h>
#include <vector>

namespace Listi {
struct SearchHit;

constexpr uint32 kSearchResultPicked = 'spik', kShowSearchResult = 'sshw';

class SearchResultsView : public Listi::AdaptiveColumnList {
public:
  SearchResultsView(BMessenger target, int32 pane);
  void SetReceiver(BHandler *target);
  void Add(const SearchHit &hit);
  void Order(int32 next);
  void Reconcile(const std::vector<SearchHit> &hits);
  const SearchHit *SelectedHit();
  void ShowOriginal(const SearchHit &hit);
  void UpdateColors();

private:
  struct HitRow;
  class ContextClick;

  bool Before(HitRow *left, HitRow *right, int32 requestedOrder) const;
  void SelectionChanged() override;

  BMessenger owner, receiver;
  int32 pane;
  int64 sequence = 0;
  int32 order = 0;
  BColumn *columns[2];
};
} // namespace Listi
