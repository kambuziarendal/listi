// SPDX-License-Identifier: MIT
#pragma once

#include <Messenger.h>
#include <SplitView.h>
#include <array>
#include <memory>
#include <optional>
#include <string>

class BGroupView;
class ImagePreview;
class MainWindowMenu;
class Pane;
struct Preferences;
struct WorkspaceLayout;

namespace Listi {
class PanelTools;
class Shelf;
constexpr uint32 kSplitChanged = 'swgt';
constexpr uint32 kDetachInspector = 'idtc', kDockInspector = 'idck';

class WorkspaceSplit : public BSplitView {
public:
  WorkspaceSplit(Pane &first, Pane &second, Preferences &prefs, MainWindowMenu &menu);
  ~WorkspaceSplit() override;
  int Source() const { return source; }
  void SetSource(int pane);
  void FollowFocus(const BView *view);
  bool ActivateFiles(int pane);
  void Focus();
  void ApplyWorkspace();
  bool AllowsCommand(uint32 action) const;
  void RememberPaneDisplay(const BMessage &message);
  bool ApplyWorkspaceEdit(const BMessage &message);
  bool PrepareLayoutChange();
  bool HandlePanelCommand(BMessage *message);
  bool CanOpenFlat() const;
  void OpenFlat(const std::shared_ptr<Shelf> &shelf);
  void OpenSearch(const BMessage &store);
  void TogglePanelPreview();
  bool DismissPanelTool();
  bool ToggleInspector();
  bool DetachInspector();
  bool DockInspector(const BMessage &);
  bool InspectorDetached() const { return floatingInspector.has_value(); }
  bool FloatingInspectorAllowsQuit();
  void OpenFloatingInspector(const std::string &editPath);
  bool QuickView() const { return quickView; }
  bool SetQuickView(bool enabled);
  void RefreshPanels();

  void PlaceInspector(bool first, float weight);
  void SetPaneVisible(int pane, bool visible);
  void SetInspectorVisible(bool visible);
  bool InspectorVisible() const { return inspectorVisible; }
  void ApplyGeometry(const WorkspaceLayout &layout, bool quickView, int source);
  void RememberWeights(WorkspaceLayout &layout, bool quickView) const;

  void MouseDown(BPoint where) override;
  void MouseUp(BPoint where) override;

  PanelTools &Panel(int pane) const { return *panes[pane]; }
  ImagePreview &Inspector() const { return *inspector; }
  bool HasPanelPreview() const;
  void UpdatePreviews();
  ImagePreview *OpenPanelPreview(int pane);

private:
  void SwitchPane();
  void FocusFilter();
  void HandlePageCommand(const BMessage &message);
  void RefreshAndFocus();
  void ClosePanelTool(int pane);
  int PanelContaining(const BView *view) const;
  bool CloseVisibleInspectorEditor();
  int SplitIndex(int logicalIndex) const;

  Preferences &prefs;
  MainWindowMenu &menu;
  int source;
  bool quickView = false;
  PanelTools *panes[2];
  BGroupView *inspectorSlot;
  ImagePreview *inspector;
  std::optional<BMessenger> floatingInspector;
  std::string floatingSelection;
  const float splitterSize;
  bool inspectorOnLeft = false;
  bool paneVisible[2] = {true, true};
  bool inspectorVisible = true;
  std::array<float, 3> before{};
};
}
