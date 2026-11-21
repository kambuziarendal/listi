// SPDX-License-Identifier: MIT
#pragma once
#include <ListView.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <TextView.h>
#include <algorithm>
#include <functional>

namespace Listi {
inline void SetBarVisible(BScrollBar *bar, bool visible) {
  if (bar && visible == bar->IsHidden(bar))
    visible ? bar->Show() : bar->Hide();
}

// Measure content in pixels for the proposed width (grids may reflow).
class AdaptiveScrollView : public BScrollView {
public:
  using Measure = std::function<BSize(float)>;
  AdaptiveScrollView(const char *name, BView *target, bool horizontal,
                     bool vertical, Measure measure,
                     border_style border = B_FANCY_BORDER)
      : BScrollView(name, target, 0, horizontal, vertical, border),
        measure(std::move(measure)) {}

  AdaptiveScrollView(const char *name, BListView *list, bool horizontal)
      : AdaptiveScrollView(name, list, horizontal, true, [list](float) {
          float width = 0, height = 0;
          if (list->CountItems() > 0) {
            list->GetPreferredSize(&width, &height);
            ++height; // Haiku returns the bottom coordinate of the last item.
          }
          return BSize(width, height);
        }) {}

  AdaptiveScrollView(const char *name, BTextView *text, bool horizontal,
                     border_style border = B_FANCY_BORDER)
      : AdaptiveScrollView(
            name, text, horizontal, true,
            [text](float width) {
              // Unwrapped text has a natural width. Wrapped text must be
              // measured after Haiku applies its own glyph, tab and line metrics.
              if (text->DoesWordWrap() && text->Bounds().Width() != width - 1) {
                text->ResizeTo(width - 1, text->Bounds().Height());
                text->Layout(true);
              }
              float left, top, right, bottom;
              text->GetInsets(&left, &top, &right, &bottom);
              const BRect rect = text->TextRect();
              return BSize(
                  text->DoesWordWrap() ? 0 : rect.Width() + left + right + 1,
                  rect.Height() + top + bottom + 1);
            },
            border) {
    textTarget = text;
  }

protected:
  void DoLayout() override {
    auto horizontal = ScrollBar(B_HORIZONTAL);
    auto vertical = ScrollBar(B_VERTICAL);
    // Reuse the native default frames on content-only updates. Resetting the
    // target to its narrow size on every layout would queue endless resizes.
    if (lastBounds != Bounds()) {
      BScrollView::DoLayout();
      lastBounds = Bounds();
      targetSize = Target()->Bounds().Size();
      if (horizontal)
        horizontalWidth = horizontal->Frame().Width();
      if (vertical)
        verticalHeight = vertical->Frame().Height();
    }
    const float gap = Border() == B_NO_BORDER ? 1 : 0;
    const float reclaimedWidth =
        vertical ? vertical->PreferredSize().width + gap : 0;
    const float reclaimedHeight =
        horizontal ? horizontal->PreferredSize().height + gap : 0;
    const float fullWidth = targetSize.width + reclaimedWidth + 1;
    const float fullHeight = targetSize.height + reclaimedHeight + 1;
    // Trying both text widths can temporarily shorten a wrapped document.
    // Preserve the reader's offset, clamped only to the final scroll range.
    const bool reflow = textTarget && textTarget->DoesWordWrap() &&
                        Target()->Bounds().Width() != fullWidth - 1;
    const float originalY = reflow ? vertical->Value() : 0;
    BSize content = measure(fullWidth);
    bool showH = horizontal && content.width > fullWidth;
    bool showV =
        vertical && content.height > fullHeight - (showH ? reclaimedHeight : 0);
    if (showV) {
      content = measure(fullWidth - reclaimedWidth);
      showH = horizontal && content.width > fullWidth - reclaimedWidth;
    }
    const float extraWidth = showV ? 0 : reclaimedWidth;
    const float extraHeight = showH ? 0 : reclaimedHeight;
    if (horizontal)
      horizontal->ResizeTo(horizontalWidth + extraWidth,
                           horizontal->Frame().Height());
    if (vertical)
      vertical->ResizeTo(vertical->Frame().Width(),
                         verticalHeight + extraHeight);
    Target()->ResizeTo(targetSize.width + extraWidth,
                       targetSize.height + extraHeight);
    SetBarVisible(horizontal, showH);
    SetBarVisible(vertical, showV);
    if (horizontal)
      SetRange(horizontal, content.width, Target()->Bounds().Width() + 1);
    if (vertical)
      SetRange(vertical, content.height, Target()->Bounds().Height() + 1);
    if (reflow)
      vertical->SetValue(originalY);
  }

private:
  static void SetRange(BScrollBar *bar, float content, float viewport) {
    bar->SetRange(0, std::max(0.f, content - viewport));
    bar->SetProportion(content > 0 ? std::clamp(viewport / content, 0.f, 1.f)
                                   : 1.f);
  }
  Measure measure;
  BTextView *textTarget = nullptr;
  BRect lastBounds;
  BSize targetSize;
  float horizontalWidth = 0, verticalHeight = 0;
};
} // namespace Listi
