// SPDX-License-Identifier: MIT
#pragma once
#include "ScrollTextView.h"
// BTextView stores glyph colours independently of BView's HighColor.
// Refresh existing text runs as well as the background on a live theme change.
class ThemeTextView : public ScrollTextView {
public:
  ThemeTextView(const char *name, color_which background,
                color_which foreground)
      : ScrollTextView(name), foreground(foreground) {
    SetViewUIColor(background);
    SetLowUIColor(background);
    SetHighUIColor(foreground);
  }
  void AttachedToWindow() override {
    BTextView::AttachedToWindow();
    RefreshTextColor();
  }
  void MessageReceived(BMessage *message) override {
    BTextView::MessageReceived(message);
    if (message->what == B_COLORS_UPDATED)
      RefreshTextColor();
  }

private:
  color_which foreground;
  void RefreshTextColor() {
    rgb_color color = ui_color(foreground);
    // The no-range overload changes only the selection/insertion style.
    // Recolor every existing run, then set the style for newly inserted text.
    SetFontAndColor(0, TextLength(), nullptr, B_FONT_ALL, &color);
    SetFontAndColor(nullptr, B_FONT_ALL, &color);
    Invalidate();
  }
};
