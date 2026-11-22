// SPDX-License-Identifier: MIT
#include "MainWindowMenu.h"
#include "Common.h"
#include "FlatView.h"
#include "MainWindow.h"
#include "PaneSidebar.h"
#include "ShelfWindow.h"
#include "WorkspaceActions.h"
#include "WorkspaceEditor.h"
#include <MenuItem.h>

namespace {
BMenuItem *AddCommand(BMenu *menu, const char *label, uint32 action,
                      char shortcut = 0, uint32 modifiers = 0) {
  auto *item = new BMenuItem(label, new BMessage(action), shortcut, modifiers);
  menu->AddItem(item);
  return item;
}
}

MainWindowMenu::MainWindowMenu() : BMenuBar("menu") {
  AddFilesMenu();
  AddSelectMenu();
  AddViewMenu();
  AddListiMenu();
}

void MainWindowMenu::AddFilesMenu() {
  auto files = new BMenu("Files");
  AddCommand(files, "Copy", kClipboardCopy, 'C');
  AddCommand(files, "Cut", kClipboardCut, 'X');
  AddCommand(files, "Paste into active folder", kClipboardPaste, 'V');
  auto clipboard = new BMenu("Save clipboard as");
  saveClipboardTextItem = AddCommand(clipboard, "Save clipboard text as file...", kClipboardSaveText);
  saveClipboardImageItem = AddCommand(clipboard, "Save clipboard image as PNG...", kClipboardSaveImage);
  files->AddItem(clipboard);
  files->AddSeparatorItem();
  AddCommand(files, "View", kView, 'V', B_SHIFT_KEY);
  AddCommand(files, "Edit", kEdit, 'E');
  AddCommand(files, "Edit in external editor", kEditExternally);
  AddCommand(files, "Copy to other panel", kCopy, 'C', B_SHIFT_KEY);
  AddCommand(files, "Move to other panel", kMove, 'M');
  AddCommand(files, "Rename", kRename, 'R');
  AddCommand(files, "Bulk rename...", kBulkRename);
  AddCommand(files, "New folder", kMkdir, 'N');
  AddCommand(files, "Move to Trash", kDelete, 'D');
  AddCommand(files, "Restore from Trash", kRestore);
  AddCommand(files, "Delete permanently...", kPermanentDelete);
  undoBulkRenameItem = AddCommand(files, "Undo last bulk rename", kUndoBulkRename);
  undoBulkRenameItem->SetEnabled(false);
  AddCommand(files, "Open with...", kOpenWith, 'O', B_SHIFT_KEY);
  AddCommand(files, "Open with system application", kOpenSystem);
  AddCommand(files, "Add to Shelf", Listi::kAddToShelf, 'Y', B_SHIFT_KEY);
  auto more = new BMenu("More file actions");
  AddCommand(more, "Duplicate...", kDuplicate, 'D', B_SHIFT_KEY);
  AddCommand(more, "Create symbolic link...", kCreateLink, 'L', B_SHIFT_KEY);
  files->AddItem(more);
  auto archives = new BMenu("Archives");
  AddCommand(archives, "Create archive...", kCreateArchive, 'B', B_SHIFT_KEY);
  AddCommand(archives, "Extract to folder...", kExtractArchive, 'E', B_SHIFT_KEY);
  AddCommand(archives, "Resume archive edits...", kResumeArchiveEdits);
  AddCommand(archives, "Add or replace files...", kAddArchive);
  AddCommand(archives, "Remove selected entries...", kRemoveArchive);
  archives->AddSeparatorItem();
  AddCommand(archives, "Forget archive passwords", 'apfg');
  files->AddItem(archives);
  AddCommand(files, "Properties...", kProperties, 'I', B_SHIFT_KEY);
  files->AddSeparatorItem();
  AddCommand(files, "Quit", B_QUIT_REQUESTED, 'Q');
  AddItem(files);
}

void MainWindowMenu::AddSelectMenu() {
  auto select = new BMenu("Select");
  AddCommand(select, "All", kAll, 'A');
  AddCommand(select, "None", kNone, 'A', B_SHIFT_KEY);
  AddCommand(select, "Invert", kInvert, 'I');
  AddCommand(select, "Same file type", kSelectType);
  AddCommand(select, "By pattern…", kSelectPattern);
  AddItem(select);
}

void MainWindowMenu::AddViewMenu() {
  auto viewMenu = new BMenu("View");
  auto workspace = new BMenu("Workspace");
  auto einnMessage = new BMessage(kWorkspace);
  einnMessage->AddInt32("workspace", int32(Workspace::einn));
  auto askrMessage = new BMessage(kWorkspace);
  askrMessage->AddInt32("workspace", int32(Workspace::askr));
  auto smidjaMessage = new BMessage(kWorkspace);
  smidjaMessage->AddInt32("workspace", int32(Workspace::smidja));
  askrMenu = new BMenuItem(WorkspaceMenuLabel(Workspace::askr), askrMessage);
  einnMenu = new BMenuItem(WorkspaceMenuLabel(Workspace::einn), einnMessage);
  tviskiptMenu = new BMenuItem(WorkspaceMenuLabel(Workspace::tviskipt),
                               new BMessage(kWorkspace));
  smidjaMenu =
      new BMenuItem(WorkspaceMenuLabel(Workspace::smidja), smidjaMessage);
  workspace->AddItem(einnMenu);
  workspace->AddItem(askrMenu);
  workspace->AddItem(tviskiptMenu);
  workspace->AddItem(smidjaMenu);
  workspace->AddSeparatorItem();
  AddCommand(workspace, "Customize workspace…", kOpenWorkspaceEditor);
  viewMenu->AddItem(workspace);
  previewMenu = AddCommand(viewMenu, "Inspector panel", kPreviewToggle, 'P');
  quickViewMenu = AddCommand(viewMenu, "Quick view", kQuickView, 'P', B_SHIFT_KEY);
  treeToggle = AddCommand(viewMenu, "Folder tree", kToggleTree);
  viewMenu->AddSeparatorItem();
  fileView = new BMenu("Active pane view");
  for (int32 i = 0; i < ViewModeCount; ++i) {
    auto choice = new BMessage(kGridMode);
    choice->AddInt32("mode", i);
    fileView->AddItem(new BMenuItem(ViewModeLabel(i), choice));
  }
  flatViewItem = AddCommand(fileView, ViewModeLabel(FlatMode), Listi::kFlatView);
  auto resetView = new BMessage(kGridMode);
  resetView->AddInt32("mode", -1);
  fileView->AddSeparatorItem();
  fileView->AddItem(new BMenuItem("Use workspace default", resetView));
  viewMenu->AddItem(fileView);
  bankToggle = AddCommand(viewMenu, "Show function buttons", kToggleBank);
  fullBankToggle = AddCommand(viewMenu, "Full action buttons", kToggleFullBank);
  compactActions = new BMenu("Function actions");
  viewMenu->AddItem(compactActions);
  AddItem(viewMenu);
}

void MainWindowMenu::AddListiMenu() {
  auto view = new BMenu("Listi");
  AddCommand(view, "Switch panel", kSwitch, 'S');
  AddCommand(view, "Same folder in other panel", kSame, 'S', B_SHIFT_KEY);
  AddCommand(view, "Connect to server...", kNetwork, 'K');
  AddCommand(view, "Refresh", kRefresh, 'L');
  AddCommand(view, "Filter names", kFilter, 'F');
  AddCommand(view, "Find files...", kSearch, 'F', B_SHIFT_KEY);
  AddCommand(view, "Compare folders...", kCompareFolders);
  auto terminalMenu = new BMenu("Terminal here");
  AddCommand(terminalMenu, "Active lister", kTerminal, 'T');
  AddCommand(terminalMenu, "Left CLI", Listi::kLeftCLI);
  AddCommand(terminalMenu, "Right CLI", Listi::kRightCLI);
  terminalMenu->AddSeparatorItem();
  auto dockMenu = new BMenu("Dock position");
  AddCommand(dockMenu, "Below file panels", Listi::kTerminalBelow);
  AddCommand(dockMenu, "Right of file panels", Listi::kTerminalBeside);
  AddCommand(dockMenu, "Above file panels", Listi::kTerminalAbove);
  AddCommand(dockMenu, "Left of file panels", Listi::kTerminalLeft);
  terminalMenu->AddItem(dockMenu);
  AddCommand(terminalMenu, "Detached Listi CLI", Listi::kShowFloatingTerminal, 'T', B_SHIFT_KEY);
  AddCommand(terminalMenu, "External terminal window", Listi::kTerminalWindow);
  view->AddItem(terminalMenu);
  AddCommand(view, "Operations", kShowJobs, 'O');
  AddCommand(view, "Show Shelf", Listi::kShowShelf);
  view->AddSeparatorItem();
  AddCommand(view, "Settings…", kPreferences, ',');
  AddItem(view);
}

void MainWindowMenu::SetTransferDirection(const char *direction) {
  for (auto action : {kCopy, kMove}) {
    if (auto item = FindItem(action)) {
      BString label(action == kCopy ? "Copy" : "Move");
      if (direction)
        label << ' ' << direction;
      else
        label << " to other panel";
      if (label != item->Label())
        item->SetLabel(label);
      const bool available = direction != nullptr;
      if (item->IsEnabled() != available)
        item->SetEnabled(available);
    }
  }
}

void MainWindowMenu::SetWorkspace(const WorkspaceLayout &layout, bool quickView) {
  if (auto item = FindItem(kSame))
    item->SetEnabled(!layout.SinglePane());
  if (auto item = FindItem(kSwitch)) {
    const bool treeFocus = layout.workspace == Workspace::askr;
    item->SetLabel(treeFocus ? "Switch tree/list focus" : "Switch panel");
    item->SetEnabled(!quickView && (treeFocus || !layout.SinglePane()));
  }
  if (auto item = FindItem(kFilter))
    item->SetEnabled(!layout.IsEinn());
  einnMenu->SetMarked(layout.IsEinn());
  askrMenu->SetMarked(layout.workspace == Workspace::askr);
  tviskiptMenu->SetMarked(layout.workspace == Workspace::tviskipt);
  smidjaMenu->SetMarked(layout.IsSmidja());
}

void MainWindowMenu::SetFolderTree(bool available, bool visible) {
  treeToggle->SetEnabled(available);
  treeToggle->SetMarked(available && visible);
}

void MainWindowMenu::SetPreview(const WorkspaceLayout &layout, bool showingQuickView) {
  quickViewMenu->SetMarked(showingQuickView);
  previewMenu->SetMarked(layout.Preview() && !layout.SinglePane());
  previewMenu->SetEnabled(!showingQuickView && !layout.SinglePane());
}

void MainWindowMenu::SetFileActions(int32 viewMode, bool flat, bool canOpenFlatView,
                                    bool canSaveClipboard, bool canUndoBulkRename) {
  for (int32 i = 0; i < ViewModeCount; ++i)
    fileView->ItemAt(i)->SetMarked(!flat && i == viewMode);
  flatViewItem->SetMarked(flat);
  flatViewItem->SetEnabled(canOpenFlatView);
  saveClipboardTextItem->SetEnabled(canSaveClipboard);
  saveClipboardImageItem->SetEnabled(canSaveClipboard);
  undoBulkRenameItem->SetEnabled(canUndoBulkRename);
}

void MainWindowMenu::SetFunctionButtonState(bool available, bool requested, bool overflow,
                                           bool fullAvailable, bool full) {
  bankToggle->SetEnabled(available);
  bankToggle->SetMarked(available && requested);
  fullBankToggle->SetEnabled(fullAvailable);
  fullBankToggle->SetMarked(full);
  compactActions->Superitem()->SetEnabled(overflow);
}

void MainWindowMenu::ClearFunctionActions() {
  while (auto *item = compactActions->RemoveItem(int32(0)))
    delete item;
}

void MainWindowMenu::AddFunctionAction(const WorkspaceAction &action) {
  AddCommand(compactActions, action.description, action.message);
}
