// SPDX-License-Identifier: MIT
#include "AdaptiveColumnList.h"
#include "ScrollTextView.h"
#include <Application.h>
#include <ColumnTypes.h>
#include <LayoutBuilder.h>
#include <StringItem.h>
#include <Window.h>
#include <cstdio>
#include <stdexcept>

namespace {
void Check(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}

bool Visible(BScrollBar *bar) { return !bar->IsHidden(bar); }

void CheckBars(BScrollBar *horizontal, BScrollBar *vertical, bool h, bool v) {
  Check(Visible(horizontal) == h, "wrong horizontal visibility");
  Check(Visible(vertical) == v, "wrong vertical visibility");
  for (auto bar : {horizontal, vertical}) {
    float low, high;
    bar->GetRange(&low, &high);
    Check((high > low) == Visible(bar), "visible bar and range disagree");
  }
}
}

int main() {
  BApplication app("application/x-vnd.Listi-adaptive-scroll-test");
  auto window = new BWindow(BRect(10, 10, 410, 310), "scroll test",
                            B_TITLED_WINDOW, B_AVOID_FOCUS);
  auto list = new BListView("side list");
  auto scroll = new Listi::AdaptiveScrollView("scroll", list, true);
  auto columns =
      new Listi::AdaptiveColumnList("details", B_WILL_DRAW, B_FANCY_BORDER);
  auto name = new BStringColumn("Name", 80, 20, 1000, B_TRUNCATE_END);
  columns->AddColumn(name, 0);
  auto text = new ScrollTextView("editor");
  text->SetInsets(4, 3, 4, 3);
  auto editor = new Listi::AdaptiveScrollView("editor scroll", text, true);
  BLayoutBuilder::Group<>(window, B_VERTICAL, 0)
      .Add(scroll)
      .Add(columns)
      .Add(editor);
  window->Show();
  snooze(50000);
  window->Lock();
  auto h = scroll->ScrollBar(B_HORIZONTAL);
  auto v = scroll->ScrollBar(B_VERTICAL);
  auto ch =
      static_cast<BScrollBar *>(columns->FindView("horizontal_scroll_bar"));
  auto cv = static_cast<BScrollBar *>(columns->FindView("vertical_scroll_bar"));
  try {
    auto th = editor->ScrollBar(B_HORIZONTAL);
    auto tv = editor->ScrollBar(B_VERTICAL);
    auto settleText = [&] {
      window->Unlock();
      snooze(50000);
      window->Lock();
    };
    text->SetWordWrap(false);
    text->SetText("short");
    settleText();
    CheckBars(th, tv, false, false);
    text->Insert(std::string(200, 'W').c_str());
    settleText();
    CheckBars(th, tv, true, false);
    text->SetWordWrap(true);
    editor->InvalidateLayout();
    settleText();
    CheckBars(th, tv, false, true);
    text->SetText("short again");
    settleText();
    CheckBars(th, tv, false, false);
    CheckBars(h, v, false, false);
    CheckBars(ch, cv, false, false);
    const float fullWidth = list->Bounds().Width();
    for (int i = 0; i < 60; ++i) {
      list->AddItem(new BStringItem("Short"));
      auto row = new BRow(22);
      row->SetField(new BStringField("file"), 0);
      columns->AddRow(row);
    }
    scroll->InvalidateLayout();
    scroll->Layout(true);
    columns->UpdateScrollBars();
    CheckBars(h, v, false, true);
    CheckBars(ch, cv, false, true);
    Check(list->Bounds().Width() < fullWidth,
          "vertical bar must consume width");
    auto wide = new BStringItem(std::string(200, 'W').c_str());
    list->AddItem(wide);
    name->SetWidth(800);
    scroll->InvalidateLayout();
    scroll->Layout(true);
    columns->UpdateScrollBars();
    CheckBars(h, v, true, true);
    CheckBars(ch, cv, true, true);
    v->SetValue(70);
    cv->SetValue(70);
    while (list->CountItems() > 1)
      delete list->RemoveItem(int32(0));
    columns->Clear();
    scroll->InvalidateLayout();
    scroll->Layout(true);
    columns->UpdateScrollBars();
    CheckBars(h, v, true, false);
    CheckBars(ch, cv, true, false);
    Check(v->Value() == 0 && cv->Value() == 0,
          "shrink must clear vertical offset");
    delete list->RemoveItem(int32(0));
    name->SetWidth(80);
    scroll->InvalidateLayout();
    scroll->Layout(true);
    columns->UpdateScrollBars();
    CheckBars(h, v, false, false);
    CheckBars(ch, cv, false, false);
    Check(list->Bounds().Width() == fullWidth, "hidden bar must release width");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    window->Quit();
    return 1;
  }
  window->Quit();
  return 0;
}
