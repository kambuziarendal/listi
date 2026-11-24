// SPDX-License-Identifier: MIT
#include "FolderTabs.h"
#include "Pane.h"
#include "ViewVisibility.h"
#include <Button.h>
#include <LayoutBuilder.h>
#include <Window.h>
#include <algorithm>
#include <atomic>
#include <GroupLayout.h>
namespace Listi {
namespace {
std::atomic<int64> nextTab{0};
std::string ShortName(std::string label) {
  auto slash = label.rfind('/');
  if (slash != std::string::npos && slash + 1 < label.size())
    return label.substr(slash + 1);
  return label;
}
}
FolderTabs::FolderTabs(int32 index, Pane &files)
    : BGroupView("folder tabs", B_HORIZONTAL, 2), side(index), pane(files) {
  tabs.push_back({++nextTab, {}, {}});
  Rebuild();
}
FolderTab FolderTabs::Snapshot() {
  tabs[active].state = pane.CaptureNavigation();
  return tabs[active];
}
BMessage FolderTabs::ArchiveTabs() {
  Snapshot();
  BMessage result;
  result.AddInt32("active", active);
  for (const auto &tab : tabs) {
    BMessage item;
    const auto &state = tab.state;
    item.AddString("alias", tab.alias.c_str());
    item.AddString("location", state.location.c_str());
    item.AddString("label", state.displayLabel.c_str());
    item.AddString("filter", state.filter.c_str());
    item.AddPoint("scroll", state.scroll);
    item.AddInt32("view", state.view);
    item.AddInt32("folderView", state.folderView);
    item.AddInt32("sort", state.sorting.column);
    item.AddBool("ascending", state.sorting.ascending);
    auto found = state.queries.find(state.location);
    if (found != state.queries.end()) {
      BMessage query;
      found->second.WriteTo(query);
      item.AddMessage("query", &query);
    }
    result.AddMessage("tab", &item);
  }
  return result;
}

void FolderTabs::RestoreTabs(const BMessage &archive) {
  std::vector<FolderTab> restored;
  BMessage item;
  for (int32 i = 0; archive.FindMessage("tab", i, &item) == B_OK; ++i) {
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
    BMessage query;
    if (item.FindMessage("query", &query) == B_OK) {
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
  Snapshot();
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
void FolderTabs::Remove() {
  tabs.erase(tabs.begin() + active);
  if (tabs.empty())
    tabs.push_back({++nextTab, {}, {}});
  active = std::min(active, tabs.size() - 1);
}
void FolderTabs::Restore() {
  pane.RestoreNavigation(tabs[active].state);
  Rebuild();
}
void FolderTabs::AfterDetach() {
  Remove();
  Restore();
}
void FolderTabs::Rename(const std::string &label) {
  tabs[active].alias = label;
  UpdateLabel();
}
bool FolderTabs::Change(uint32 cmd, int64 chosen) {
  if (cmd == kTabLeft || cmd == kTabRight) {
    const size_t other = cmd == kTabLeft ? (active ? active - 1 : active)
                                         : std::min(active + 1, tabs.size() - 1);
    std::swap(tabs[active], tabs[other]);
    active = other;
    Rebuild();
    return true;
  }
  if (cmd == kTabClose && tabs.size() == 1)
    return false;
  if (cmd == kTabReopen && !closed)
    return false;
  auto current = Snapshot();
  switch (cmd) {
  case kTabNew: {
    FolderTab tab{++nextTab, {}, {}};
    tab.state.location = current.state.location;
    tab.state.queries = current.state.queries;
    auto path = pane.SelectedFolder();
    if (!path.empty())
      tab.state.location = path;
    Insert(std::move(tab));
    break;
  }
  case kTabDuplicate:
    current.id = ++nextTab;
    Insert(std::move(current));
    break;
  case kTabClose:
    closed = std::move(current);
    Remove();
    break;
  case kTabReopen:
    Insert(std::move(*closed));
    closed.reset();
    break;
  case kTabPrevious: active = (active + tabs.size() - 1) % tabs.size(); break;
  case kTabNext: active = (active + 1) % tabs.size(); break;
  case kTabChoose: {
    auto found = std::find_if(tabs.begin(), tabs.end(), [chosen](const auto &t) { return t.id == chosen; });
    if (found == tabs.end() || found->id == current.id) {
      UpdateLabel();
      return false;
    }
    active = size_t(found - tabs.begin());
    break;
  }
  default: return false;
  }
  Restore();
  return true;
}
void FolderTabs::SetShown(bool filesPage, bool always) {
  pageVisible = filesPage;
  pinned = always;
  SetViewVisible(this, pageVisible && (pinned || Count() > 1 || CanReopen()));
}
void FolderTabs::Rebuild() {
  while (auto *view = ChildAt(0)) {
    GroupLayout()->RemoveView(view);
    delete view;
  }
  slots.clear();
  // Keep large tab collections from imposing a minimum width on the window.
  // Cycle controls move the visible segment when neighbouring tabs are hidden.
  const size_t first = active > 0 ? active - 1 : 0;
  auto add = [&](uint32 cmd, const char *text) {
    auto *msg = new BMessage(cmd);
    msg->AddInt32("pane", side);
    auto *button = new BButton(text, msg);
    AddChild(button);
    return button;
  };
  if (first > 0)
    add(kTabPrevious, "‹");
  for (size_t i = first; i < tabs.size() && i < first + 3; ++i) {
    auto *button = add(kTabChoose, "");
    button->Message()->AddInt64("tab", tabs[i].id);
    button->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    button->SetValue(i == active ? B_CONTROL_ON : B_CONTROL_OFF);
    slots.push_back({button, i});
  }
  if (first + 3 < tabs.size())
    add(kTabNext, "›");
  if (CanReopen())
    add(kTabReopen, "Reopen")->SetToolTip("Reopen the last closed tab in this pane");
  UpdateLabel();
  SetShown(pageVisible, pinned);
}
void FolderTabs::UpdateLabel() {
  for (auto &slot : slots) {
    const auto &tab = tabs[slot.tab];
    slot.button->SetValue(slot.tab == active ? B_CONTROL_ON : B_CONTROL_OFF);
    std::string full = tab.alias;
    if (full.empty()) {
      const auto label = slot.tab == active ? pane.LocationLabel() : tab.state.displayLabel;
      full = label.rfind("Search: ", 0) == 0 ? label : ShortName(label.empty() ? tab.state.location : label);
    }
    BString compact(full.c_str());
    if (compact.CountChars() > 12) {
      compact.TruncateChars(11);
      compact << "…";
    }
    if (compact != slot.button->Label()) {
      slot.button->SetLabel(compact.String());
    }
    slot.button->SetToolTip(full.c_str());
  }
}
}
