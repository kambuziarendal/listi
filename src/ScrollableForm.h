// SPDX-License-Identifier: MIT
#pragma once
#include <ScrollBar.h>
#include <ScrollView.h>
#include <Font.h>
#include <algorithm>

namespace Listi {
class ScrollableForm : public BScrollView {
  BView *content;

public:
  explicit ScrollableForm(BView *page)
      : BScrollView("settings page scroll",
                    new BView("settings viewport", B_SCROLL_VIEW_AWARE),
                    0, false, true, B_NO_BORDER),
        content(page) {
    Target()->AddChild(content);
  }

  BSize MinSize() override {
    const auto bar = ScrollBar(B_VERTICAL);
    return {content->MinSize().width + bar->PreferredSize().width + 1,
            std::max(64.f, bar->MinSize().height)};
  }

  void DoLayout() override {
    BScrollView::DoLayout();
    const BRect bounds = Target()->Bounds();
    if (!bounds.IsValid())
      return;
    float minimum = content->MinSize().height;
    if (content->HasHeightForWidth())
      content->GetHeightForWidth(bounds.Width(), &minimum, nullptr, nullptr);
    const float height = std::max(bounds.Height(), minimum);
    content->ResizeTo(bounds.Width(), height);

    auto bar = ScrollBar(B_VERTICAL);
    bar->SetRange(0, height - bounds.Height());
    bar->SetProportion((bounds.Height() + 1) / (height + 1));
    bar->SetSteps(be_plain_font->Size() * 1.33f, bounds.Height());
  }

  void Reveal(BView *view) {
    BView *ancestor = view;
    while (ancestor && ancestor != content)
      ancestor = ancestor->Parent();
    if (!ancestor)
      return;
    const BRect bounds = Target()->Bounds();
    const BRect field =
        Target()->ConvertFromScreen(view->ConvertToScreen(view->Bounds()));
    const float top = bounds.top;
    if (field.top < top)
      ScrollBar(B_VERTICAL)->SetValue(field.top);
    else if (field.bottom > bounds.bottom)
      ScrollBar(B_VERTICAL)->SetValue(top + field.bottom - bounds.bottom);
  }
};
}
