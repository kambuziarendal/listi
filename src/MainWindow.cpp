// SPDX-License-Identifier: MIT
#include <stdexcept>
#include "MainWindow.h"
#include "RecoveryInventory.h"
#include "AdaptiveScrollView.h"
#include "ThemeTextView.h"
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
  BMessage error(kServiceError);
  error.AddString("error", text);
  be_app->PostMessage(&error);
}

namespace {
class MainWindow : public BWindow {
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
    auto saved = prefs.Save();
    saved.what = kSavePreferences;
    service.SendMessage(&saved);
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
    const int source = split->Source();
    const auto &target = *panes[1 - source];
    const bool available = !prefs.SinglePane() && split->AllowsCommand(kCopy);
    const bool folder = target.LocationValid() && strcmp(target.Path(), "@disks") != 0;
    toolbar.SetTransferTarget(source, available, folder ? target.Path() : nullptr);
  }
  void ApplyWorkspace() {
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
                                      panes[source]->Path(), savedNavigation.SearchStore());
      break;
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
    case kApplyPreferences:
      prefs.ApplyDialog(*m);
      Apply();
      Save();
      break;
    default:
      return false;
    }
    return true;
  }

public:
  MainWindow(const Preferences &p, BMessenger svc,
             const Listi::SavedPlaces &saved, bool writable,
             const Listi::SavedSearches &queries, bool queriesWritable)
      : BWindow(BRect(70, 70, 1170, 710), "Listi 0.38.0", B_TITLED_WINDOW,
                B_AUTO_UPDATE_SIZE_LIMITS),
        prefs(p), service(svc),
        savedNavigation(svc, saved, writable, queries, queriesWritable) {
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
    menu->SetFolderTree(page == PanelPage::Files && pane.CanToggleTree(),
                        SidebarHasTree(pane.Sidebar()));
    const bool canSaveClipboard =
        !actionArea->Pending() && Listi::CanSaveClipboardIn(path);
    const bool canUndoBulkRename =
        !actionArea->Pending() && queue.CanUndoBulkRename();
    menu->SetFileActions(pane.CurrentView(), flat, split->CanOpenFlat(),
                         canSaveClipboard, canUndoBulkRename);
  }

  bool QuitRequested() override {
    if (!InspectorEditorsAllowQuit() || !FilePropertiesAllowQuit() || !OpenWithAllowQuit() ||
        !ArchiveEditorsAllowQuit())
      return false;
    if (savedNavigation.Saving()) {
      PostMainWindowNotice(
          "Saved places or searches are still being written. Please try quitting again shortly.");
      return false;
    }
    if (queue.Busy() || clipboard.Busy()) {
      queue.Show();
      PostMainWindowNotice("File operations are still running. Let them finish, or use "
             "Cancel all before quitting.");
      return false;
    }
    if (clipboard.ArchivePromptPending())
      CloseAction();
    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
  }
  void MessageReceived(BMessage *m) override {
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
      for (auto p : panes) {
        p->MessageReceived(m);
        if (m->HasString("destination") &&
            Listi::ParentLocation(m->GetString("destination", "")) == p->Path())
          p->Reload();
      }
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
    case kReviewRecovery: {
      std::string text;
      try {
        text = queue.Busy() ? "Wait for the active operation before reviewing recovery records."
                            : Listi::ReviewRecovery();
      } catch (const std::exception &error) {
        text = error.what();
      }
      auto *review = new BWindow(BRect(0, 0, 610, 365),
                                 "Listi recovery review", B_TITLED_WINDOW,
                                 B_CLOSE_ON_ESCAPE | B_AUTO_UPDATE_SIZE_LIMITS);
      auto *report = new ThemeTextView("recovery records",
                                       B_DOCUMENT_BACKGROUND_COLOR,
                                       B_DOCUMENT_TEXT_COLOR);
      report->MakeEditable(false);
      report->MakeSelectable(true);
      report->SetWordWrap(true);
      report->SetText(text.c_str());
      auto *scroll = new Listi::AdaptiveScrollView("recovery scroll", report, false);
      scroll->SetExplicitMinSize(BSize(320, 140));
      BLayoutBuilder::Group<>(review, B_VERTICAL)
          .SetInsets(8)
          .Add(scroll);
      review->CenterIn(Frame());
      review->Show();
      break;
    }
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
                          const Listi::SavedSearches &searches, bool searchesWritable) {
  return new MainWindow(prefs, service, places, placesWritable,
                        searches, searchesWritable);
}
