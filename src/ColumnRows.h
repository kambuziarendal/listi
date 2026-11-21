// SPDX-License-Identifier: MIT
#pragma once
#include <ColumnListView.h>

namespace Listi {
inline void ClearColumnRows(BColumnListView &list) {
  // Haiku's Clear() deletes rows but does not clear the keyboard focus row.
  if (auto *row = list.FocusRow()) {
    list.RemoveRow(row);
    delete row;
  }
  list.Clear();
  list.InvalidateLayout();
}
}
