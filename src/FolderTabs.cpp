// SPDX-License-Identifier: MIT
#include "StoredFields.h"
#include "FolderTabs.h"
#include "Pane.h"
#include "PanelAppearance.h"
#include "ViewVisibility.h"
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Window.h>
#include <algorithm>
#include <atomic>
#include <cmath>
namespace Listi {
namespace {
std::atomic<int64> nextTab{0};
constexpr uint32 kOverflow = 'tbov';
constexpr float kPadding = 8;
constexpr float kCloseWidth = 22;
constexpr float kOverflowWidth = 28;
constexpr float kMinTabWidth = 52;
constexpr float kMaxTabWidth = 160;

std::string ShortName(std::string label) {
  auto slash = label.rfind('/');
  if (slash != std::string::npos && slash + 1 < label.size())
    return label.substr(slash + 1);
  return label;
}
}
FolderTabs::FolderTabs(int32 index, Pane &files)
    : BView("folder tabs", B_WILL_DRAW | B_FRAME_EVENTS | B_SUPPORTS_LAYOUT |
                               B_FULL_UPDATE_ON_RESIZE),
      side(index), pane(files) {
  font_height metrics;
  GetFontHeight(&metrics);
  const float height = std::max(24.f, std::ceil(metrics.ascent + metrics.descent) + 10);
  SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
  // The strip takes one row but must never change the pane's width constraints.
  SetExplicitMinSize(BSize(0, height));
  SetExplicitPreferredSize(BSize(0, height));
  SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, height));
  SetExplicitAlignment(BAlignment(B_ALIGN_USE_FULL_WIDTH, B_ALIGN_VERTICAL_CENTER));
  tabs.push_back({++nextTab, {}, {}});
  Rebuild();
}
void FolderTabs::SaveActive() {
  tabs[active].state = pane.CaptureNavigation();
}
FolderTab FolderTabs::Snapshot() {
  SaveActive();
  return tabs[active];
}
BMessage FolderTabs::ArchiveTabs() {
  SaveActive();
  BMessage result;
  CheckStoredField(result.AddInt32("active", active));
  for (const auto &tab : tabs) {
    BMessage item;
    const auto &state = tab.state;
    CheckStoredField(item.AddString("alias", tab.alias.c_str()));
    CheckStoredField(item.AddString("location", state.location.c_str()));
    CheckStoredField(item.AddString("label", state.displayLabel.c_str()));
    CheckStoredField(item.AddString("filter", state.filter.c_str()));
    CheckStoredField(item.AddPoint("scroll", state.scroll));
    CheckStoredField(item.AddInt32("view", state.view));
    CheckStoredField(item.AddInt32("folderView", state.folderView));
    CheckStoredField(item.AddInt32("sort", state.sorting.column));
    CheckStoredField(item.AddBool("ascending", state.sorting.ascending));
    auto found = state.queries.find(state.location);
    if (found != state.queries.end()) {
      BMessage query;
      if (!found->second.WriteTo(query))
        throw std::runtime_error("Could not encode the complete saved search.");
      CheckStoredField(item.AddMessage("query", &query));
    }
    CheckStoredField(result.AddMessage("tab", &item));
  }
  return result;
}

void FolderTabs::RestoreTabs(const BMessage &archive) {
  std::vector<FolderTab> restored;
  const auto count = StoredCount(archive, "tab");
  if (!count) throw std::runtime_error("Missing working set tabs.");
  for (int32 i = 0; i < count; ++i) {
    const auto item = StoredItem(archive, "tab", i);
    FolderTab tab{++nextTab, item.GetString("alias", ""), {}};
    auto &state = tab.state;
    state.location = item.GetString("location", "/boot/home");
    state.displayLabel = item.GetString("label", "");
    state.filter = item.GetString("filter", "");
    state.workingSetView = true;
    state.view = item.GetInt32("view", -1);
    state.folderView = item.GetInt32("folderView", -1);
    state.sorting = {item.GetInt32("sort", 0), item.GetBool("ascending", true)};
    item.FindPoint("scroll", &state.scroll);
    if (StoredCount(item, "query", 1)) {
      const auto query = StoredItem(item, "query", 0);
      state.location = "@set-query:" + std::to_string(tab.id);
      state.queries.emplace(state.location, SavedSearch::FromMessage(query));
    }
    restored.push_back(std::move(tab));
  }
  if (!restored.empty()) {
    tabs = std::move(restored);
    active = std::min(size_t(archive.GetInt32("active", 0)), tabs.size() - 1);
    closed.reset();
    Restore();
  }
}

void FolderTabs::OpenDirectory(const char *path) {
  SaveActive();
  FolderTab tab{++nextTab, {}, {}};
  tab.state.location = path;
  Insert(std::move(tab));
  Restore();
}
void FolderTabs::Adopt(FolderTab tab) {
  tabs[active] = std::move(tab);
  Restore();
}
void FolderTabs::Insert(FolderTab tab) {
  ++active;
  tabs.insert(tabs.begin() + active, std::move(tab));
}
void FolderTabs::Remove(size_t index) {
  tabs.erase(tabs.begin() + index);
  if (tabs.empty())
    tabs.push_back({++nextTab, {}, {}});
  if (index < active)
    --active;
  active = std::min(active, tabs.size() - 1);
}
void FolderTabs::Restore() {
  pane.RestoreNavigation(tabs[active].state);
  Rebuild();
}
std::optional<FolderTab> FolderTabs::CopyTab(int64 id) {
  if (id == ActiveId())
    return Snapshot();
  for (const auto &tab : tabs)
    if (tab.id == id)
      return tab;
  return std::nullopt;
}
void FolderTabs::InsertTransferred(FolderTab tab) {
  SaveActive();
  Insert(std::move(tab));
  Restore();
}
void FolderTabs::ReleaseTab(int64 id) {
  for (size_t i = 0; i < Count(); ++i) {
    if (tabs[i].id != id)
      continue;
    const bool wasActive = i == active;
    Remove(i);
    if (wasActive)
      Restore();
    else
      Rebuild();
    return;
  }
}
std::string FolderTabs::DescribeTab(int64 id) const {
  for (size_t i = 0; i < Count(); ++i)
    if (tabs[i].id == id)
      return "Tab " + std::to_string(i + 1) + ": " + LabelAt(i);
  return {};
}
void FolderTabs::ForgetUnmountedDevice(int32 device) {
  if (device < 0)
    return;
  auto forget = [device](FolderTab &tab) {
    if (tab.state.folderDevice == device) {
      tab.state = {};
      tab.state.location = "@disks";
    }
  };
  for (size_t index = 0; index < tabs.size(); ++index)
    if (index != active)
      forget(tabs[index]);
  if (closed)
    forget(*closed);
  Rebuild();
}
void FolderTabs::Rename(const std::string &label) {
  tabs[active].alias = label;
  UpdateLabel();
}
bool FolderTabs::Change(uint32 cmd, int64 chosen) {
  if (cmd == kTabClose)
    return Close(chosen);
  if (cmd == kTabLeft || cmd == kTabRight) {
    const size_t other = cmd == kTabLeft ? (active ? active - 1 : active)
                                         : std::min(active + 1, tabs.size() - 1);
    std::swap(tabs[active], tabs[other]);
    active = other;
    Rebuild();
    return true;
  }
  if (cmd == kTabReopen && !closed)
    return false;
  switch (cmd) {
  case kTabNew: {
    SaveActive();
    const auto &current = tabs[active].state;
    FolderTab tab{++nextTab, {}, {}};
    tab.state.location = current.location;
    tab.state.queries = current.queries;
    auto path = pane.SelectedFolder();
    if (!path.empty())
      tab.state.location = path;
    Insert(std::move(tab));
    break;
  }
  case kTabDuplicate: {
    auto current = Snapshot();
    current.id = ++nextTab;
    Insert(std::move(current));
    break;
  }
  case kTabReopen:
    SaveActive();
    Insert(std::move(*closed));
    closed.reset();
    break;
  case kTabPrevious:
  case kTabNext: {
    if (tabs.size() == 1)
      return false;
    SaveActive();
    const size_t step = cmd == kTabNext ? 1 : tabs.size() - 1;
    active = (active + step) % tabs.size();
    break;
  }
  case kTabChoose: {
    auto found = std::find_if(tabs.begin(), tabs.end(), [chosen](const auto &t) { return t.id == chosen; });
    if (found == tabs.end() || found->id == ActiveId())
      return false;
    SaveActive();
    active = size_t(found - tabs.begin());
    break;
  }
  default: return false;
  }
  Restore();
  return true;
}
bool FolderTabs::Close(int64 id) {
  if (tabs.size() == 1)
    return false;
  size_t index = active;
  if (id != -1) {
    auto found = std::find_if(tabs.begin(), tabs.end(),
                              [id](const auto &tab) { return tab.id == id; });
    if (found == tabs.end())
      return false;
    index = size_t(found - tabs.begin());
  }
  const bool wasActive = index == active;
  if (wasActive)
    SaveActive();
  closed = std::move(tabs[index]);
  Remove(index);
  if (wasActive)
    Restore();
  else
    Rebuild();
  return true;
}
void FolderTabs::SetShown(bool filesPage, bool always) {
  pageVisible = filesPage;
  pinned = always;
  SetViewVisible(this, pageVisible && (pinned || Count() > 1 || CanReopen()));
}
void FolderTabs::Rebuild() {
  Arrange();
  SetShown(pageVisible, pinned);
}

void FolderTabs::UpdateLabel() {
  // Navigation can notify the window again after Restore() laid out the strip.
  // Only the active label can change without a structural rebuild.
  for (const auto &slot : slots) {
    if (slot.tab == active) {
      if (slot.label != LabelAt(active))
        Arrange();
      return;
    }
  }
}

std::string FolderTabs::LabelAt(size_t index) const {
  const auto &tab = tabs[index];
  if (!tab.alias.empty())
    return tab.alias;
  const auto location = index == active ? pane.LocationLabel() : tab.state.displayLabel;
  if (location.rfind("Search: ", 0) == 0)
    return location;
  return ShortName(location.empty() ? tab.state.location : location);
}

void FolderTabs::Arrange() {
  slots.clear();
  overflow = {};
  float available = std::max(0.f, Bounds().Width() + 1);
  const bool needsMenu = CanReopen() || tabs.size() * kMinTabWidth > available;
  const float menuWidth = needsMenu ? std::min(kOverflowWidth, available) : 0.f;
  available -= menuWidth;
  const auto visible = std::min(tabs.size(), size_t(available / kMinTabWidth));
  // Keep the active tab in the visible run. The menu gives direct access
  // to every tab when the pane is too narrow to show them all.
  const size_t first = active >= visible ? active - visible + 1 : 0;
  const float closeWidth = tabs.size() > 1 ? kCloseWidth : 0;
  const float bottom = Bounds().bottom;
  float left = 0;
  for (size_t offset = 0; offset < visible; ++offset) {
    const size_t i = first + offset;
    auto label = LabelAt(i);
    const float preferred = std::clamp(
        std::ceil(StringWidth(label.c_str())) + 2 * kPadding + closeWidth,
        kMinTabWidth, kMaxTabWidth);
    const float width = std::min(preferred, std::floor(available / (visible - offset)));
    BRect frame(left, 1, left + width - 1, bottom);
    BRect close;
    if (closeWidth > 0)
      close = BRect(frame.right - closeWidth + 1, frame.top, frame.right - 1, frame.bottom - 2);
    slots.push_back({i, frame, close, std::move(label)});
    left += width;
    available -= width;
  }
  if (menuWidth > 0)
    overflow = BRect(left, 1, left + menuWidth - 1, bottom);
  Invalidate();
}

void FolderTabs::FrameResized(float, float) {
  Arrange();
}

void FolderTabs::Draw(BRect) {
  const rgb_color panel = ui_color(B_PANEL_BACKGROUND_COLOR);
  const rgb_color text = ui_color(B_PANEL_TEXT_COLOR);
  const rgb_color accent = ui_color(B_KEYBOARD_NAVIGATION_COLOR);
  const rgb_color divider = PanelBlend(panel, text, .23f);
  const auto hovered = ActionAt(hover);
  font_height metrics;
  GetFontHeight(&metrics);
  const float baseline = std::floor((Bounds().bottom + metrics.ascent - metrics.descent) / 2);
  SetHighColor(divider);
  StrokeLine(Bounds().LeftBottom(), Bounds().RightBottom());
  for (const auto &slot : slots) {
    const int64 id = tabs[slot.tab].id;
    const bool selected = slot.tab == active;
    const bool highlighted = hovered == Action{kTabChoose, id};
    const rgb_color fill = selected ? PanelBlend(panel, accent, .13f)
                                    : PanelBlend(panel, text, highlighted ? .10f : .05f);
    SetHighColor(fill);
    FillRect(slot.frame);
    SetHighColor(divider);
    StrokeRect(slot.frame);
    if (selected) {
      SetHighColor(accent);
      FillRect(BRect(slot.frame.left + 1, slot.frame.bottom - 1,
                     slot.frame.right - 1, slot.frame.bottom));
    }
    if (slot.close.IsValid()) {
      if (hovered == Action{kTabClose, id}) {
        SetHighColor(PanelBlend(fill, text, .16f));
        FillRect(slot.close);
      }
      SetHighColor(text);
      const float x = std::floor((slot.close.left + slot.close.right) / 2);
      const float y = std::floor((slot.close.top + slot.close.bottom) / 2);
      StrokeLine(BPoint(x - 3, y - 3), BPoint(x + 3, y + 3));
      StrokeLine(BPoint(x + 3, y - 3), BPoint(x - 3, y + 3));
    }
    const float end = slot.close.IsValid() ? slot.close.left : slot.frame.right - kPadding;
    BString label(slot.label.c_str());
    TruncateString(&label, B_TRUNCATE_END, end - slot.frame.left - kPadding);
    SetHighColor(text);
    SetLowColor(fill);
    DrawString(label.String(), BPoint(slot.frame.left + kPadding, baseline));
  }
  if (overflow.IsValid()) {
    const auto fill = PanelBlend(panel, text, hovered.command == kOverflow ? .10f : .05f);
    SetHighColor(fill);
    FillRect(overflow);
    SetHighColor(divider);
    StrokeRect(overflow);
    SetHighColor(text);
    SetLowColor(fill);
    DrawString("…", BPoint(overflow.left + (overflow.Width() - StringWidth("…")) / 2, baseline));
  }
}

FolderTabs::Action FolderTabs::ActionAt(BPoint point) const {
  if (overflow.Contains(point))
    return {kOverflow};
  for (const auto &slot : slots) {
    if (slot.frame.Contains(point))
      return {slot.close.Contains(point) ? kTabClose : kTabChoose, tabs[slot.tab].id};
  }
  return {};
}

void FolderTabs::MouseDown(BPoint point) {
  const auto *event = Window()->CurrentMessage();
  const int32 buttons = event ? event->GetInt32("buttons", 0) : 0;
  if (buttons & B_SECONDARY_MOUSE_BUTTON) {
    pressed = {};
    const auto action = ActionAt(point);
    if (action.id != -1) {
      BMessage menu(kTabContext);
      menu.AddInt32("pane", side);
      menu.AddInt64("tab", action.id);
      menu.AddPoint("where", ConvertToScreen(point));
      Window()->PostMessage(&menu);
    }
    return;
  }
  if (!(buttons & B_PRIMARY_MOUSE_BUTTON))
    return;
  pressed = ActionAt(point);
  if (pressed.command)
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
}

void FolderTabs::MouseUp(BPoint point) {
  const auto action = pressed;
  pressed = {};
  if (!action.command || !(action == ActionAt(point)))
    return;
  if (action.command == kOverflow)
    ShowOverflow();
  else
    Post(action);
}

void FolderTabs::MouseMoved(BPoint point, uint32 transit, const BMessage *) {
  const auto old = ActionAt(hover);
  hover = transit == B_EXITED_VIEW || transit == B_OUTSIDE_VIEW ? BPoint(-1, -1) : point;
  if (!(old == ActionAt(hover)))
    Invalidate();
}

bool FolderTabs::GetToolTipAt(BPoint point, BToolTip **tip) {
  const auto action = ActionAt(point);
  std::string caption;
  if (action.command == kOverflow)
    caption = "Folder tabs";
  for (const auto &slot : slots) {
    if (tabs[slot.tab].id == action.id) {
      caption = slot.label;
      if (action.command == kTabClose)
        caption.insert(0, "Close ");
      break;
    }
  }
  SetToolTip(caption.empty() ? nullptr : caption.c_str());
  return BView::GetToolTipAt(point, tip);
}

void FolderTabs::ShowOverflow() {
  BPopUpMenu menu("Folder tabs", false, false);
  if (slots.size() < tabs.size()) {
    for (size_t i = 0; i < tabs.size(); ++i) {
      auto *message = new BMessage(kTabChoose);
      message->AddInt64("tab", tabs[i].id);
      auto *item = new BMenuItem(LabelAt(i).c_str(), message);
      menu.AddItem(item);
      item->SetMarked(i == active);
    }
    if (CanReopen())
      menu.AddSeparatorItem();
  }
  if (CanReopen())
    menu.AddItem(new BMenuItem("Reopen closed tab", new BMessage(kTabReopen)));
  if (auto *item = menu.Go(ConvertToScreen(overflow.LeftBottom() + BPoint(0, 2)), false, true)) {
    const auto *message = item->Message();
    Post({message->what, message->GetInt64("tab", -1)});
  }
}

void FolderTabs::Post(Action action) {
  BMessage message(action.command);
  message.AddInt32("pane", side);
  if (action.id != -1)
    message.AddInt64("tab", action.id);
  Window()->PostMessage(&message);
}
}
