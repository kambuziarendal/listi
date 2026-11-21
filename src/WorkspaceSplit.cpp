// SPDX-License-Identifier: MIT
#include "WorkspaceSplit.h"
#include "ViewVisibility.h"

#include "ImagePreview.h"
#include "MainWindowMenu.h"
#include "Pane.h"
#include "FlatView.h"
#include "VirtualFile.h"
#include "ShelfWindow.h"
#include "PanelTools.h"
#include "WorkspaceLayout.h"
#include <Layout.h>
#include <Window.h>

namespace Listi {
namespace {
constexpr float kWorkspaceSpacing = 6;
constexpr int kInspector = 2;
}

WorkspaceSplit::WorkspaceSplit(Pane &first, Pane &second, Preferences &settings,
                               MainWindowMenu &bar)
    : BSplitView(B_HORIZONTAL, kWorkspaceSpacing), prefs(settings), menu(bar),
      source(prefs.SinglePane() ? prefs.singlePane : 0),
      panes{new PanelTools(0, first), new PanelTools(1, second)},
      inspector(new ImagePreview()), splitterSize(SplitterSize()) {
  AddChild(panes[0]);
  AddChild(panes[1]);
  AddChild(inspector);
  const WorkspaceLayout defaults;
  for (int i = 0; i < 3; ++i)
    SetItemWeight(i, defaults.splitWeights[i], i == kInspector);
  SetCollapsible(0, false);
  SetCollapsible(1, false);
}

int WorkspaceSplit::SplitIndex(int logicalIndex) const {
  if (inspectorOnLeft)
    return logicalIndex == kInspector ? 0 : logicalIndex + 1;
  return logicalIndex;
}

void WorkspaceSplit::PlaceInspector(bool first, float weight) {
  if (inspectorOnLeft == first)
    return;

  GetLayout()->RemoveView(inspector);
  inspectorOnLeft = first;
  AddChild(SplitIndex(kInspector), inspector, weight);
  for (int i = 0; i < 3; ++i)
    SetCollapsible(SplitIndex(i), i == kInspector);
}

void WorkspaceSplit::SetPaneVisible(int pane, bool visible) {
  if (paneVisible[pane] == visible)
    return;

  paneVisible[pane] = visible;
  if (visible)
    panes[pane]->Show();
  else
    panes[pane]->Hide();
}

void WorkspaceSplit::SetInspectorVisible(bool visible) {
  if (inspectorVisible == visible)
    return;

  inspectorVisible = visible;
  SetViewVisible(inspector, visible);
}

void WorkspaceSplit::ApplyGeometry(const WorkspaceLayout &layout, bool quickView,
                                   int source) {
  SetOrientation(layout.panelsStacked ? B_VERTICAL : B_HORIZONTAL);
  const bool oneView = layout.SinglePane() && !quickView;
  SetSpacing(oneView ? 0 : kWorkspaceSpacing);
  SetSplitterSize(oneView ? 0 : splitterSize);
  for (int i = 0; i < 3; ++i)
    SetItemWeight(SplitIndex(i), layout.splitWeights[i], true);
  if (quickView)
    SetItemWeight(SplitIndex(kInspector), layout.splitWeights[1 - source], true);
}

void WorkspaceSplit::RememberWeights(WorkspaceLayout &layout,
                                     bool quickView) const {
  if (layout.SinglePane() || quickView)
    return;

  // Keep the inspector's previous share when it is not participating.
  const bool inspectorExpanded =
      inspectorVisible && !inspector->IsHidden(inspector);
  const int count = inspectorExpanded ? 3 : 2;
  const float totalShare =
      inspectorExpanded ? 1 : 1 - layout.splitWeights[kInspector];
  float raw[3]{}, total = 0;
  for (int i = 0; i < count; ++i) {
    raw[i] = ItemWeight(SplitIndex(i));
    total += raw[i];
  }
  if (total <= 0)
    return;
  for (int i = 0; i < count; ++i)
    layout.splitWeights[i] = raw[i] * totalShare / total;
}

void WorkspaceSplit::MouseDown(BPoint where) {
  for (int32 i = 0; i < CountItems(); ++i)
    before[i] = ItemWeight(i);
  BSplitView::MouseDown(where);
}

void WorkspaceSplit::MouseUp(BPoint where) {
  BSplitView::MouseUp(where);
  for (int32 i = 0; i < CountItems(); ++i) {
    if (before[i] != ItemWeight(i)) {
      Window()->PostMessage(kSplitChanged);
      break;
    }
  }
}

int WorkspaceSplit::PanelContaining(const BView *view) const {
  for (; view; view = view->Parent()) {
    if (view == panes[0])
      return 0;
    if (view == panes[1])
      return 1;
  }
  return -1;
}

bool WorkspaceSplit::HasPanelPreview() const {
  return Panel(0).Current() == PanelPage::Preview ||
         Panel(1).Current() == PanelPage::Preview;
}

bool WorkspaceSplit::CloseVisibleInspectorEditor() {
  return !InspectorVisible() || Inspector().CloseEditor();
}

void WorkspaceSplit::UpdatePreviews() {
  Panel(0).UpdatePreview(Panel(1));
  Panel(1).UpdatePreview(Panel(0));
  Inspector().ShowFile(InspectorVisible() ? Panel(source).SelectedFile() : "");
}

ImagePreview *WorkspaceSplit::OpenPanelPreview(int pane) {
  if (!CloseVisibleInspectorEditor())
    return nullptr;
  auto &preview = Panel(pane).OpenPreview();
  RefreshAndFocus();
  return &preview;
}

void WorkspaceSplit::SetSource(int pane) {
  source = pane;
  for (int i = 0; i < 2; ++i)
    Panel(i).Files().SetSource(i == source);
}

void WorkspaceSplit::FollowFocus(const BView *view) {
  if (!prefs.IsSmidja() && Panel(0).Current() != PanelPage::Flat &&
      Panel(1).Current() != PanelPage::Flat)
    return;
  const int pane = PanelContaining(view);
  if (pane >= 0 && paneVisible[pane] && source != pane) {
    SetSource(pane);
    UpdatePreviews();
  }
}

bool WorkspaceSplit::ActivateFiles(int pane) {
  if (pane < 0 || pane > 1 || !paneVisible[pane] ||
      Panel(pane).Current() != PanelPage::Files)
    return false;
  SetSource(pane);
  return true;
}

bool WorkspaceSplit::AllowsCommand(uint32 action) const {
  switch (action) {
  case kCopy: case kMove:
    return (Panel(source).Current() == PanelPage::Files ||
            Panel(source).Current() == PanelPage::Flat) &&
           Panel(1 - source).Current() == PanelPage::Files;
  case kSame: case kCreateLink: case kDuplicate:
  case kCompareFolders: case kCreateArchive: case kExtractArchive:
    return Panel(0).Current() == PanelPage::Files &&
           Panel(1).Current() == PanelPage::Files;
  case kClipboardPaste:
  case kClipboardSaveText: case kClipboardSaveImage: case kUndoBulkRename:
  case kEdit: case kEditExternally: case kView: case kPreviewOtherPanel:
  case kOpenWith: case kProperties:
  case kDelete: case kRestore: case kPermanentDelete: case kRename:
  case kBulkRename: case kMkdir: case kAddArchive: case kRemoveArchive:
  case kSelectType: case kSelectPattern:
  case Listi::kApplyPattern: case kNetwork:
    return Panel(source).Current() == PanelPage::Files;
  case kAll: case kNone: case kInvert:
  case kClipboardCopy: case kClipboardCut: case kCopyPaths: case kOpenSystem:
  case kGridMode:
  case Listi::kAddToShelf:
  case Listi::kFlatView:
    return Panel(source).Current() == PanelPage::Files ||
           Panel(source).Current() == PanelPage::Flat;
  default:
    return true;
  }
}

void WorkspaceSplit::Focus() { Panel(source).FocusCurrent(); }

void WorkspaceSplit::SwitchPane() {
  if (quickView)
    return;
  if (prefs.workspace == Workspace::askr && Panel(source).Current() == PanelPage::Files) {
    Panel(source).Files().ToggleTreeFocus();
  } else if (!prefs.SinglePane()) {
    SetSource(1 - source);
    Focus();
    UpdatePreviews();
  }
}

void WorkspaceSplit::RefreshPanels() {
  Inspector().SetQuickViewCloseVisible(quickView);
  SetInspectorVisible(quickView || (prefs.Preview() && !prefs.SinglePane() &&
                                   !HasPanelPreview()));
  UpdatePreviews();
  menu.SetPreview(prefs, quickView || HasPanelPreview());
}

void WorkspaceSplit::RefreshAndFocus() {
  RefreshPanels();
  Focus();
}

void WorkspaceSplit::ClosePanelTool(int pane) {
  auto &panel = Panel(pane);
  const int focus = panel.Current() == PanelPage::Preview ? 1 - pane : pane;
  if (panel.CloseCurrent()) {
    SetSource(focus);
    RefreshAndFocus();
  }
}

void WorkspaceSplit::ApplyWorkspace() {
  PlaceInspector(quickView ? source == 1 : prefs.inspectorLeft, prefs.splitWeights[2]);
  for (int i = 0; i < 2; ++i) {
    Panel(i).EnableTools(prefs.IsSmidja());
    SetPaneVisible(i, i == source || (!quickView && !prefs.SinglePane()));
    Panel(i).Files().SetWorkspace(prefs);
    Panel(i).Files().SetSource(i == source);
  }
  RefreshPanels();
  ApplyGeometry(prefs, quickView, source);
  menu.SetWorkspace(prefs, quickView);
}

bool WorkspaceSplit::PrepareLayoutChange() {
  if (!Inspector().CloseEditor() || !Panel(0).CloseEditor() ||
      !Panel(1).CloseEditor())
    return false;
  quickView = false;
  return true;
}

bool WorkspaceSplit::SetQuickView(bool enabled) {
  if (!enabled && !Inspector().CloseEditor())
    return false;
  quickView = enabled;
  return true;
}

void WorkspaceSplit::HandlePageCommand(const BMessage &message) {
  const int pane = message.GetInt32("pane", -1);
  if (pane < 0 || pane > 1 || !paneVisible[pane] ||
      (!prefs.IsSmidja() && Panel(pane).Current() != PanelPage::Flat))
    return;
  if (message.what == kClosePanelPage) {
    ClosePanelTool(pane);
    return;
  }
  const int32 page = message.GetInt32("page", -1);
  if (page >= int32(PanelPage::Files) && page <= int32(PanelPage::Flat) &&
      (prefs.IsSmidja() || page == int32(PanelPage::Files) || page == int32(PanelPage::Flat)) &&
      Panel(pane).Select(PanelPage(page))) {
    SetSource(pane);
    RefreshAndFocus();
  }
}

bool WorkspaceSplit::CanOpenFlat() const {
  const auto &panel = Panel(source);
  const char *path = panel.Files().Path();
  return panel.Current() == PanelPage::Flat ||
         (panel.Current() == PanelPage::Files && path[0] == '/' && !IsVirtual(path));
}

void WorkspaceSplit::OpenFlat(const std::shared_ptr<Shelf> &shelf) {
  if (!CanOpenFlat())
    return;
  if (Panel(source).Current() != PanelPage::Flat) {
    Panel(source).OpenFlat(shelf);
    RefreshAndFocus();
  }
}

void WorkspaceSplit::OpenSearch(const BMessage &store) {
  if (Panel(source).OpenSearch(store))
    RefreshAndFocus();
}

void WorkspaceSplit::FocusFilter() {
  if (Panel(source).Current() == PanelPage::Flat) {
    Focus();
    return;
  }
  if (Panel(source).Select(PanelPage::Files)) {
    RefreshPanels();
    Panel(source).Files().FocusFilter();
  }
}

void WorkspaceSplit::TogglePanelPreview() {
  if (Panel(source).Current() == PanelPage::Preview)
    ClosePanelTool(source);
  else if (Panel(1 - source).Current() == PanelPage::Preview)
    ClosePanelTool(1 - source);
  else
    OpenPanelPreview(1 - source);
}

bool WorkspaceSplit::DismissPanelTool() {
  switch (Panel(source).Current()) {
  case PanelPage::Flat:
  case PanelPage::Preview:
    ClosePanelTool(source);
    return true;
  case PanelPage::Search:
    Panel(source).Select(PanelPage::Files);
    RefreshAndFocus();
    return true;
  case PanelPage::Files:
    if (!HasPanelPreview())
      return false;
    ClosePanelTool(1 - source);
    return true;
  }
  return false;
}

bool WorkspaceSplit::ToggleInspector() {
  if (prefs.SinglePane() || quickView || HasPanelPreview() ||
      !CloseVisibleInspectorEditor())
    return false;
  prefs.SetPreview(!prefs.Preview());
  RefreshPanels();
  return true;
}
void WorkspaceSplit::RememberPaneDisplay(const BMessage &message) {
  for (int i = 0; i < 2; ++i) {
    prefs.SetSidebar(i, Panel(i).Files().Sidebar());
    prefs.SetPaneMode(i, Panel(i).Files().PaneOverride());
  }
  if (message.HasString("folder")) {
    std::string folder = message.GetString("folder", "");
    if (message.GetBool("forget", false))
      prefs.folderModes.erase(folder);
    else
      prefs.folderModes[folder] = message.GetInt32("mode", 0);
  }
  Panel(0).Files().FolderModes(prefs.folderModes);
  Panel(1).Files().FolderModes(prefs.folderModes);
}

bool WorkspaceSplit::ApplyWorkspaceEdit(const BMessage &message) {
  BMessage layoutMessage, profilesMessage;
  if (message.FindMessage("layout", &layoutMessage) != B_OK ||
      message.FindMessage("profiles", &profilesMessage) != B_OK)
    return false;
  if (!PrepareLayoutChange())
    return false;
  WorkspaceLayout layout;
  layout.Load(layoutMessage);
  prefs.profiles.Load(profilesMessage);
  if (layout.buttons != prefs.buttons)
    prefs.smidjaButtons = prefs.tviskiptButtons = -1;
  prefs.ApplyLayout(layout);
  if (prefs.SinglePane())
    source = prefs.singlePane;
  else
    prefs.singlePane = source;
  return true;
}

bool WorkspaceSplit::HandlePanelCommand(BMessage *m) {
  switch (m->what) {
  case Listi::kPanelPage:
  case Listi::kClosePanelPage:
    HandlePageCommand(*m);
    break;
  case kActivate:
    if (ActivateFiles(m->GetInt32("pane", source)))
      UpdatePreviews();
    break;
  case kSwitch:
    SwitchPane();
    break;
  case kSame:
    if (prefs.SinglePane())
      break;
    Panel(1 - source).Files().Navigate(Panel(source).Files().Path());
    break;
  case kRefresh:
    if (auto &active = Panel(source); active.Current() == PanelPage::Flat) {
      active.Page(PanelPage::Flat)->MessageReceived(m);
      break;
    }
    for (auto *panel : panes)
      panel->Files().Reload();
    break;
  case kFilter:
    FocusFilter();
    break;
  case kGridMode:
    if (m->GetInt32("mode", -1) == FlatMode)
      break; // Already in Flat View.
    if (Panel(source).Current() == PanelPage::Flat) {
      ClosePanelTool(source);
      if (m->GetInt32("mode", -1) == Panel(source).Files().CurrentView())
        break;
    }
    Panel(source).Files().MessageReceived(m);
    break;
  default:
    return false;
  }
  return true;
}
} // namespace Listi
