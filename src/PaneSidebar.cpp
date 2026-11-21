// SPDX-License-Identifier: MIT
#include "PaneSidebar.h"
#include "FolderTree.h"
#include "NavigationHistory.h"
#include "Pane.h"
#include "Shelf.h"
#include "ShelfWindow.h"
#include "VirtualFile.h"
#include <CardLayout.h>
#include <GroupLayout.h>
#include <GroupView.h>
#include <ListView.h>
#include <MenuItem.h>
#include <MessageFilter.h>
#include <PopUpMenu.h>
#include "AdaptiveScrollView.h"
#include <StringItem.h>
#include <Window.h>
#include <algorithm>
#include <string_view>
#include <utility>

namespace {
constexpr uint32 kFavoriteNavigate = 'fnav', kRecentNavigate = 'rnav',
                 kRemoveShortcut = 'prms', kRemoveShelfEntry = 'sfre',
                 kSidebarShelfDrop = 'sfsd';

constexpr const char *kPlaceCommands[] = {
    "Save this place...", "Rename this saved place...", "Forget this saved place..."};
constexpr int32 kPlaceCommandCount = sizeof(kPlaceCommands) / sizeof(kPlaceCommands[0]);
constexpr int32 kFirstPlace = kPlaceCommandCount + 1; // After the separator.

class SidebarItem : public BStringItem {
public:
  SidebarItem(const char *label, const std::string &path)
      : BStringItem(label), path(path) {}

  const std::string path;
};

class SidebarInput : public BMessageFilter {
public:
  explicit SidebarInput(PaneSidebar *sidebar)
      : BMessageFilter(B_MOUSE_DOWN), sidebar(sidebar) {}

  filter_result Filter(BMessage *message, BHandler **target) override {
    if (!(message->GetInt32("buttons", 0) & B_SECONDARY_MOUSE_BUTTON))
      return B_DISPATCH_MESSAGE;
    if (auto view = dynamic_cast<BView *>(*target)) {
      if (sidebar->ContextMenu(view, message->GetPoint("where", {})))
        return B_SKIP_MESSAGE;
    }
    return B_DISPATCH_MESSAGE;
  }

private:
  PaneSidebar *sidebar;
};

void InstallSidebarInput(PaneSidebar *sidebar, BView *view) {
  view->AddFilter(new SidebarInput(sidebar));
  for (auto child = view->ChildAt(0); child; child = child->NextSibling())
    InstallSidebarInput(sidebar, child);
}

void ClearSidebarItems(BListView &list) {
  while (auto item = list.RemoveItem(int32(0)))
    delete item;
}

class PlacesMenu : public BMenu {
public:
  explicit PlacesMenu(Pane *owner) : BMenu("Places"), pane(owner) {}

  void AttachedToWindow() override {
    BMenu::AttachedToWindow();
    // Menu tracking polls GetMouse. Leave mouse-down delivery to this view
    // instead of letting the tracking thread consume it from the queue.
    SetEventMask(EventMask(), B_NO_POINTER_HISTORY);
  }

  void MouseDown(BPoint point) override {
    const auto event = Window()->CurrentMessage();
    if (event && (event->GetInt32("buttons", 0) & B_SECONDARY_MOUSE_BUTTON)) {
      for (int32 i = 0; i < CountItems(); ++i) {
        const auto item = ItemAt(i);
        const auto message = item->Message();
        if (!item->Frame().Contains(point) || !message ||
            message->what != kNavigate || !message->GetBool("place", false))
          continue;

        BMessage remove(kRemoveShortcut);
        remove.AddString("path", message->GetString("path", ""));
        const char escape = B_ESCAPE;
        KeyDown(&escape, 1);
        BMessenger(pane).SendMessage(&remove, (BHandler *)nullptr, 0);
        return;
      }
    }
    BMenu::MouseDown(point);
  }

private:
  Pane *pane;
};

class ShelfSidebarList : public BListView {
public:
  explicit ShelfSidebarList(Pane *owner)
      : BListView("sidebar shelf"), pane(owner) {}
  ~ShelfSidebarList() override { ClearSidebarItems(*this); }

  void MessageReceived(BMessage *message) override {
    if (message->WasDropped()) {
      BMessage drop(*message);
      drop.what = kSidebarShelfDrop;
      pane->MessageReceived(&drop);
    } else {
      BListView::MessageReceived(message);
    }
  }

private:
  Pane *pane;
};

class DestinationList : public BListView {
public:
  DestinationList(Pane *owner, const char *name, uint32 navigateMessage)
      : BListView(name), pane(owner), navigateMessage(navigateMessage) {}
  ~DestinationList() override { ClearSidebarItems(*this); }

  void MouseDown(BPoint point) override {
    const auto event = Window()->CurrentMessage();
    const bool primary =
        event && event->GetInt32("buttons", 0) == B_PRIMARY_MOUSE_BUTTON &&
        event->GetInt32("clicks", 1) == 1;
    const int32 clicked = IndexOf(point);
    BListView::MouseDown(point);
    if (primary && clicked >= 0 && clicked == CurrentSelection())
      OpenSelection();
  }

  void KeyDown(const char *bytes, int32 length) override {
    if (length == 1 && bytes[0] == B_TAB)
      Window()->PostMessage(kSwitch);
    else if (length == 1 && bytes[0] == B_ENTER)
      OpenSelection();
    else
      BListView::KeyDown(bytes, length);
  }

  bool GetToolTipAt(BPoint point, BToolTip **tip) override {
    if (navigateMessage == kRecentNavigate) {
      auto item = dynamic_cast<SidebarItem *>(ItemAt(IndexOf(point)));
      SetToolTip(item ? item->path.c_str() : nullptr);
    }
    return BListView::GetToolTipAt(point, tip);
  }

private:
  Pane *pane;
  uint32 navigateMessage;

  void OpenSelection() {
    auto item = dynamic_cast<SidebarItem *>(ItemAt(CurrentSelection()));
    if (!item)
      return;
    BMessage open(navigateMessage);
    open.AddString("path", item->path.c_str());
    Window()->PostMessage(&open, pane);
  }
};

} // namespace

PaneSidebar::PaneSidebar(Pane &pane, int32 paneId, BMenu &goMenu,
                         BMenu &viewMenu,
                         const Listi::NavigationHistory &history,
                         std::shared_ptr<Listi::Shelf> shelf)
    : BCardView("navigation sidebar"), pane(pane), paneId(paneId),
      viewMenu(viewMenu), history(history), shelf(std::move(shelf)) {
  placesMenu = new PlacesMenu(&pane);
  for (int32 i = 0; i < kPlaceCommandCount; ++i) {
    auto *command = new BMessage(kPlaceAction);
    command->AddInt32("action", i);
    placesMenu->AddItem(new BMenuItem(kPlaceCommands[i], command));
  }
  placesMenu->AddSeparatorItem();
  goMenu.AddItem(placesMenu);
  SetExplicitMinSize(BSize(110, 96));
  SetExplicitPreferredSize(BSize(170, 300));
  tree = new FolderTree(&pane);
  favorites =
      new DestinationList(&pane, "favorite destinations", kFavoriteNavigate);
  favoriteScroll = new Listi::AdaptiveScrollView("favorite destinations scroll", favorites, true);
  favoriteScroll->SetExplicitMinSize(BSize(0, 32));
  auto navigation = new BGroupView(B_VERTICAL, 3);
  navigation->GroupLayout()->AddView(favoriteScroll, 0.35f);
  navigation->GroupLayout()->AddView(tree, 0.65f);
  favoriteScroll->Hide();
  AddChild(navigation);
  shelfList = new ShelfSidebarList(&pane);
  auto shelfScroll =
      new Listi::AdaptiveScrollView("sidebar shelf scroll", shelfList, true);
  AddChild(shelfScroll);
  recentFolders = new DestinationList(&pane, "recent folders", kRecentNavigate);
  auto recentScroll =
      new Listi::AdaptiveScrollView("recent folders scroll", recentFolders, true);
  AddChild(recentScroll);
  InstallSidebarInput(this, this);
  CardLayout()->SetVisibleItem(0);
  UpdatePlaces();
}

void PaneSidebar::AttachedToWindow() {
  BCardView::AttachedToWindow();
  placesMenu->SetTargetForItems(&pane);
}

void PaneSidebar::UpdateLocation() {
  UpdatePlaces();
  if (!pane.IsScanning()) {
    RefreshRecentFolders();
    ShowTreePath();
  }
}

bool PaneSidebar::ContainsFocus() const {
  auto focused = pane.Window() ? pane.Window()->CurrentFocus() : nullptr;
  while (focused && focused != this)
    focused = focused->Parent();
  return focused != nullptr;
}

void PaneSidebar::ToggleFocus() {
  if (ContainsFocus())
    pane.FocusList();
  else if (SidebarHasTree(visibleSidebar))
    tree->FindView("folder tree")->MakeFocus();
  else if (visibleSidebar == SidebarMode::Favorites)
    favorites->MakeFocus();
  else if (visibleSidebar == SidebarMode::Shelf)
    shelfList->MakeFocus();
  else if (visibleSidebar == SidebarMode::RecentFolders)
    recentFolders->MakeFocus();
}

void PaneSidebar::ShowTreePath() {
  if (SidebarHasTree(visibleSidebar))
    tree->ShowPath(pane.Path());
}

void PaneSidebar::ResetTree() { tree->Reset(); }

void PaneSidebar::AddSidebarModes(BMenu &choices, bool menuField) {
  for (auto option : SidebarModes) {
    if (option == SidebarMode::Shelf && !shelf)
      continue;
    auto message = new BMessage(kSidebarMode);
    message->AddInt32("mode", static_cast<int32>(option));
    auto item = new BMenuItem(option == SidebarMode::Hidden && !menuField
                                  ? "Hide sidebar"
                                  : SidebarModeLabel(option),
                              message);
    item->SetTarget(&pane);
    item->SetMarked(option == pane.Sidebar());
    choices.AddItem(item);
  }
}
void PaneSidebar::UpdateSidebarMenu(const WorkspaceLayout &layout) {
  if (!sidebarMenuItem ||
      sidebarMenuConfigurable != layout.HasConfigurableSidebar()) {
    if (sidebarMenuItem) {
      viewMenu.RemoveItem(sidebarMenuItem);
      delete sidebarMenuItem;
    }
    sidebarMenuConfigurable = layout.HasConfigurableSidebar();
    if (sidebarMenuConfigurable) {
      auto choices = new BMenu("Sidebar");
      AddSidebarModes(*choices, true);
      sidebarMenuItem = new BMenuItem(choices);
    } else {
      sidebarMenuItem = new BMenuItem("Folder tree", new BMessage(kToggleTree));
      sidebarMenuItem->SetTarget(&pane);
    }
    viewMenu.AddItem(sidebarMenuItem);
  }
  if (sidebarMenuConfigurable) {
    auto choices = sidebarMenuItem->Submenu();
    for (int32 i = 0; i < choices->CountItems(); ++i)
      choices->ItemAt(i)->SetMarked(
          choices->ItemAt(i)->Message()->GetInt32("mode", -1) ==
          static_cast<int32>(pane.Sidebar()));
  } else {
    sidebarMenuItem->SetMarked(visibleSidebar == SidebarMode::FolderTree);
    sidebarMenuItem->SetEnabled(!layout.SinglePane() &&
                                pane.CurrentView() != MillerMode);
  }
}
bool PaneSidebar::ContextMenu(BView *target, BPoint point) {
  if (!sidebarMenuConfigurable || visibleSidebar == SidebarMode::Hidden)
    return false;
  const BPoint screen = target->ConvertToScreen(point);
  BPopUpMenu choices("Sidebar actions", false, false);
  auto add = [&](const char *label, BMessage *message, bool enabled = true) {
    auto item = new BMenuItem(label, message);
    item->SetEnabled(enabled);
    choices.AddItem(item);
  };
  BListView *rows = target == favorites   ? favorites
                    : target == shelfList ? shelfList
                                          : nullptr;
  if (rows) {
    auto entry =
        dynamic_cast<SidebarItem *>(rows->ItemAt(rows->IndexOf(point)));
    if (entry) {
      auto remove =
          new BMessage(rows == favorites ? kRemoveShortcut : kRemoveShelfEntry);
      remove->AddString("path", entry->path.c_str());
      if (rows == shelfList)
        remove->AddInt64("revision", renderedShelf.revision);
      add(rows == favorites ? "Remove shortcut" : "Remove from Shelf", remove,
          rows != favorites || placesWritable);
    }
  }
  if (SidebarHasTree(visibleSidebar)) {
    const auto folder = tree->PathAt(screen);
    if (!folder.empty()) {
      auto shortcut = new BMessage(kAddShortcut);
      shortcut->AddString("path", folder.c_str());
      add("Add to shortcuts…", shortcut,
          placesWritable && !places.Find(folder) &&
              places.Items().size() < Listi::SavedPlaces::Limit);
    }
  }
  if (visibleSidebar == SidebarMode::Shelf) {
    add("Add selected files to Shelf", new BMessage(Listi::kAddToShelf),
        !pane.Selected().empty() && !pane.IsScanning());
    add("Open Shelf…", new BMessage(Listi::kShowShelf));
  }
  if (choices.CountItems() != 0)
    choices.AddSeparatorItem();
  AddSidebarModes(choices);
  if (auto picked = choices.Go(screen, false, true, false))
    if (picked->Message())
      pane.MessageReceived(picked->Message());
  return true;
}
void PaneSidebar::RefreshShelf() {
  if (!shelf || visibleSidebar != SidebarMode::Shelf)
    return;
  std::set<std::string> selected;
  if (auto item = dynamic_cast<SidebarItem *>(
          shelfList->ItemAt(shelfList->CurrentSelection())))
    selected.insert(item->path);
  auto snapshot = shelf->Snapshot();
  selected = snapshot.RetainSelection(renderedShelf, selected);
  renderedShelf = std::move(snapshot);
  ClearSidebarItems(*shelfList);
  for (const auto &entry : renderedShelf.entries) {
    const auto slash = entry.path.find_last_of('/');
    std::string label = entry.path.substr(slash + 1);
    switch (entry.DisplayState()) {
    case Listi::ShelfState::Missing:
      label += " (Unavailable)";
      break;
    case Listi::ShelfState::Replaced:
      label += " (Replaced)";
      break;
    case Listi::ShelfState::Ambiguous:
      label += " (Needs relink)";
      break;
    case Listi::ShelfState::Ready:
      break;
    }
    shelfList->AddItem(new SidebarItem(label.c_str(), entry.path));
    if (selected.count(entry.path))
      shelfList->Select(shelfList->CountItems() - 1);
  }
  if (shelfList->CountItems() == 0) {
    auto empty = new BStringItem("Shelf is empty");
    empty->SetEnabled(false);
    shelfList->AddItem(empty);
  }
  shelfList->Parent()->InvalidateLayout();
}
void PaneSidebar::UpdateMode(const WorkspaceLayout &layout) {
  SidebarMode wanted = pane.Sidebar();
  if (layout.IsEinn() ||
      (!layout.HasConfigurableSidebar() && pane.CurrentView() == MillerMode))
    wanted = SidebarMode::Hidden;
  if (wanted != visibleSidebar) {
    if (ContainsFocus())
      pane.FocusList();
    if (wanted == SidebarMode::Hidden) {
      Hide();
    } else {
      const bool showFavorites = wanted == SidebarMode::Favorites ||
                                 wanted == SidebarMode::ShortcutsAndTree;
      const bool showTree = SidebarHasTree(wanted);
      if (showFavorites == favoriteScroll->IsHidden(favoriteScroll))
        showFavorites ? favoriteScroll->Show() : favoriteScroll->Hide();
      if (showTree == tree->IsHidden(tree))
        showTree ? tree->Show() : tree->Hide();
      int32 card = 0;
      if (wanted == SidebarMode::Shelf)
        card = 1;
      else if (wanted == SidebarMode::RecentFolders)
        card = 2;
      CardLayout()->SetVisibleItem(card);
      if (visibleSidebar == SidebarMode::Hidden)
        Show();
    }
    visibleSidebar = wanted;
    ShowTreePath();
    RefreshShelf();
    RefreshRecentFolders();
  }
  UpdateSidebarMenu(layout);
}

void PaneSidebar::SetPlaces(const Listi::SavedPlaces &value, bool writable) {
  places = value;
  placesWritable = writable;
  ClearSidebarItems(*favorites);
  while (auto *item = placesMenu->RemoveItem(kFirstPlace))
    delete item;
  for (const auto &place : places.Items()) {
    favorites->AddItem(new SidebarItem(place.name.c_str(), place.path));
    auto *message = new BMessage(kNavigate);
    message->AddString("path", place.path.c_str());
    message->AddBool("place", true);
    placesMenu->AddItem(new BMenuItem(place.name.c_str(), message));
  }
  if (places.Items().empty()) {
    auto *empty = new BStringItem("No saved places");
    empty->SetEnabled(false);
    favorites->AddItem(empty);
  }
  favoriteScroll->InvalidateLayout();
  if (pane.Window())
    placesMenu->SetTargetForItems(&pane);
  UpdatePlaces();
}
void PaneSidebar::UpdatePlaces() {
  int32 current = -1;
  for (size_t i = 0; i < places.Items().size(); ++i) {
    const bool here = places.Items()[i].path == pane.Path();
    placesMenu->ItemAt(kFirstPlace + i)->SetMarked(here);
    if (here)
      current = i;
  }
  placesMenu->ItemAt(0)->SetEnabled(placesWritable && current < 0 &&
      pane.LocationValid() && places.Items().size() < Listi::SavedPlaces::Limit);
  placesMenu->ItemAt(1)->SetEnabled(placesWritable && current >= 0);
  placesMenu->ItemAt(2)->SetEnabled(placesWritable && current >= 0);
  if (current >= 0)
    favorites->Select(current);
  else
    favorites->DeselectAll();
}
void PaneSidebar::RefreshRecentFolders() {
  if (visibleSidebar != SidebarMode::RecentFolders)
    return;
  ClearSidebarItems(*recentFolders);
  for (const auto &folder : history.RecentFolders()) {
    auto label = Listi::LocationLeaf(folder);
    if (label.empty())
      label = folder;
    recentFolders->AddItem(new SidebarItem(label.c_str(), folder));
    if (pane.LocationValid() && folder == pane.Path())
      recentFolders->Select(recentFolders->CountItems() - 1);
  }
  if (recentFolders->CountItems() == 0) {
    auto empty = new BStringItem("No recent folders");
    empty->SetEnabled(false);
    recentFolders->AddItem(empty);
  }
  recentFolders->Parent()->InvalidateLayout();
  recentFolders->ScrollTo(0, 0);
}

void PaneSidebar::OpenDestination(const char *destination) {
  pane.Activate();
  if (std::string_view(pane.Path()) != destination || !pane.LocationValid())
    pane.Navigate(destination);
}

bool PaneSidebar::HandleCommand(const BMessage &message) {
  switch (message.what) {
  case kAddShortcut:
  case kRemoveShortcut: {
    const char *destination = message.GetString("path", "");
    const bool removing = message.what == kRemoveShortcut;
    if (placesWritable && destination[0] &&
        removing == (places.Find(destination) != nullptr)) {
      BMessage request(kPlaceAction);
      request.AddInt32("action", removing ? 2 : 0);
      request.AddString("path", destination);
      request.AddInt32("pane", paneId);
      pane.Window()->PostMessage(&request);
    }
    break;
  }
  case Listi::kAddToShelf:
    if (visibleSidebar == SidebarMode::Shelf) {
      pane.Activate();
      pane.Window()->PostMessage(Listi::kAddToShelf);
    }
    break;
  case Listi::kShowShelf:
    pane.Window()->PostMessage(Listi::kShowShelf);
    break;
  case kRemoveShelfEntry:
    if (shelf && visibleSidebar == SidebarMode::Shelf) {
      try {
        shelf->Remove({message.GetString("path", "")},
                      size_t(message.GetInt64("revision", -1)));
      } catch (const std::exception &error) {
        pane.ShowStatus(error.what());
      }
      pane.Window()->PostMessage(Listi::kRefreshShelf);
    }
    break;
  case kSidebarShelfDrop:
    if (shelf && visibleSidebar == SidebarMode::Shelf) {
      try {
        Listi::AddShelfDrop(*shelf, message);
        pane.Window()->PostMessage(Listi::kRefreshShelf);
      } catch (const std::exception &error) {
        pane.ShowStatus(error.what());
      }
    }
    break;
  case kFavoriteNavigate: {
    if (visibleSidebar != SidebarMode::Favorites &&
        visibleSidebar != SidebarMode::ShortcutsAndTree)
      break;
    const char *destination = message.GetString("path", "");
    if (!places.Find(destination))
      break;
    OpenDestination(destination);
    break;
  }
  case kRecentNavigate: {
    if (visibleSidebar != SidebarMode::RecentFolders)
      break;
    const char *destination = message.GetString("path", "");
    const auto &recent = history.RecentFolders();
    if (std::find(recent.begin(), recent.end(), destination) == recent.end())
      break;
    OpenDestination(destination);
    break;
  }
  case kPlaceAction: {
    if (!placesWritable)
      break;
    int32 action = message.GetInt32("action", 0);
    if ((action == 0 && (!pane.LocationValid() || places.Find(pane.Path()))) ||
        (action != 0 && !places.Find(pane.Path())))
      break;
    BMessage request(message);
    request.AddString("path", pane.Path());
    request.AddInt32("pane", paneId);
    pane.Window()->PostMessage(&request);
    pane.Activate();
    break;
  }
  default:
    return false;
  }
  return true;
}
