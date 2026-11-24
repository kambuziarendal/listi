// SPDX-License-Identifier: MIT
#include "WorkspaceLayout.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

void WorkspaceLayout::Load(const BMessage &m) {
  *this = WorkspaceLayout{};
  // Legacy numeric values remain authoritative when present: an older app may
  // have changed them since our last save. New-only settings use ASCII IDs.
  int32 stored;
  if (m.FindInt32("workspace", &stored) == B_OK) {
    workspace = stored >= 0 && stored <= 3 ? static_cast<Workspace>(stored)
                                           : Workspace::tviskipt;
  } else if (m.HasString("workspaceId")) {
    workspace = Workspace::tviskipt;
    for (auto value : {Workspace::tviskipt, Workspace::einn, Workspace::askr,
                       Workspace::smidja})
      if (!strcmp(m.GetString("workspaceId", ""), WorkspaceId(value)))
        workspace = value;
  } else {
    workspace = m.GetBool("simpleWorkspace", false) ? Workspace::einn
                                                    : Workspace::tviskipt;
  }
  singlePane = m.GetInt32("singlePane", 0) == 1 ? 1 : 0;
  for (auto w : {Workspace::tviskipt, Workspace::einn, Workspace::askr,
                 Workspace::smidja}) {
    BMessage view;
    bool hasView = m.FindMessage(WorkspaceId(w), &view) == B_OK;
    const int32 fallback = DefaultMode(w);
    int32 mode = view.GetInt32("default", fallback);
    defaultModes[int(w)] = mode >= 0 && mode < ViewModeCount ? mode : fallback;
    for (int pane = 0; pane < 2; ++pane) {
      const char *key = pane ? "rightMode" : "leftMode";
      int32 saved = hasView ? view.GetInt32(key, -1) : m.GetInt32(key, -1);
      // Legacy Details was always saved, even when the user never chose a mode.
      if (!hasView && saved == 0)
        saved = -1;
      paneOverrides[int(w)][pane] =
          saved >= 0 && saved < ViewModeCount ? saved : -1;
    }
  }
  paneTree[0] = m.GetBool("leftTree", false);
  paneTree[1] = m.GetBool("rightTree", false);
  const char *favoriteKeys[] = {"smidjaLeftFavorites", "smidjaRightFavorites"};
  const char *workbenchKeys[] = {"workbenchLeftTree", "workbenchRightTree"};
  const char *treeKeys[] = {"smidjaLeftTree", "smidjaRightTree"};
  for (int pane = 0; pane < 2; ++pane) {
    const bool shown =
        m.GetBool(workbenchKeys[pane], m.GetBool(treeKeys[pane], true));
    const auto legacy = m.GetBool(favoriteKeys[pane], false)
                            ? SidebarMode::Favorites : SidebarMode::FolderTree;
    const auto loadMode = [&](const char *key, SidebarMode fallback) {
      const int32 value = m.GetInt32(key, pane, static_cast<int32>(fallback));
      return IsVisibleSidebar(value) ? static_cast<SidebarMode>(value) : fallback;
    };
    smidjaRestore[pane] = loadMode("smidjaSidebarMode", legacy);
    smidjaSidebar[pane] = shown ? smidjaRestore[pane] : SidebarMode::Hidden;
    askrRestore[pane] = loadMode("askrSidebarMode", SidebarMode::FolderTree);
    askrSidebar[pane] = m.GetBool("askrSidebarVisible", pane, true)
                            ? askrRestore[pane] : SidebarMode::Hidden;
  }
  preview = m.GetBool("preview", false);
  smidjaPreview =
      m.GetBool("workbenchPreview", m.GetBool("smidjaPreview", true));

  buttons = m.GetBool("buttons", true);
  fullActionButtons = m.GetBool("fullActionButtons", false);
  command = m.GetBool("command", false);
  bankTop = m.GetBool("bankTop", false);
  commandTop = m.GetBool("commandTop", false);
  inspectorLeft = m.GetBool("inspectorLeft", false);
  panelsStacked = m.GetBool("panelsStacked", false);
  treeRight = m.GetBool("treeRight", false);
  filterBottom = m.GetBool("filterBottom", false);
  adaptive = m.GetBool("adaptive", false);

  BMessage bank;
  if (m.FindMessage("actionBank", &bank) == B_OK) {
    const int32 count = bank.GetInt32("count", -1);
    if (count >= 0 && count <= ActionButtonCount) {
      bool seen[ActionButtonCount] = {};
      std::vector<int32> selection;
      for (int32 i = 0; i < count; ++i) {
        int32 action;
        if (bank.FindInt32("action", i, &action) != B_OK || action < 0 ||
            action >= ActionButtonCount || seen[action])
          break;
        seen[action] = true;
        selection.push_back(action);
      }
      if (static_cast<int32>(selection.size()) == count)
        actionButtons = std::move(selection);
    }
  }
  float weights[3];
  for (int32 i = 0; i < 3; ++i)
    if (m.FindFloat("splitWeight", i, &weights[i]) != B_OK ||
        !std::isfinite(weights[i]) || weights[i] <= 0)
      return;
  float total = weights[0] + weights[1] + weights[2];
  if (!std::isfinite(total))
    return;
  for (int32 i = 0; i < 3; ++i)
    if (weights[i] / total < 0.01f)
      return;
  for (int32 i = 0; i < 3; ++i)
    splitWeights[i] = weights[i] / total;
}

void WorkspaceLayout::Archive(BMessage &m) const {
  // Compatibility aliases support upgrades and rollback without a migration.
  // The pre-enum single-pane fallback must never reveal a hidden target.
  m.AddString("workspaceId", WorkspaceId(workspace));
  m.AddBool("simpleWorkspace", SinglePane());
  m.AddInt32("workspace", static_cast<int32>(workspace));
  m.AddInt32("singlePane", singlePane);
  // Old versions have no column mode; use Details rather than a thumbnail alias.
  m.AddInt32("leftMode", PaneMode(0) >= MillerMode ? 0 : PaneMode(0));
  m.AddInt32("rightMode", PaneMode(1) >= MillerMode ? 0 : PaneMode(1));
  for (auto w : {Workspace::tviskipt, Workspace::einn, Workspace::askr,
                 Workspace::smidja}) {
    BMessage view;
    view.AddInt32("default", DefaultMode(w));
    view.AddInt32("leftMode", paneOverrides[int(w)][0]);
    view.AddInt32("rightMode", paneOverrides[int(w)][1]);
    m.AddMessage(WorkspaceId(w), &view);
  }
  m.AddBool("leftTree", paneTree[0]);
  m.AddBool("rightTree", paneTree[1]);
  for (int pane = 0; pane < 2; ++pane) {
    const bool askrVisible = askrSidebar[pane] != SidebarMode::Hidden;
    m.AddBool("askrSidebarVisible", askrVisible);
    m.AddInt32("askrSidebarMode", static_cast<int32>(
        askrVisible ? askrSidebar[pane] : askrRestore[pane]));
    m.AddInt32("smidjaSidebarMode", static_cast<int32>(
        smidjaSidebar[pane] != SidebarMode::Hidden ? smidjaSidebar[pane]
                                                  : smidjaRestore[pane]));
  }
  m.AddBool("smidjaLeftFavorites", smidjaSidebar[0] == SidebarMode::Favorites);
  m.AddBool("smidjaRightFavorites", smidjaSidebar[1] == SidebarMode::Favorites);
  // Older versions show a tree in place of favorites, without changing files.
  const bool leftVisible = smidjaSidebar[0] != SidebarMode::Hidden;
  const bool rightVisible = smidjaSidebar[1] != SidebarMode::Hidden;
  m.AddBool("smidjaLeftTree", leftVisible);
  m.AddBool("smidjaRightTree", rightVisible);
  m.AddBool("smidjaPreview", smidjaPreview);
  m.AddBool("workbenchLeftTree", leftVisible);
  m.AddBool("workbenchRightTree", rightVisible);
  m.AddBool("preview", preview);
  m.AddBool("workbenchPreview", smidjaPreview);

  m.AddBool("buttons", buttons);
  m.AddBool("fullActionButtons", fullActionButtons);
  m.AddBool("command", command);
  m.AddBool("bankTop", bankTop);
  m.AddBool("commandTop", commandTop);
  m.AddBool("inspectorLeft", inspectorLeft);
  m.AddBool("panelsStacked", panelsStacked);
  m.AddBool("treeRight", treeRight);
  m.AddBool("filterBottom", filterBottom);
  m.AddBool("adaptive", adaptive);
  BMessage bank;
  bank.AddInt32("count", static_cast<int32>(actionButtons.size()));
  for (int32 button : actionButtons)
    bank.AddInt32("action", button);
  m.AddMessage("actionBank", &bank);
  for (float weight : splitWeights)
    m.AddFloat("splitWeight", weight);
}

namespace {
std::string ProfileName(const std::string &raw) {
  auto first = raw.find_first_not_of(" \t\r\n");
  auto name =
      first == std::string::npos
          ? ""
          : raw.substr(first, raw.find_last_not_of(" \t\r\n") - first + 1);
  if (name.empty() || name.size() > 80 ||
      std::any_of(name.begin(), name.end(),
                  [](unsigned char c) { return c < 32 || c == 127; }))
    throw std::runtime_error(
        "Use a profile name of 1 to 80 bytes without control characters.");
  return name;
}
} // namespace

const WorkspaceProfile *WorkspaceProfiles::Find(const std::string &name) const {
  auto found = std::find_if(items.begin(), items.end(), [&](const auto &item) {
    return item.name == name;
  });
  return found == items.end() ? nullptr : &*found;
}

WorkspaceProfile &WorkspaceProfiles::Require(const std::string &name) {
  const auto *item = Find(name);
  if (!item)
    throw std::runtime_error("That workspace profile no longer exists.");
  return items[item - items.data()];
}

void WorkspaceProfiles::Add(const std::string &raw,
                            const WorkspaceLayout &layout) {
  auto name = ProfileName(raw);
  if (Find(name))
    throw std::runtime_error("That workspace profile name is already in use.");
  if (items.size() == Limit)
    throw std::runtime_error("Workspace profile limit reached (32).");
  items.push_back({name, layout});
}

void WorkspaceProfiles::Update(const std::string &name,
                               const WorkspaceLayout &layout) {
  Require(name).layout = layout;
}

void WorkspaceProfiles::Rename(const std::string &oldName,
                               const std::string &raw) {
  auto &item = Require(oldName);
  auto name = ProfileName(raw);
  if (name != oldName && Find(name))
    throw std::runtime_error("That workspace profile name is already in use.");
  item.name = name;
}

void WorkspaceProfiles::Remove(const std::string &name) {
  auto &item = Require(name);
  items.erase(items.begin() + (&item - items.data()));
}

void WorkspaceProfiles::Load(const BMessage &settings) {
  items.clear();
  BMessage profiles;
  if (settings.FindMessage("workspaceProfiles", &profiles) != B_OK ||
      profiles.GetInt32("version", 0) != 1)
    return;
  for (int32 i = 0; i < int32(Limit); ++i) {
    BMessage item, state;
    if (profiles.FindMessage("profile", i, &item) != B_OK)
      break;
    if (item.FindMessage("layout", &state) != B_OK)
      continue;
    WorkspaceLayout layout;
    layout.Load(state);
    try {
      Add(item.GetString("name", ""), layout);
    } catch (const std::runtime_error &) {
      // A malformed or duplicate profile does not discard its siblings.
    }
  }
}

void WorkspaceProfiles::Archive(BMessage &settings) const {
  BMessage profiles;
  profiles.AddInt32("version", 1);
  for (const auto &profile : items) {
    BMessage item, layout;
    profile.layout.Archive(layout);
    item.AddString("name", profile.name.c_str());
    item.AddMessage("layout", &layout);
    profiles.AddMessage("profile", &item);
  }
  settings.AddMessage("workspaceProfiles", &profiles);
}
