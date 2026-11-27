// SPDX-License-Identifier: MIT
#pragma once
#include <Message.h>

namespace Listi {
// Count decoded rows, not displayed rows: parents, filters and interrupted
// volumes can legitimately suppress rows after decoding. A bad batch poisons
// only this scan; a later completion must not silently certify its prefix.
struct ScanRows {
  int64 decoded = 0;
  bool failed = false;

  template<class Visit>
  void Read(const BMessage &batch, const char *field, Visit visit) {
    type_code type;
    int32 count;
    if (failed)
      return;
    if (batch.GetInfo(field, &type, &count) != B_OK || type != B_MESSAGE_TYPE ||
        count <= 0) {
      failed = true;
      return;
    }
    for (int32 i = 0; i < count; ++i) {
      BMessage row;
      if (batch.FindMessage(field, i, &row) != B_OK) {
        failed = true;
        return;
      }
      visit(row);
      ++decoded;
    }
  }

  bool Complete(const BMessage &done) const {
    return !failed && done.GetInt64("rowTotal", -1) == decoded;
  }
};
}
