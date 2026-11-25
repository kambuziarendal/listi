// SPDX-License-Identifier: MIT
#include <stdexcept>
#include "UnavailableVolume.h"
#include "../build/ListiBuildIdentity.h"
#include "FileClipboard.h"
#include <Alert.h>
#include <Invoker.h>
#include <sys/utsname.h>
#include "MainWindow.h"
#include "WindowOverview.h"
#include "FolderTabs.h"
#include <Screen.h>
#include <NodeMonitor.h>
#include "WorkingSets.h"
#include <atomic>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include "LayoutIntroduction.h"
#include "FolderHistory.h"
#include "AutomaticHistory.h"
#include "HistorySettings.h"
#include "HistoryPaneHint.h"
#include "FileListExportWindow.h"
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
#include "FilePanelFavorite.h"
#include <FindDirectory.h>
#include <Path.h>
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
#include <utility>
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

void ShowQuitBlocker(const char *text) {
  auto *alert = new BAlert("Cannot close Listi", text, "OK", nullptr, nullptr,
                           B_WIDTH_AS_USUAL, B_WARNING_ALERT);
  alert->SetShortcut(0, B_ESCAPE);
  alert->Go(nullptr);
}

namespace {
class CLIQuitAlert : public BAlert {
public:
  CLIQuitAlert(BMessenger recipient, uint32 command, bool wholeApp)
      : BAlert("Close CLI sessions?",
               wholeApp
                   ? "Listi has open CLI sessions. Closing them stops their shells and may interrupt running commands."
                   : "This window has open CLI sessions. Closing them stops their shells and may interrupt running commands.",
               "Cancel", wholeApp ? "Close CLI and quit" : "Close CLI and window",
               nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT),
        result(new BMessage(command), recipient) {
    SetShortcut(0, B_ESCAPE);
    SetDefaultButton(ButtonAt(0));
  }

  void ShowPrompt() { Go(&result); }

private:
  BInvoker result; // BAlert borrows the invoker until the dialog closes.
};
} // namespace

BMessenger ConfirmClosingCLIs(BMessenger recipient, uint32 command, bool wholeApp) {
  auto *alert = new CLIQuitAlert(recipient, command, wholeApp);
  BMessenger target(alert);
  alert->ShowPrompt();
  return target;
}

bool CheckSharedQuitBlockers() {
  if (Listi::FolderHistoryBusy())
    ShowQuitBlocker("A folder history operation is running. Let it finish or cancel it, then try again.");
  else if (FolderCompareBusy())
    ShowQuitBlocker("A folder comparison is running. Let it finish or cancel it, then try again.");
  else if (!InspectorEditorsAllowQuit())
    ShowQuitBlocker("An editor is still open with unsaved changes or is busy. Check the editor before closing Listi.");
  else if (!FilePropertiesAllowQuit())
    ShowQuitBlocker("File Properties has unsaved changes or is busy. Check that window before closing Listi.");
  else if (!OpenWithAllowQuit())
    ShowQuitBlocker("Open With is still working. Let it finish before closing Listi.");
  else if (!ArchiveEditorsAllowQuit())
    ShowQuitBlocker("An archive editor is still working. Let it finish before closing Listi.");
  else
    return true;
  return false;
}

namespace {
constexpr uint32 kConfirmWindowCLIQuit = 'wccl';
std::atomic<int> browserCount{0};
class MainWindow : public Listi::WorkingWindow {
  BMessenger overview, guide;
  std::vector<BMessage> historyPairs;
  bool readingConfig = false, refreshConfig = false, automaticEnabled = false;
  std::string titleLocation;
  std::string detachedWorkNotice;
  std::array<std::string, 2> tabLocations, probedFolders;
  bool sessionLayout = false;
  std::optional<bool> wasEinn;
  BRect wideFrame, einnFrame;
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
  BMessenger cliQuitAlert;
  bool closeCLIOnQuit = false;
  bool operationsWindowSuspended = false;

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
    actionArea->FocusQuestion();
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
  void RefreshTerminalTabLabel(int side) {
    const auto id = terminalDock->OwnerTab(side);
    if (id > 0)
      terminalDock->UpdateTabLabel(side, split->Panel(side).FolderBar().DescribeTab(id));
  }
  void UpdateTransferTarget() {
    // Wait for the initial folder read so its progress does not erase the notice.
    if (!detachedWorkNotice.empty() && !panes[0]->IsScanning()) {
      panes[0]->ShowStatus(detachedWorkNotice.c_str());
      detachedWorkNotice.clear();
    }
    for (int side = 0; side < 2; ++side) {
      const auto *pane = panes[side];
      bool known = false;
      int32 versions = Listi::kUnknownVersions;
      if (pane->LocationValid() && !pane->IsSearchList()) {
        const auto [device, node] = pane->FolderIdentity();
        for (const auto &pair : historyPairs)
          if (strcmp(pane->Path(), pair.GetString("path", 0, "")) == 0 &&
              device == pair.GetInt64("node", 0, -1) && node == pair.GetInt64("node", 1, -1)) {
            known = true;
            versions = pair.GetInt32("versions", Listi::kUnknownVersions);
            break;
          }
      }
      panes[side]->SetHistoryHint(known, automaticEnabled, versions);
      // A store may return or receive new points while its source is not displayed.
      if (!known && probedFolders[side] != pane->Path())
        probedFolders[side].clear();
      if (known && probedFolders[side] != pane->Path() && !readingConfig)
        ReadHistoryConfig();
      if (tabLocations[side] != panes[side]->Path()) {
        tabLocations[side] = panes[side]->Path();
        split->Panel(side).FolderBar().UpdateLabel();
        RefreshTerminalTabLabel(side);
      }
    }
    const int source = split->Source();
    const char *current = panes[source]->Path();
    const bool copyTarget = split->Panel(source).Current() == PanelPage::Files &&
        panes[source]->LocationValid() && !panes[source]->IsSearchList() && current[0] == '/';
    const auto [device, node] = panes[source]->FolderIdentity();
    SetCopyFolder(copyTarget ? current : "", copyTarget
        ? Listi::NodeIdentity{int32_t(device), node} : Listi::NodeIdentity{});
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
    const bool einn = prefs.workspace == Workspace::einn;
    const bool switchSize = !wasEinn || *wasEinn != einn;
    if (wasEinn && switchSize)
      (*wasEinn ? einnFrame : wideFrame) = Frame();
    for (int side = 0; side < 2; ++side)
      split->Panel(side).ShowFolderTabs(prefs.alwaysFolderTabs);
    toolbar.Arrange();
    split->ApplyWorkspace();
    queue.SetCompact(prefs.SinglePane());
    queue.SetSeparateWindow(prefs.operationsInWindow && !operationsWindowSuspended);
    if (switchSize) {
      const auto saved = einn ? einnFrame : wideFrame;
      if (saved.IsValid()) {
        ResizeTo(saved.Width(), saved.Height());
        MoveTo(saved.LeftTop());
      } else {
        const auto screen = BScreen(this).Frame();
        ResizeTo(std::min(einn ? 540.f : 1140.f, screen.Width() - 32),
                 std::min(einn ? 320.f : 650.f, screen.Height() - 56));
      }
      MoveOnScreen();
      wasEinn = einn;
    }
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
  void OpenSelectionPreview(const std::string &editPath) {
    if (split->InspectorDetached()) {
      split->OpenFloatingInspector(editPath);
      return;
    }
    ImagePreview *target;
    if (prefs.IsSmidja())
      target = split->OpenPanelPreview(1 - split->Source());
    else {
      SetQuickView(true);
      target = &split->Inspector();
    }
    if (target && !editPath.empty())
      target->BeginEditing(editPath);
  }

  bool HandleFolderTab(BMessage &message) {
    using namespace Listi;
    switch (message.what) {
    case kTabNew: case kTabDuplicate: case kTabClose: case kTabReopen:
    case kTabRename: case kTabNamed: case kTabLeft: case kTabRight:
    case kTabNext: case kTabPrevious: case kTabChoose: case kTabDetach: case kTabAlways:
    case kTabContext: case kTabOpposite:
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
    const int64 id = message.GetInt64("tab", bar.ActiveId());
    switch (message.what) {
    case kTabContext: {
      BPoint where;
      if (bar.DescribeTab(id).empty() || message.FindPoint("where", &where) != B_OK)
        return true;
      BPopUpMenu context("Folder tab", false, false);
      auto *move = new BMenuItem(side == 0 ? "Move tab to right pane" : "Move tab to left pane",
                                 new BMessage(kTabOpposite));
      context.AddItem(move);
      move->SetEnabled(!panes[1 - side]->IsHidden() &&
                       split->Panel(1 - side).Current() == PanelPage::Files);
      context.AddItem(new BMenuItem("Detach tab to Einn window", new BMessage(kTabDetach)));
      context.AddSeparatorItem();
      auto *close = new BMenuItem("Close tab", new BMessage(kTabClose));
      context.AddItem(close);
      close->SetEnabled(bar.Count() > 1);
      if (auto *selected = context.Go(where, false, true)) {
        BMessage action(selected->Message()->what);
        action.AddInt32("pane", side);
        action.AddInt64("tab", id);
        PostMessage(&action);
      }
      return true;
    }
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
      break;
    case kTabOpposite: {
      auto tab = bar.CopyTab(id);
      const int other = 1 - side;
      if (!tab || !split->ActivateFiles(other))
        break;
      const auto placement = terminalDock->OpenPlacement(side, id);
      if (placement && !terminalDock->TransferTo(placement, *terminalDock, other)) {
        split->SetSource(side);
        queue.ErrorInfo("Cannot move the CLI to the opposite pane. The tab and CLI have not moved.");
        break;
      }
      split->Panel(other).FolderBar().InsertTransferred(std::move(*tab));
      bar.ReleaseTab(id);
      break;
    }
    case kTabDetach: {
      auto tab = bar.CopyTab(id);
      if (!tab || !openBrowser)
        break;
      BrowserRequest request{{}, std::move(tab), Frame().LeftTop() + BPoint(24, 24)};
      request.cliPlacement = terminalDock->OpenPlacement(side, id);
      if (request.cliPlacement)
        request.cliSource = BMessenger(this);
      if (queue.Busy() || clipboard.Busy())
        request.detachedWorkNotice = "File operations continue in the original window.";
      if (openBrowser(std::move(request)))
        bar.ReleaseTab(id);
      break;
    }
    default:
      bar.Change(message.what, message.GetInt64("tab", -1));
    }
    for (int index = 0; index < 2; ++index)
      RefreshTerminalTabLabel(index);
    split->RefreshPanels();
    split->Focus();
    return true;
  }

  void MoveTabToWindow(const BMessage &message) {
    if (PromptBlocksCommand())
      return;
    const int side = message.GetInt32("pane", -1);
    if (side != 0 && side != 1)
      return;
    auto &bar = split->Panel(side).FolderBar();
    auto tab = bar.CopyTab(message.GetInt64("tab", -1));
    if (!tab)
      return;
    BMessenger address;
    if (message.FindMessenger("destination", &address) != B_OK ||
        !address.IsTargetLocal() || address == BMessenger(this) ||
        address.LockTargetWithTimeout(250000) != B_OK) {
      queue.ErrorInfo("The destination window is busy or closed. The tab has not moved.");
      return;
    }
    BLooper *looper = nullptr;
    address.Target(&looper);
    struct UnlockWindow {
      BLooper *looper;
      ~UnlockWindow() { looper->Unlock(); }
    } unlock{looper};
    auto *target = dynamic_cast<MainWindow *>(looper);
    if (!target || target->actionArea->Pending()) {
      queue.ErrorInfo("The destination window cannot accept a tab right now.");
      return;
    }
    const int dest = target->split->Source();
    if (target->split->Panel(dest).Current() != PanelPage::Files) {
      queue.ErrorInfo("Select a file panel in the destination window first.");
      return;
    }
    const auto placement = terminalDock->OpenPlacement(side, tab->id);
    if (placement && !terminalDock->TransferTo(placement, *target->terminalDock, dest)) {
      queue.ErrorInfo("Cannot move the CLI to that panel. The tab and CLI have not moved.");
      return;
    }
    const auto id = tab->id;
    target->split->Panel(dest).FolderBar().InsertTransferred(std::move(*tab));
    bar.ReleaseTab(id);
    RefreshTerminalTabLabel(side);
    target->RefreshTerminalTabLabel(dest);
    split->RefreshPanels();
    target->split->RefreshPanels();
    target->split->Focus();
    target->Activate();
  }

  bool HandleNavigation(BMessage *m) {
    const int source = split->Source();
    switch (m->what) {
    case kFilePanelFavorite:
    case kFilePanelFavoriteNamed:
      try {
        if (m->what == kFilePanelFavorite) {
          const auto &pane = *panes[source];
          const std::filesystem::path folder(pane.Path());
          if (!pane.LocationValid() || pane.IsSearchList() || Listi::IsVirtual(pane.Path()))
            throw std::runtime_error("Open an available local folder first.");
          const auto info = Listi::FavoriteFolderIdentity(folder);
          BMessage accepted(kFilePanelFavoriteNamed);
          accepted.AddString("folder", folder.c_str());
          accepted.AddInt64("device", info.st_dev);
          accepted.AddInt64("inode", info.st_ino);
          const auto leaf = folder.filename().string();
          AskInline(("Favorite name for " + folder.string()).c_str(), "Add favorite",
                    accepted, leaf.empty() ? "Root" : leaf.c_str());
        } else {
          BPath settings;
          if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings) != B_OK)
            throw std::runtime_error("Could not locate your Haiku settings folder.");
          const std::string name = m->GetString("name", "");
          auto result = Listi::AddFilePanelFavorite(settings.Path(), m->GetString("folder", ""),
              name, m->GetInt64("device", -1), m->GetInt64("inode", -1));
          if (result == Listi::FavoriteResult::NameInUse) {
            BMessage retry(*m);
            retry.RemoveName("name");
            AskInline("That favorite name is in use. Choose another:", "Add favorite",
                      retry, name.c_str());
          } else {
            panes[source]->ShowStatus(result == Listi::FavoriteResult::Added
                ? "Added to Haiku file panel favorites." : "This folder is already a favorite.");
          }
        }
      } catch (const std::exception &error) {
        PostMainWindowNotice(error.what());
      }
      break;
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
        if (split->InspectorDetached())
          split->OpenFloatingInspector({});
        else if (prefs.IsSmidja())
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
    case Listi::kDetachInspector:
      if (!PromptBlocksCommand() && split->DetachInspector())
        ApplyWorkspace();
      break;
    case Listi::kDockInspector:
      if (split->DockInspector(*m))
        ApplyWorkspace();
      break;
    case kPreviewSelection:
      if (m->GetInt32("pane", -1) == split->Source() || split->HasPanelPreview())
        split->UpdatePreviews();
      if (m->GetInt32("pane", -1) == split->Source() && m->HasString("operationError"))
        queue.ErrorInfo(m->GetString("operationError", ""), m->GetInt64("operation", 0),
                        m->GetString("path", ""));
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
      changes.RemoveName("operationsInWindow");
      queue.SetSeparateWindow(prefs.operationsInWindow && !operationsWindowSuspended);
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
        sessionLayout(request.localLayout || request.detached.has_value() || request.workingSet.has_value()), openBrowser(std::move(open)), prefs(p), service(svc),
        savedNavigation(svc, saved, writable, queries, queriesWritable, BMessenger(this)) {
    panes[0] = new Pane(0, prefs, shelf.Model());
    panes[1] = new Pane(1, prefs, shelf.Model());
    split = new Listi::WorkspaceSplit(*panes[0], *panes[1], prefs, *menu);
    auto *terminalSplit = new BSplitView(B_VERTICAL, 6);
    terminalDock = std::make_unique<Listi::TerminalDock>(
        terminalSplit, split, split->Panel(0), split->Panel(1), BMessenger(this));
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
    // Suspended tabs outlive the active panes' per-directory subscriptions.
    watch_node(nullptr, B_WATCH_MOUNT, BMessenger(this));
    queue.Start();
    AddCommonFilter(new Listi::WindowKeyFilter());

    Apply();
    PublishPlaces();
    split->Focus();
    ++browserCount;
    if (request.workingSet) {
      const auto &savedWindow = *request.workingSet;
      SetWorkingSetName(savedWindow.GetString("workingSetName", ""));
      SetAlias(savedWindow.GetString("alias", ""));
      for (int side = 0; side < 2; ++side) {
        BMessage panel;
        if (savedWindow.FindMessage("panel", side, &panel) == B_OK)
          split->Panel(side).FolderBar().RestoreTabs(panel);
      }
      split->SetSource(savedWindow.GetInt32("source", 0) == 1 ? 1 : 0);
      split->Focus();
      BRect frame;
      if (savedWindow.FindRect("frame", &frame) == B_OK && frame.IsValid()) {
        ResizeTo(frame.Width(), frame.Height());
        MoveTo(frame.LeftTop());
        MoveOnScreen();
      }
    }
    if (request.position) {
      MoveTo(*request.position);
      MoveOnScreen();
    }
    if (request.detached) {
      split->Panel(0).FolderBar().Adopt(std::move(*request.detached));
      detachedWorkNotice = std::move(request.detachedWorkNotice);
      if (request.cliPlacement) {
        bool transferred = false;
        // Queue() opens windows asynchronously. Never move a session the user
        // has since closed, restarted or placed elsewhere.
        if (request.cliSource.LockTargetWithTimeout(1000000) == B_OK) {
          BLooper *looper = nullptr;
          auto *source = dynamic_cast<MainWindow *>(request.cliSource.Target(&looper));
          if (source)
            transferred = source->terminalDock->TransferTo(request.cliPlacement, *terminalDock);
          looper->Unlock();
        }
        if (transferred)
          RefreshTerminalTabLabel(0);
        else {
          if (!detachedWorkNotice.empty())
            detachedWorkNotice += " ";
          detachedWorkNotice += "CLI not transferred. Check the original window.";
        }
      }
    } else if (!request.path.empty())
      panes[split->Source()]->Navigate(request.path.c_str());
  }
  ~MainWindow() override {
    stop_watching(BMessenger(this));
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
  void ReadHistoryConfig() {
    if (readingConfig) { refreshConfig = true; return; }
    probedFolders = {panes[0]->Path(), panes[1]->Path()};
    readingConfig = Listi::RequestHistoryConfig(BMessenger(this), probedFolders);
  }
  void WindowActivated(bool active) override {
    BWindow::WindowActivated(active);
    if (active) ReadHistoryConfig();
  }
  void MenusBeginning() override {
    BWindow::MenusBeginning();
    const auto &pane = *panes[split->Source()];
    const char *path = pane.Path();
    const auto page = split->Panel(split->Source()).Current();
    const bool flat = page == PanelPage::Flat;
    const auto &bar = split->Panel(split->Source()).FolderBar();
    const bool filePage = page == PanelPage::Files;
    menu->SetTabDestinations(WindowId(), split->Source(), bar.ActiveId(), filePage);
    menu->FindItem(kCopyToWindow)->SetEnabled(filePage || flat);
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

  bool CheckQuitBlockers() {
    if (PreferencesOpenFor(BMessenger(this))) {
      ShowQuitBlocker("Apply or close this window's preferences first.");
      return false;
    }
    if (savedNavigation.Saving()) {
      ShowQuitBlocker("Saved places or searches are still being written. Wait for the write to finish, then try again.");
      return false;
    }
    if (queue.Busy() || clipboard.Busy()) {
      queue.Show();
      ShowQuitBlocker("File operations are still running. Let them finish, or use Cancel all in the operation queue.");
      return false;
    }
    if (!split->FloatingInspectorAllowsQuit() || !InspectorEditorsAllowQuit(this)) {
      ShowQuitBlocker("This window's editor has unsaved changes or is busy. Check the editor before closing this window.");
      return false;
    }
    return true;
  }

  void SuspendOperationsWindow(bool suspend) {
    operationsWindowSuspended = suspend;
    queue.SetSeparateWindow(prefs.operationsInWindow && !operationsWindowSuspended);
  }

  bool HasCLIs() const { return terminalDock->HasSessions(); }
  bool CloseCLIs() {
    if (terminalDock->CloseSessions())
      return true;
    ShowQuitBlocker("A CLI session could not be closed. Close it from its CLI toolbar, then try again.");
    return false;
  }

  bool QuitRequested() override {
    const bool closeCLI = std::exchange(closeCLIOnQuit, false);
    if (!CheckQuitBlockers())
      return false;
    if (browserCount == 1) {
      if (!CheckSharedQuitBlockers())
        return false;
    }
    if (HasCLIs()) {
      if (!closeCLI) {
        if (!cliQuitAlert.IsValid())
          cliQuitAlert = ConfirmClosingCLIs(BMessenger(this), kConfirmWindowCLIQuit, false);
        return false;
      }
      if (!CloseCLIs())
        return false;
    }
    if (clipboard.ArchivePromptPending())
      CloseAction();
    return true;
  }
  void MessageReceived(BMessage *m) override {
    if (m->what == B_NODE_MONITOR && m->GetInt32("opcode", 0) == B_DEVICE_UNMOUNTED) {
      for (int side = 0; side < 2; ++side) {
        split->Panel(side).FolderBar().ForgetUnmountedDevice(m->GetInt32("device", -1));
        RefreshTerminalTabLabel(side);
      }
      return;
    }
    if (m->what == kConfirmWindowCLIQuit) {
      cliQuitAlert = {};
      if (m->GetInt32("which", 0) == 1)
        closeCLIOnQuit = PostMessage(B_QUIT_REQUESTED) == B_OK;
      return;
    }
    if (m->what == Listi::kReadHistoryConfig) {
      readingConfig = false;
      historyPairs = Listi::HistoryPairs(*m);
      automaticEnabled = m->GetBool("enabled", false);
      if (refreshConfig) { refreshConfig = false; ReadHistoryConfig(); }
      return;
    }
    if (m->what == kMoveTabToWindow) {
      MoveTabToWindow(*m);
      return;
    }
    if (m->what == kQuitListi) {
      be_app->PostMessage(B_QUIT_REQUESTED);
      return;
    }
    if (m->what == kNewBrowser || m->what == kSelectedBrowser) {
      auto &pane = *panes[split->Source()];
      std::string path = m->what == kNewBrowser ? pane.SearchRoot() : pane.SelectedFolder();
      if (!path.empty() && openBrowser)
        openBrowser({path, {}, Frame().LeftTop() + BPoint(24, 24)});
      return;
    }
    if (m->what == Listi::kCaptureWorkingWindow) {
      BMessage snapshot(Listi::kCaptureWorkingWindow);
      WorkspaceLayout savedLayout = prefs;
      split->RememberWeights(savedLayout, split->QuickView());
      savedLayout.Archive(snapshot);
      snapshot.AddBool("visibleButtons", prefs.ActionButtonsVisible());
      snapshot.AddInt32("source", split->Source());
      snapshot.AddString("alias", Alias().c_str());
      snapshot.AddRect("frame", Frame());
      for (int side = 0; side < 2; ++side) {
        auto panel = split->Panel(side).FolderBar().ArchiveTabs();
        snapshot.AddMessage("panel", &panel);
      }
      m->SendReply(&snapshot);
      return;
    }
    if (m->what == Listi::kOpenWorkingSet || m->what == Listi::kSaveWorkingSet) {
      Listi::ShowWorkingSets(*this, m->what == Listi::kSaveWorkingSet, openBrowser);
      return;
    }
    if (HandleFolderTab(*m) || HandleWindowAction(m))
      return;
    if (m->what == kAboutListi) {
      std::string text = "Listi " LISTI_PACKAGE_VERSION "\nBuild: " LISTI_BUILD_ID;
      struct utsname os {};
      text += "\nSystem: ";
      if (uname(&os) == 0)
        text += std::string(os.sysname) + " " + os.release + " " + os.version +
                "\nArchitecture: " + os.machine;
      else
        text += "unknown\nArchitecture: unknown";
      text += "\nABI: " __HAIKU_ARCH_ABI;
      text += std::string("\nWorkspace: ") + WorkspaceName(prefs.workspace);
      for (int i = 0; i < (prefs.SinglePane() ? 1 : 2); ++i) {
        text += i == 0 ? "\nPanel 1: " : "\nPanel 2: ";
        switch (split->Panel(i).Current()) {
        case PanelPage::Flat: text += "Flat View"; break;
        case PanelPage::Search: text += "Search"; break;
        case PanelPage::History: text += "Folder history"; break;
        case PanelPage::Preview: text += "Preview"; break;
        case PanelPage::Files: text += ViewModeLabel(panes[i]->CurrentView()); break;
        }
      }
      auto *about = new BAlert("About Listi", text.c_str(), "Close",
                               "Copy diagnostic information");
      if (about->Go() == 1) {
        try {
          Listi::PutPathsAsText({text});
        } catch (const std::exception &error) {
          PostMainWindowNotice(error.what());
        }
      }
      return;
    }
    if (m->what == Listi::kAutomaticHistorySettings) {
      if (Listi::LaunchHistoryHelper(true) != B_OK)
        PostMainWindowNotice("Could not open automatic history. Install the complete Listi package.");
      return;
    }
    if (m->what == Listi::kOpenSavedFolderHistory) {
      if (!PromptBlocksCommand()) Listi::OpenSavedFolderHistory();
      return;
    }
    if (m->what == Listi::kFolderHistory || m->what == Listi::kPreviewHistory) {
      int side = m->GetInt32("pane", split->Source());
      if (PromptBlocksCommand() || !split->ActivateFiles(side)) return;
      const auto &pane = *panes[side];
      if (!pane.LocationValid() || pane.IsSearchList() || pane.Path()[0] != '/')
        PostMainWindowNotice("Open a local folder before using folder history.");
      else if (m->what == Listi::kFolderHistory)
        Listi::OpenFolderHistory(pane.Path());
      else if (!prefs.SinglePane() && !split->QuickView() && split->Panel(1 - side).OpenHistory(pane.Path()))
        split->RefreshPanels();
      else PostMainWindowNotice("Open a second file panel to preview history here.");
      return;
    }
    if (m->what == Listi::kOpenWindowOverview) {
      overview = Listi::OpenWindowOverview(*this, overview);
      return;
    }
    if (m->what == kExternalTab) {
      int side = split->Source();
      const char *path = m->GetString("path", "");
      if (!*path) return;
      if (!split->ActivateFiles(side)) {
        if (!openBrowser || !openBrowser({path, {}, {}}))
          queue.ErrorInfo("Could not open folder.");
        return;
      }
      split->Panel(side).FolderBar().OpenDirectory(path);
      RefreshTerminalTabLabel(side);
      if (IsMinimized()) Minimize(false);
      Activate();
      split->Focus();
      return;
    }
    if (m->what == Listi::kShowLayoutGuide) {
      guide = Listi::OpenLayoutIntroduction(*this, guide, prefs,
                                           m->GetBool("firstRun", false));
      return;
    }
    if (m->what == Listi::kGuideChoice) {
      const int32 value = m->GetInt32("workspace", -1);
      if (value < 0 || value > 3) {
        if (m->GetBool("firstRun", false)) Save();
        return;
      }
      if (!split->PrepareLayoutChange()) return;
      prefs.workspace = Workspace(value);
      prefs.singlePane = split->Source();
      const int32 sidebar = m->GetInt32("sidebarChoice", 0);
      if (sidebar >= 1 && sidebar <= 3) {
        const SidebarMode modes[] = {SidebarMode::Hidden, SidebarMode::FolderTree,
                                     SidebarMode::Shelf};
        for (int i = 0; i < 2; ++i)
          prefs.SetSidebar(i, modes[sidebar - 1]);
      }
      if (!prefs.SinglePane()) {
        prefs.SetPreview(m->GetBool("preview", prefs.Preview()));
        prefs.command = m->GetBool("command", prefs.command);
        if (m->GetBool("buttons", prefs.ActionButtonsVisible()) != prefs.ActionButtonsVisible())
          (prefs.IsSmidja() ? prefs.smidjaButtons : prefs.tviskiptButtons) =
              m->GetBool("buttons", false);
      }
      Preferences before = prefs;
      prefs.ApplyShared(*m);
      if (prefs.folderOpening != before.folderOpening) {
        BMessage delta = prefs.SharedChanges(&before);
        be_app->PostMessage(&delta);
      }
      ApplyWorkspace();
      Save();
      return;
    }
    if (m->what == Listi::kFlatContext) {
      const int pane = m->GetInt32("pane", -1);
      if (pane >= 0 && pane < 2 && split->Panel(pane).Current() == PanelPage::Flat) {
        auto *view = split->Panel(pane).Page(PanelPage::Flat);
        split->FollowFocus(view);
        if (split->Source() == pane)
          view->MessageReceived(m);
      }
      return;
    }
    try {
      Listi::Job job;
      if (HandleCopyFolder(*m, job)) {
        if (job.checkDestinationWindow)
          queue.Enqueue(job);
        return;
      }
    } catch (const std::exception &error) {
      PostMainWindowNotice(error.what());
      return;
    }
    const int source = split->Source();
    if (m->what == kCopyToWindow) {
      if (PromptBlocksCommand())
        return;
      try {
        auto &panel = split->Panel(source);
        Listi::Job job;
        if (panel.Current() == PanelPage::Flat)
          job = Listi::FlatSelection(*panel.Page(PanelPage::Flat));
        else if (panel.Current() == PanelPage::Files) {
          job.sources = panes[source]->Selected();
          job.expectedSources = panes[source]->SelectedLocalIdentities();
        }
        if (job.sources.empty())
          throw std::runtime_error("Select one or more local entries first.");
        for (const auto &path : job.sources)
          if (path.empty() || path.front() != '/' || Listi::IsVirtual(path))
            throw std::runtime_error("Copy to window supports local entries only.");
        job.action = Listi::Action::Copy;
        PickCopyFolder(std::move(job));
      } catch (const std::exception &error) {
        PostMainWindowNotice(error.what());
      }
      return;
    }
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
                           m->what == kClipboardSaveImage || m->what == kMkdir || m->what == kNewFile;
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
    if ((needsOtherFolder && Listi::InterruptedVolumes().Contains(
                          panes[prefs.SinglePane() ? source : 1 - source]->Path())) ||
        (needsCurrentFolder && Listi::InterruptedVolumes().Contains(panes[source]->Path()))) {
      queue.ErrorInfo(Listi::kUnavailableVolumeText);
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
                               [this](const std::string &path) { OpenSelectionPreview(path); }))
      return;
    switch (m->what) {
    case kInlineNotice:
      queue.ErrorInfo(m->GetString("error", "Operation failed"));
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
    case kExportFileList:
      Listi::ShowFileListExport(pane.ExportModel());
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
      if (m->GetInt32("state", 0) == 3 &&
          queue.ErrorDismissed(m->GetInt64("operation", 0), m->GetString("path", "")))
        break;
      [[fallthrough]];
    case kOperationErrorDismissed:
      for (auto p : panes)
        p->MessageReceived(m);
      break;
    case kVolumeUnavailable:
      for (auto *pane : panes)
        pane->MessageReceived(m);
      split->UpdatePreviews();
      break;
    case kQueueWake:
      for (auto &done : queue.TakeEvents()) {
        if (done.what == kOperationErrorDismissed) {
          for (auto *p : panes)
            p->MessageReceived(&done);
          continue;
        }
        queue.PresentEvent(done);
        if (done.what != kJobDone)
          continue;
        clipboard.JobFinished(done);
        if (queue.ErrorDismissed(done.GetInt64("operation", 0),
                                 done.GetString("onlyFailedFile", "")))
          done.AddBool("errorDismissed", true);
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
      m->AddMessenger("origin", BMessenger(this));
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
      if (split->Panel(lister).Current() == PanelPage::History || panes[lister]->IsSearchList()) {
        PostMainWindowNotice("Open a local folder before starting a terminal.");
        break;
      }
      const auto &tabs = split->Panel(lister).FolderBar();
      if (auto prompt = terminalDock->Open(*m, lister, panes[lister]->Path(),
                                           tabs.ActiveId(), tabs.DescribeTab(tabs.ActiveId())))
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
      if (panel.Current() == PanelPage::History || !strcmp(pane.Path(), "@disks") ||
          Listi::IsVirtual(pane.Path())) {
        PostMainWindowNotice("Open a local folder first.");
        break;
      }
      BMessage launch(kOpenPaths);
      launch.AddMessenger("origin", BMessenger(this));
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
  if (request.workingSet) {
    layout.WorkspaceLayout::Load(*request.workingSet);
    bool visible;
    if (!layout.SinglePane() && request.workingSet->FindBool("visibleButtons", &visible) == B_OK)
      (layout.IsSmidja() ? layout.smidjaButtons : layout.tviskiptButtons) = visible;
  }
  if (request.detached) {
    layout.workspace = Workspace::einn;
    layout.singlePane = 0;
  }
  return new MainWindow(layout, service, places, placesWritable,
                        searches, searchesWritable, std::move(request), std::move(open));
}

bool CheckBrowserQuitBlockers(BWindow *window) {
  auto *browser = dynamic_cast<MainWindow *>(window);
  return !browser || browser->CheckQuitBlockers();
}

bool BrowserHasCLIs(BWindow *window) {
  auto *browser = dynamic_cast<MainWindow *>(window);
  return browser && browser->HasCLIs();
}

bool CloseBrowserCLIs(BWindow *window) {
  auto *browser = dynamic_cast<MainWindow *>(window);
  return !browser || browser->CloseCLIs();
}

void SuspendBrowserOperationsWindow(BWindow *window, bool suspend) {
  if (auto *browser = dynamic_cast<MainWindow *>(window))
    browser->SuspendOperationsWindow(suspend);
}
