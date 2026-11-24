// SPDX-License-Identifier: MIT
#include <stdexcept>
#include "MainWindow.h"
#include "WindowOverview.h"
#include "FolderTabs.h"
#include <atomic>
#include <MenuItem.h>
#include "LayoutIntroduction.h"
#include "RecoveryReviewWindow.h"
#include "FlatView.h"
#include "MainWindowMenu.h"
#include "ApplicationServices.h"
#include "ArchivePasswords.h"
#include "Common.h"
#include "ClipboardFlow.h"
#include "FileDropFlow.h"
#include "FileOperationFlow.h"
#include "PaneContextMenu.h"
#include "ArchiveEditor.h"
#include "FolderCompare.h"
#include "PropertiesWindow.h"
#include "OpenWith.h"
#include "SavedNavigationController.h"
#include "SearchWindow.h"
#include "ShelfController.h"
#include "ShelfWindow.h"
#include "ImagePreview.h"
#include "InspectorEditor.h"
#include "InlineAction.h"
#include "OperationQueue.h"
#include "Pane.h"
#include "PaneSidebar.h"
#include "PanelTools.h"
#include "TerminalDock.h"
#include "VirtualFile.h"
#include "WindowKeyFilter.h"
#include "WindowScripting.h"
#include "WorkspaceEditor.h"
#include "WorkspaceSplit.h"
#include "WorkspaceToolbar.h"
#include <Application.h>
#include <array>
#include <exception>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <SplitView.h>
#include <Window.h>
#include <cstring>
#include <memory>
using Listi::PanelPage;

void PostMainWindowNotice(const char *text) {
  if (auto *window = dynamic_cast<BWindow *>(BLooper::LooperForThread(find_thread(nullptr)))) {
    if (window->FindView("first panel tools")) {
      BMessage local(kInlineNotice);
      local.AddString("error", text);
      window->PostMessage(&local);
      return;
    }
  }
  BMessage error(kServiceError);
  error.AddString("error", text);
  be_app->PostMessage(&error);
}

namespace {
std::atomic<int> browserCount{0};
class MainWindow : public Listi::WorkingWindow {
  BMessenger overview, guide;
  std::string titleLocation;
  std::array<std::string, 2> tabLocations;
  bool sessionLayout = false;
  BrowserOpener openBrowser;
  Preferences prefs;
  BMessenger service;
  BMessenger searchWindow;
  Listi::SavedNavigationController savedNavigation;
  std::array<Pane *, 2> panes{};
  Listi::ShelfController shelf{BMessenger(this), panes};
  MainWindowMenu *menu = new MainWindowMenu();
  Listi::WorkspaceToolbar toolbar{*this, *menu, prefs};
  Listi::WorkspaceSplit *split;
  OperationQueue queue{BMessenger(this)};
  InlineAction *actionArea = new InlineAction(BMessenger(this),
                                             [this] { toolbar.Refresh(true); });
  Listi::ClipboardFlow clipboard{*actionArea, queue};
  Listi::FileDropFlow fileDrop{*actionArea, queue, BMessenger(this), service};
  Listi::FileOperationFlow fileOperations{*actionArea, queue, clipboard, BMessenger(this)};
  std::unique_ptr<Listi::TerminalDock> terminalDock;

  void PublishPlaces() {
    for (auto pane : panes)
      pane->SetPlaces(savedNavigation.Places(), savedNavigation.CanEditPlaces());
  }
  void Save() {
    // Only the initial browser persists a default layout for future sessions.
    if (sessionLayout)
      return;
    auto saved = prefs.Save();
    saved.what = kSavePreferences;
    saved.AddMessenger("origin", BMessenger(this));
    be_app->PostMessage(&saved);
  }
  bool PromptBlocksCommand() {
    if (!actionArea->Pending())
      return false;
    queue.ErrorInfo("Finish or cancel the open question first.");
    return true;
  }
  void CloseAction() {
    actionArea->Close();
    clipboard.CancelPrompt();
    toolbar.Refresh(actionArea->Pending());
    split->Focus();
  }
  void AskInline(const char *text, const char *button, const BMessage &result,
                 const char *initial = nullptr) {
    if (!actionArea->Ask(text, button, result, initial))
      queue.ErrorInfo("Finish or cancel the open question first.");
  }
  void SetQuickView(bool enabled) {
    if (split->QuickView() != enabled) {
      if (!split->SetQuickView(enabled))
        return;
      ApplyWorkspace();
    }
    split->Focus();
  }
  void UpdateTransferTarget() {
    for (int side = 0; side < 2; ++side) {
      if (tabLocations[side] != panes[side]->Path()) {
        tabLocations[side] = panes[side]->Path();
        split->Panel(side).FolderBar().UpdateLabel();
      }
    }
    const int source = split->Source();
    const char *current = panes[source]->Path();
    if (titleLocation != current) {
      titleLocation = current;
      const auto last = titleLocation.find_last_not_of('/');
      std::string leaf = "/";
      if (titleLocation == "@disks")
        leaf = "Disks";
      else if (last != std::string::npos) {
        auto slash = titleLocation.rfind('/', last);
        size_t start = slash == std::string::npos ? 0 : slash + 1;
        leaf = titleLocation.substr(start, last - start + 1);
      }
      if (panes[source]->IsSearchList()) leaf = panes[source]->LocationLabel();
      Describe(leaf + " · Listi", titleLocation);
    }
    const auto &target = *panes[1 - source];
    const bool available = !prefs.SinglePane() && split->AllowsCommand(kCopy);
    const bool folder = !target.IsSearchList() && target.LocationValid() && strcmp(target.Path(), "@disks") != 0;
    toolbar.SetTransferTarget(source, available, folder ? target.Path() : nullptr);
  }
  void ApplyWorkspace() {
    for (int side = 0; side < 2; ++side)
      split->Panel(side).ShowFolderTabs(prefs.alwaysFolderTabs);
    toolbar.Arrange();
    split->ApplyWorkspace();
    queue.SetCompact(prefs.SinglePane());
    UpdateTransferTarget();
    // Fit the buttons after the rest of the workspace has its final geometry.
    toolbar.ApplyVisibility(actionArea->Pending());
  }
  void SetWorkspace(Workspace value) {
    if (prefs.workspace == value || !split->PrepareLayoutChange())
      return;
    prefs.workspace = value;
    prefs.singlePane = split->Source();
    ApplyWorkspace();
    if (!actionArea->Pending())
      split->Focus();
    Save();
  }
  void Apply() {
    for (auto p : panes) {
      p->Apply(prefs);
      p->Reload();
    }
    ApplyWorkspace();
  }
  ImagePreview *OpenSelectionPreview() {
    if (prefs.IsSmidja())
      return split->OpenPanelPreview(1 - split->Source());
    SetQuickView(true);
    return &split->Inspector();
  }

  bool HandleFolderTab(BMessage &message) {
    using namespace Listi;
    switch (message.what) {
    case kTabNew: case kTabDuplicate: case kTabClose: case kTabReopen:
    case kTabRename: case kTabNamed: case kTabLeft: case kTabRight:
    case kTabNext: case kTabPrevious: case kTabChoose: case kTabDetach: case kTabAlways:
      break;
    default:
      return false;
    }
    if (PromptBlocksCommand())
      return true;
    const int side = message.GetInt32("pane", split->Source());
    if (!split->ActivateFiles(side))
      return true;
    auto &bar = split->Panel(side).FolderBar();
    switch (message.what) {
    case kTabAlways: {
      prefs.alwaysFolderTabs = !prefs.alwaysFolderTabs;
      BMessage delta(kSharedPreferences);
      delta.AddBool("alwaysFolderTabs", prefs.alwaysFolderTabs);
      delta.AddMessenger("origin", BMessenger(this));
      be_app->PostMessage(&delta);
      for (int index = 0; index < 2; ++index)
        split->Panel(index).ShowFolderTabs(prefs.alwaysFolderTabs);
      Save();
      return true;
    }
    case kTabRename: {
      BMessage accepted(kTabNamed);
      accepted.AddInt32("pane", side);
      accepted.AddInt64("tab", bar.ActiveId());
      accepted.AddBool("placeLabel", true);
      const auto tab = bar.Snapshot();
      AskInline("Name this tab:", "Save name", accepted, tab.alias.c_str());
      return true;
    }
    case kTabNamed:
      if (message.GetInt64("tab", -1) == bar.ActiveId())
        bar.Rename(message.GetString("name", ""));
      return true;
    case kTabDetach: {
      if (openBrowser && openBrowser({{}, bar.Snapshot(), Frame().LeftTop() + BPoint(24, 24)}))
        bar.AfterDetach();
      break;
    }
    default:
      bar.Change(message.what, message.GetInt64("tab", -1));
    }
    split->RefreshPanels();
    split->Focus();
    return true;
  }

  bool HandleNavigation(BMessage *m) {
    const int source = split->Source();
    switch (m->what) {
    case kPlaceAction:
    case kPlaceAccepted:
      try {
        if (m->what == kPlaceAccepted) {
          savedNavigation.EditPlace(*m);
          PublishPlaces();
        } else if (auto prompt = savedNavigation.PreparePlaceEdit(*m)) {
          AskInline(prompt->question.c_str(), prompt->button, prompt->accepted,
                    prompt->initial ? prompt->initial->c_str() : nullptr);
        }
      } catch (const std::exception &e) {
        PostMainWindowNotice(e.what());
      }
      break;
    case kSearchEdit:
    case kSearchesSaved:
      savedNavigation.HandleSearchMessage(
          *m, searchWindow, {split->Panel(0).SearchMessenger(),
                            split->Panel(1).SearchMessenger()});
      break;
    case kPlacesSaved:
      if (savedNavigation.FinishPlaceSave(*m)) {
        if (m->HasString("error"))
          PostMainWindowNotice(m->GetString("error", "Save failed."));
        PublishPlaces();
      }
      break;
    case Listi::kFlatView:
      split->OpenFlat(shelf.Model());
      break;
    case kSearch:
      if (prefs.IsSmidja()) {
        split->OpenSearch(savedNavigation.SearchStore());
        break;
      }
      if (searchWindow.IsValid())
        searchWindow.SendMessage(kSearch);
      else
        searchWindow = OpenFileSearch(BMessenger(this), source,
                                      panes[source]->SearchRoot(), savedNavigation.SearchStore());
      break;
    case Listi::kOpenSearchList:
    case kRevealSearch: {
      int32 which = m->GetInt32("pane", -1);
      if (which < 0 || which > 1)
        break;
      if (split->QuickView() && source != which) {
        if (!split->SetQuickView(false))
          break;
        ApplyWorkspace();
      }
      if (!split->Panel(which).Select(PanelPage::Files))
        break;
      split->SetSource(which);
      split->RefreshPanels();
      if (prefs.SinglePane()) {
        prefs.singlePane = which;
        ApplyWorkspace();
        Save();
      }
      panes[which]->MessageReceived(m);
      Activate();
      split->Focus();
      break;
    }
    default:
      return false;
    }
    return true;
  }

  bool HandleWorkspace(BMessage *m) {
    switch (m->what) {
    case kQuickView:
      if (!actionArea->Pending()) {
        if (prefs.IsSmidja())
          split->TogglePanelPreview();
        else
          SetQuickView(!split->QuickView());
      }
      break;
    case kCloseQuickView:
      if (split->QuickView() && !actionArea->Pending())
        SetQuickView(false);
      break;
    case kPreviewToggle:
      if (split->ToggleInspector())
        Save();
      break;
    case kPreviewSelection:
      if (m->GetInt32("pane", -1) == split->Source() || split->HasPanelPreview())
        split->UpdatePreviews();
      if (m->GetInt32("pane", -1) == split->Source() && m->HasString("operationError"))
        queue.ErrorInfo(m->GetString("operationError", ""));
      break;
    case kPaneDisplay:
      split->RememberPaneDisplay(*m);
      Save();
      break;
    case kWorkspace: {
      int32 value =
          m->GetInt32("workspace", m->GetBool("simple", false) ? 1 : 0);
      if (value >= 0 && value <= 3)
        SetWorkspace(static_cast<Workspace>(value));
    } break;
    case Listi::kSplitChanged:
      split->RememberWeights(prefs, split->QuickView());
      Save();
      break;
    case kToggleBank:
      if (toolbar.Toggle(actionArea->Pending()))
        Save();
      break;
    case kToggleFullBank:
      if (!prefs.SinglePane() && !prefs.actionButtons.empty()) {
        prefs.fullActionButtons = !prefs.fullActionButtons;
        toolbar.Resize(Bounds().Width(), actionArea->Pending());
        Save();
      }
      break;
    case kOpenWorkspaceEditor:
      if (!PromptBlocksCommand())
        OpenWorkspaceEditor(BMessenger(this), prefs, m->GetBool("modules", false));
      break;
    case kApplyWorkspaceEditor: {
      if (PromptBlocksCommand())
        break;
      if (!split->ApplyWorkspaceEdit(*m))
        break;
      ApplyWorkspace();
      split->Focus();
      Save();
      break;
    }
    case kPreferences:
      OpenPreferences(BMessenger(this), prefs);
      break;
    case kSharedPreferences: {
      const Preferences before = prefs;
      prefs.ApplyShared(*m);
      BMessage changes = prefs.SharedChanges(&before);
      changes.RemoveName("separateWindows");
      changes.RemoveName("alwaysFolderTabs");
      if (!changes.IsEmpty())
        Apply();
      else if (before.alwaysFolderTabs != prefs.alwaysFolderTabs)
        for (int side = 0; side < 2; ++side)
          split->Panel(side).ShowFolderTabs(prefs.alwaysFolderTabs);
      break;
    }
    case kApplyPreferences: {
      BMessage delta;
      if (m->FindMessage("sharedChanges", &delta) == B_OK && !delta.IsEmpty()) {
        delta.what = kSharedPreferences;
        delta.AddMessenger("origin", BMessenger(this));
        be_app->PostMessage(&delta);
      }
      prefs.ApplyDialog(*m);
      Apply();
      Save();
      break;
    }
    default:
      return false;
    }
    return true;
  }

public:
  MainWindow(const Preferences &p, BMessenger svc,
             const Listi::SavedPlaces &saved, bool writable,
             const Listi::SavedSearches &queries, bool queriesWritable, BrowserRequest request, BrowserOpener open)
      : Listi::WorkingWindow(BRect(70, 70, 1170, 710), "Listi", "File manager",
                B_AUTO_UPDATE_SIZE_LIMITS),
        sessionLayout(request.localLayout || request.detached.has_value()), openBrowser(std::move(open)), prefs(p), service(svc),
        savedNavigation(svc, saved, writable, queries, queriesWritable, BMessenger(this)) {
    panes[0] = new Pane(0, prefs, shelf.Model());
    panes[1] = new Pane(1, prefs, shelf.Model());
    split = new Listi::WorkspaceSplit(*panes[0], *panes[1], prefs, *menu);
    auto *terminalSplit = new BSplitView(B_VERTICAL, 6);
    terminalDock = std::make_unique<Listi::TerminalDock>(
        terminalSplit, split, BMessenger(this));
    actionArea->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    actionArea->GroupLayout()->SetInsets(8, 6, 8, 6);
    actionArea->Hide();
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .Add(menu, 0)
        .Add(toolbar.Top(), 0)
        .Add(terminalSplit)
        .Add(actionArea, 0)
        .Add(queue.View(), 0)
        .Add(toolbar.Bottom(), 0);
    // Child visibility must not impose a transient maximum on the window.
    // Apply after attachment: Haiku replaces layout explicit-data proxies.
    GetLayout()->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    queue.Start();
    AddCommonFilter(new Listi::WindowKeyFilter());

    Apply();
    PublishPlaces();
    split->Focus();
    ResizeTo(1140, 650);
    ++browserCount;
    if (request.position)
      MoveTo(*request.position);
    if (request.detached)
      split->Panel(0).FolderBar().Adopt(std::move(*request.detached));
    else if (!request.path.empty())
      panes[split->Source()]->Navigate(request.path.c_str());
  }
  ~MainWindow() override {
    --browserCount;
    BMessage closed(kBrowserClosed);
    closed.AddInt64("id", WindowId());
    be_app->PostMessage(&closed);
  }
  void FrameResized(float width, float height) override {
    BWindow::FrameResized(width, height);
    toolbar.Resize(width, actionArea->Pending());
  }

  status_t GetSupportedSuites(BMessage *m) override {
    Listi::AddWindowScriptingSuite(*m);
    return BWindow::GetSupportedSuites(m);
  }
  BHandler *ResolveSpecifier(BMessage *msg, int32 index, BMessage *spec,
                             int32 what, const char *property) override {
    if (Listi::IsWindowScriptProperty(property))
      return this;
    return BWindow::ResolveSpecifier(msg, index, spec, what, property);
  }
  void DispatchMessage(BMessage *message, BHandler *target) override {
    if (message->what == B_MOUSE_DOWN || message->what == B_KEY_DOWN) {
      auto *receiver = message->what == B_MOUSE_DOWN
                           ? dynamic_cast<BView *>(target) : CurrentFocus();
      split->FollowFocus(receiver);
    }
    const bool quitting = message->what == B_QUIT_REQUESTED;
    BWindow::DispatchMessage(message, target);
    if (!quitting)
      UpdateTransferTarget();
  }
  void MenusBeginning() override {
    BWindow::MenusBeginning();
    const auto &pane = *panes[split->Source()];
    const char *path = pane.Path();
    const auto page = split->Panel(split->Source()).Current();
    const bool flat = page == PanelPage::Flat;
    const auto &bar = split->Panel(split->Source()).FolderBar();
    const bool filePage = page == PanelPage::Files;
    for (uint32 cmd : {Listi::kTabNew, Listi::kTabDuplicate, Listi::kTabRename,
                       Listi::kTabDetach, Listi::kTabAlways})
      menu->FindItem(cmd)->SetEnabled(filePage);
    for (uint32 cmd : {Listi::kTabPrevious, Listi::kTabNext, Listi::kTabClose})
      menu->FindItem(cmd)->SetEnabled(filePage && bar.Count() > 1);
    menu->FindItem(Listi::kTabLeft)->SetEnabled(filePage && bar.Index() > 0);
    menu->FindItem(Listi::kTabRight)->SetEnabled(filePage && bar.Index() + 1 < bar.Count());
    menu->FindItem(Listi::kTabReopen)->SetEnabled(filePage && bar.CanReopen());
    menu->FindItem(Listi::kTabAlways)->SetMarked(prefs.alwaysFolderTabs);
    menu->FindItem(kSelectedBrowser)->SetEnabled(filePage && !pane.SelectedFolder().empty());
    menu->SetFolderTree(page == PanelPage::Files && pane.CanToggleTree(),
                        SidebarHasTree(pane.Sidebar()));
    const bool canSaveClipboard =
        !pane.IsSearchList() && !actionArea->Pending() && Listi::CanSaveClipboardIn(path);
    const bool canUndoBulkRename =
        !actionArea->Pending() && queue.CanUndoBulkRename();
    if (auto *paste = menu->FindItem(kClipboardPaste))
      paste->SetEnabled(!pane.IsSearchList());
    menu->SetFileActions(pane.CurrentView(), flat, split->CanOpenFlat(),
                         canSaveClipboard, canUndoBulkRename);
  }

  bool QuitRequested() override {
    if (PreferencesOpenFor(BMessenger(this))) {
      queue.ErrorInfo("Apply or close this window's preferences first.");
      return false;
    }
    if (browserCount > 1 && !InspectorEditorsAllowQuit(this))
      return false;
    if (browserCount == 1 && (!InspectorEditorsAllowQuit() || !FilePropertiesAllowQuit() ||
        !OpenWithAllowQuit() || !ArchiveEditorsAllowQuit()))
      return false;
    if (terminalDock->HasSessions()) {
      queue.ErrorInfo("Close this window's CLI sessions first.");
      return false;
    }
    if (savedNavigation.Saving()) {
      queue.ErrorInfo(
          "Saved places or searches are still being written. Please try quitting again shortly.");
      return false;
    }
    if (queue.Busy() || clipboard.Busy()) {
      queue.Show();
      queue.ErrorInfo("File operations are still running. Let them finish, or use "
             "Cancel all before quitting.");
      return false;
    }
    if (clipboard.ArchivePromptPending())
      CloseAction();
    return true;
  }
  void MessageReceived(BMessage *m) override {
    if (m->what == kQuitListi) {
      be_app->PostMessage(B_QUIT_REQUESTED);
      return;
    }
    if (m->what == kNewBrowser || m->what == kSelectedBrowser) {
      auto &pane = *panes[split->Source()];
      std::string path = m->what == kNewBrowser ? pane.Path() : pane.SelectedFolder();
      if (!path.empty() && openBrowser)
        openBrowser({path, {}, Frame().LeftTop() + BPoint(24, 24)});
      return;
    }
    if (HandleFolderTab(*m) || HandleWindowAction(m))
      return;
    if (m->what == Listi::kOpenWindowOverview) {
      overview = Listi::OpenWindowOverview(*this, overview);
      return;
    }
    if (m->what == Listi::kShowLayoutGuide) {
      guide = Listi::OpenLayoutIntroduction(*this, guide, prefs.workspace,
                                           m->GetBool("firstRun", false));
      return;
    }
    if (m->what == Listi::kGuideChoice) {
      const int32 value = m->GetInt32("workspace", -1);
      if (value >= 0 && value <= 3 && prefs.workspace != Workspace(value))
        SetWorkspace(Workspace(value));
      else if (m->GetBool("firstRun", false))
        Save();
      return;
    }
    if (m->what == Listi::kFlatContext) {
      const int pane = m->GetInt32("pane", -1);
      if (pane >= 0 && pane < 2 && split->Panel(pane).Current() == PanelPage::Flat &&
          !PromptBlocksCommand()) {
        auto *view = split->Panel(pane).Page(PanelPage::Flat);
        split->FollowFocus(view);
        if (split->Source() == pane)
          view->MessageReceived(m);
      }
      return;
    }
    const int source = split->Source();
    if (Listi::HandleWindowScript(*m, prefs.workspace, *panes[source],
                                  *panes[1 - source], queue))
      return;
    if (!split->AllowsCommand(m->what)) {
      queue.ErrorInfo("Switch to Files in the affected panel before using this command.");
      return;
    }
    // The last completed scan can explicitly invalidate the location, e.g.
    // after USB removal. Do not synchronously probe that stale mount on retry.
    const bool panelTransfer = m->what == kCopy || m->what == kMove;
    const bool writesHere = m->what == kClipboardPaste || m->what == kClipboardSaveText ||
                           m->what == kClipboardSaveImage || m->what == kMkdir;
    const Pane &destination = *panes[panelTransfer ? 1 - source : source];
    const bool needsOtherFolder = panelTransfer || m->what == kCreateLink ||
        m->what == kCreateArchive || m->what == kExtractArchive || m->what == kCompareFolders;
    const bool needsCurrentFolder = writesHere || m->what == kBulkRename ||
        m->what == kRename || m->what == kCompareFolders ||
        m->what == kCommand || m->what == Listi::kTerminalWindow;
    if ((needsOtherFolder && panes[prefs.SinglePane() ? source : 1 - source]->IsSearchList()) ||
        (needsCurrentFolder && panes[source]->IsSearchList())) {
      queue.ErrorInfo("Choose a real folder for this action, not a search list.");
      return;
    }
    if ((panelTransfer || writesHere) && !destination.LocationValid() &&
        !destination.IsScanning()) {
      queue.ErrorInfo("Choose an available destination folder, or reconnect the disk and reload.");
      return;
    }
    auto &panel = split->Panel(source);
    if (panel.Current() == PanelPage::Flat) {
      auto *flat = panel.Page(PanelPage::Flat);
      switch (m->what) {
      case kCopy:
      case kMove:
        if (!PromptBlocksCommand()) {
          try {
            if (prefs.SinglePane())
              throw std::runtime_error("Use Copy or Cut, then navigate and Paste.");
            auto job = Listi::FlatSelection(*flat);
            job.action = m->what == kCopy ? Listi::Action::Copy : Listi::Action::Move;
            job.destination = panes[1 - source]->Path();
            fileOperations.SubmitTransfer(std::move(job));
          } catch (const std::exception &e) {
            PostMainWindowNotice(e.what());
          }
        }
        return;
      case kOpenSystem: case kClipboardCopy: case kClipboardCut: case kCopyPaths:
      case kAll: case kNone: case kInvert: case Listi::kAddToShelf:
        if (!PromptBlocksCommand())
          flat->MessageReceived(m);
        return;
      }
    }
    auto &pane = *panes[source];
    if (fileOperations.Handle(*m, pane, *panes[1 - source], prefs.SinglePane()) ||
        fileDrop.Handle(*m) || terminalDock->ApplyPlacement(*m) ||
        shelf.Handle(*m, pane, queue) || split->HandlePanelCommand(m) ||
        HandleNavigation(m) || HandleWorkspace(m) ||
        pane.HandleSelectionCommand(*m, *actionArea, queue) ||
        pane.HandleOpenCommand(m->what, prefs.editor, service,
                               [this] { return OpenSelectionPreview(); }))
      return;
    switch (m->what) {
    case kInlineNotice:
      if (actionArea->Pending()) {
        queue.ErrorInfo(m->GetString("error", "Operation failed"));
        break;
      }
      AskInline(m->GetString("error", "Operation failed"), "OK",
                BMessage(kInlineCancel));
      break;
    case kInlineClosed:
      if (!actionArea->Pending()) {
        if (clipboard.ArchivePromptPending())
          CloseAction();
        else
          clipboard.CancelPrompt();
      }
      toolbar.Refresh(actionArea->Pending());
      if (!actionArea->Pending() && !terminalDock->HasFocus())
        split->Focus();
      break;
    case kInlineCancel:
      if (pane.CancelSelectionGesture())
        break;
      if (actionArea->Pending())
        CloseAction();
      else if (!split->DismissPanelTool()) {
        if (split->QuickView())
          SetQuickView(false);
        else
          queue.Collapse();
      }
      break;
    case kContextMenu: {
      if (PromptBlocksCommand())
        break;
      const int32 owner = m->GetInt32("pane", -1);
      if (!split->ActivateFiles(owner))
        break;
      try {
        auto result = Listi::RunPaneContextMenu(
            *m, *panes[owner], *panes[1 - owner], owner,
            prefs.SinglePane(),
            !prefs.SinglePane() &&
                split->Panel(1 - owner).Current() == PanelPage::Files,
            prefs.folderModes.count(panes[owner]->Path()),
            savedNavigation.Places(), savedNavigation.CanEditPlaces());
        // Keep dispatch synchronous so a queued activation cannot redirect it.
        if (result.command.what == Listi::kAddToShelf)
          shelf.Add(*panes[owner], result.shelfSources);
        else if (result.command.what)
          MessageReceived(&result.command);
      } catch (const std::exception &e) {
        PostMainWindowNotice(e.what());
      }
      break;
    }
    case 'apfg':
      Listi::ForgetArchivePasswords();
      break;
    case kResumeArchiveEdits:
      ResumeArchiveEditors(prefs.editor.String());
      break;
    case kClipboardSaveText:
    case kClipboardSaveImage:
    case kClipboardCopy:
    case kClipboardCut:
    case kClipboardPaste:
    case kCopyPaths:
      clipboard.Begin(m->what, pane);
      break;
    case Listi::kClipboardFileAccepted: {
      const char *name = m->GetString("name", "");
      if (auto folder = clipboard.SaveFile(name)) {
        const auto status = std::string("Saved ") + name;
        for (auto *pane : panes) {
          if (pane->Path() == *folder) {
            pane->Reload();
            pane->ShowStatus(status.c_str());
          }
        }
      }
      break;
    }
    case kFileProgress:
      for (auto p : panes)
        p->MessageReceived(m);
      break;
    case kQueueWake:
      for (auto &done : queue.TakeEvents()) {
        queue.PresentEvent(done);
        if (done.what != kJobDone)
          continue;
        clipboard.JobFinished(done);
        for (auto p : panes)
          p->MessageReceived(&done);
        shelf.Refresh();
      }
      break;
    case kShowJobs:
      queue.Toggle();
      break;
    case kReviewRecovery:
      if (queue.Busy())
        queue.ErrorInfo("Wait for the active operation before reviewing recovery records.");
      else
        Listi::OpenRecoveryReview(Frame());
      break;
    case kCompareFolders:
      OpenFolderCompare(pane.Path(), panes[1 - source]->Path());
      break;
    case kOpenPaths:
      service.SendMessage(m);
      break;
    case kTerminal:
    case Listi::kLeftCLI:
    case Listi::kRightCLI:
    case Listi::kShowFloatingTerminal: {
      const int lister = m->what == Listi::kLeftCLI    ? 0
                         : m->what == Listi::kRightCLI ? 1
                         : m->GetInt32("terminal lister", source);
      if (lister < 0 || lister > 1)
        break;
      if (panes[lister]->IsSearchList()) {
        PostMainWindowNotice("Open a local folder before starting a terminal.");
        break;
      }
      if (auto prompt = terminalDock->Open(*m, lister, panes[lister]->Path()))
        AskInline(prompt->text.String(), "Start new CLI", prompt->accepted);
      break;
    }
    case Listi::kCloseTerminal:
      if (terminalDock->Close(m->GetInt64("terminal placement", -1))) {
        Activate();
        if (!terminalDock->HasFocus())
          split->Focus();
      }
      break;
    case Listi::kTerminalWindow:
    case kCommand: {
      if (!strcmp(pane.Path(), "@disks") ||
          Listi::IsVirtual(pane.Path())) {
        PostMainWindowNotice("Open a local folder first.");
        break;
      }
      BMessage launch(kOpenPaths);
      launch.AddString("path", pane.Path());
      launch.AddString("mode", m->what == Listi::kTerminalWindow ? "terminal"
                                                               : "command");
      launch.AddString("command", toolbar.CommandText());
      launch.AddString("application", prefs.terminal);
      service.SendMessage(&launch);
      break;
    }
    case kNetwork:
      if (!actionArea->Connect(BMessenger(&pane)))
        queue.ErrorInfo("Finish or cancel the open question first.");
      break;
    default:
      BWindow::MessageReceived(m);
      break;
    }
  }
};
} // namespace

BWindow *CreateMainWindow(const Preferences &prefs, BMessenger service,
                          const Listi::SavedPlaces &places, bool placesWritable,
                          const Listi::SavedSearches &searches, bool searchesWritable, BrowserRequest request, BrowserOpener open) {
  auto layout = prefs;
  if (request.detached) {
    layout.workspace = Workspace::einn;
    layout.singlePane = 0;
  }
  return new MainWindow(layout, service, places, placesWritable,
                        searches, searchesWritable, std::move(request), std::move(open));
}

bool BrowserAllowsQuit(BWindow *window) {
  auto *browser = dynamic_cast<MainWindow *>(window);
  return !browser || browser->QuitRequested();
}
