// SPDX-License-Identifier: MIT
#pragma once
#include <TextView.h>
#include <ScrollView.h>

// SetText(), paste, keyboard input and undo all use these native edit hooks.
class ScrollTextView : public BTextView {
public:
  using BTextView::BTextView;

protected:
  void InsertText(const char *text, int32 length, int32 offset,
                  const text_run_array *runs) override {
    BTextView::InsertText(text, length, offset, runs);
    InvalidateScrollLayout();
  }
  void DeleteText(int32 start, int32 end) override {
    BTextView::DeleteText(start, end);
    InvalidateScrollLayout();
  }

private:
  void InvalidateScrollLayout() {
    if (auto scroll = dynamic_cast<BScrollView *>(Parent()))
      scroll->InvalidateLayout();
  }
};
