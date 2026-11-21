// SPDX-License-Identifier: MIT
#pragma once
#include "AdaptiveScrollView.h"
#include <ColumnListView.h>
#include <MessageFilter.h>
#include <Messenger.h>

namespace Listi {
constexpr uint32 kScrollLayout = 'sbar';
class ColumnResizeFinished : public BMessageFilter {
  BHandler *target;

public:
  explicit ColumnResizeFinished(BHandler *owner)
      : BMessageFilter(B_MOUSE_UP), target(owner) {}
  filter_result Filter(BMessage *, BHandler **) override {
    BMessenger(target).SendMessage(kScrollLayout);
    return B_DISPATCH_MESSAGE;
  }
};
// Native column lists reserve both scrollbars in their child layout even
// when their ranges are zero. Reclaim that space for both headings and rows.
class AdaptiveColumnList : public BColumnListView {
public:
  AdaptiveColumnList(const char *name, uint32 flags, border_style border)
      : BColumnListView(name, flags, border) {
    auto bar = static_cast<BScrollBar *>(FindView("horizontal_scroll_bar"));
    bar->Target()->AddFilter(new ColumnResizeFinished(this));
  }
  void MessageReceived(BMessage *message) override {
    if (message->what == kScrollLayout)
      UpdateScrollBars();
    else
      BColumnListView::MessageReceived(message);
  }
  void UpdateScrollBars() { DoLayout(); }
  void DoLayout() override {
    if (arranging)
      return;
    arranging = true;
    BColumnListView::DoLayout();
    auto vertical = static_cast<BScrollBar *>(FindView("vertical_scroll_bar"));
    auto horizontal =
        static_cast<BScrollBar *>(FindView("horizontal_scroll_bar"));
    auto body = ScrollView();
    auto heading = horizontal->Target();
    const float verticalWidth = vertical->Frame().Width();
    const float horizontalHeight = horizontal->Frame().Height();
    const float fullWidth = body->Bounds().Width() + verticalWidth;
    const float fullHeight = body->Bounds().Height() + horizontalHeight;
    // Obtain the native column margins through PreferredSize, not private
    // constants. PreferredSize includes hidden columns, which we subtract.
    float contentWidth = BColumnListView::PreferredSize().width -
                         (Bounds().Width() - heading->Frame().Width());
    for (int32 i = 0; i < CountColumns(); ++i)
      if (!ColumnAt(i)->IsVisible())
        contentWidth -= ColumnAt(i)->Width();
    const float contentHeight = ContentHeight();
    // Start with both bars absent: one necessary bar can require the other,
    // but two mutually unnecessary bars should never keep each other visible.
    bool showHorizontal = contentWidth > fullWidth;
    const bool showVertical =
        contentHeight > fullHeight - (showHorizontal ? horizontalHeight : 0);
    if (showVertical)
      showHorizontal = contentWidth > fullWidth - verticalWidth;
    const float extraWidth = showVertical ? 0 : verticalWidth;
    const float extraHeight = showHorizontal ? 0 : horizontalHeight;
    body->ResizeBy(extraWidth, extraHeight);
    heading->ResizeBy(extraWidth, 0);
    vertical->ResizeBy(0, extraHeight);
    horizontal->ResizeBy(extraWidth, 0);
    SetBarVisible(vertical, showVertical);
    SetBarVisible(horizontal, showHorizontal);
    vertical->SetRange(0,
                       std::max(0.f, contentHeight - body->Bounds().Height()));
    horizontal->SetRange(
        0, std::max(0.f, contentWidth - heading->Bounds().Width()));
    arranging = false;
  }

protected:
  virtual float ContentHeight() {
    float height = 0;
    for (int32 i = 0; i < CountRows(); ++i)
      height += RowAt(i)->Height() + 1;
    return height;
  }

private:
  bool arranging = false;
};
} // namespace Listi
