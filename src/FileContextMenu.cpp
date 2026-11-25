// SPDX-License-Identifier: MIT
#include "FileContextMenu.h"
#include "Common.h"
#include "FolderHistory.h"
#include "MainWindow.h"
#include "CopyTextMenu.h"
#include "ShelfWindow.h"
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Screen.h>
#include <algorithm>
namespace {
void FolderActions(BMenu &menu, const ContextMenuOptions &options) {
  auto add = [](BMenu &target, const char *label, uint32 what, bool enabled,
                int32 value = 0, bool marked = false) {
    auto message = new BMessage(what);
    message->AddBool("folderContext", true);
    message->AddInt32("value", value);
    auto item = new BMenuItem(label, message);
    item->SetEnabled(enabled);
    item->SetMarked(marked);
    target.AddItem(item);
  };
  if (options.history) {
    auto *history = new BMenu("Folder history");
    add(*history, "Open in window", Listi::kFolderHistory, true);
    add(*history, "Preview in other panel", Listi::kPreviewHistory, options.dual);
    menu.AddItem(history);
  }
  add(menu, "Detach to Einn", Listi::kTabDetach, true);
  const auto sort = options.sort;
  auto sorting = new BMenu("Sort by");
  auto sortItem = [&](const char *label, int column, bool ascending,
                      bool marked) {
    auto message = new BMessage(kSort);
    message->AddBool("folderContext", true);
    message->AddInt32("value", column);
    message->AddBool("ascending", ascending);
    auto item = new BMenuItem(label, message);
    item->SetMarked(marked);
    sorting->AddItem(item);
  };
  const char *columns[] = {"Name", "Size", "Modified"};
  for (int i = 0; i < 3; ++i)
    sortItem(columns[i], i, sort.ascending, sort.column == i);
  sorting->AddSeparatorItem();
  sortItem("Ascending", sort.column, true, sort.ascending);
  sortItem("Descending", sort.column, false, !sort.ascending);
  menu.AddItem(sorting);
  auto views = new BMenu("View");
  for (int32 mode = 0; mode < ViewModeCount; ++mode)
    add(*views, ViewModeLabel(mode), kGridMode, true, mode, options.view == mode);
  add(*views, ViewModeLabel(FlatMode), kGridMode, options.folderLocal, FlatMode);
  views->AddSeparatorItem();
  add(*views, "Remember view for this folder", kRememberView, true, 0,
      options.rememberView);
  add(*views, "Use pane view for this folder", kForgetView, options.rememberView);
  add(*views, "Use workspace default", kGridMode, true, -1);
  menu.AddItem(views);
  if (options.restoreSidebar)
    add(menu, "Show sidebar", kRestoreSidebar, true);
  const bool local = options.folderLocal;
  const bool writable = local || options.folderArchive;
  menu.AddSeparatorItem();
  add(menu, "New file…", kNewFile, local);
  add(menu, "New folder...", kMkdir, writable);
  add(menu, "Paste here", kClipboardPaste, writable);
  menu.AddSeparatorItem();
  add(menu, "Refresh", kRefresh, true);
  add(menu, "Terminal here", kTerminal, local);
  menu.AddSeparatorItem();
  add(menu, "Properties...", kProperties, local);
}
ContextAction Choose(BPopUpMenu &menu, BPoint screen) {
  auto chosen = menu.Go(screen, false, true, false);
  if (!chosen || !chosen->Message())
    return {};
  return {chosen->Message()->what,
          chosen->Message()->GetBool("folderContext", false),
          chosen->Message()->GetInt32("value", 0),
          chosen->Message()->GetBool("ascending", true)};
}
void FileActions(BMenu &menu, const ContextMenuOptions &options) {
  const bool single = options.selected == 1, directory = options.directory;
  const bool local = options.local, dual = options.dual;
  auto add = [&](const char *label, uint32 action, bool enabled = true) {
    auto message = new BMessage(action);
    if (action == kClipboardPaste)
      message->AddBool("folderContext", true);
    auto item = new BMenuItem(label, message);
    item->SetEnabled(enabled);
    menu.AddItem(item);
  };
  add("Open", kInvoke, single);
  if (single && directory) {
    add("Open in new window", kSelectedBrowser);
    add("Detach to Einn", Listi::kTabDetach);
    add("Add to shortcuts…", kAddShortcut, options.addShortcut);
  }
  if (!directory) {
    add("Open with...", kOpenWith, single && local);
    add(options.singlePane ? "Preview beside file list" : "Preview in other panel",
        kPreviewOtherPanel, single);
    add("Preview in window", kView, single);
    add("Edit", kEdit, single && (local || options.archiveRemove));
    const std::string typeLabel =
        options.fileType.empty()
            ? "Select same file type"
            : "Select same file type (" + options.fileType + ")";
    add(typeLabel.c_str(), kSelectType, !options.fileType.empty());
  }
  menu.AddSeparatorItem();
  add("Copy", kClipboardCopy, local || options.archiveRemove);
  add("Cut", kClipboardCut, local || options.archiveRemove);
  AddCopyTextMenu(menu, local && options.selected, options.selected,
                  local && options.folderLocal && options.selected);
  add("Add to Shelf", Listi::kAddToShelf, options.addToShelf);
  add("Paste here", kClipboardPaste,
      options.folderLocal || options.folderArchive);
  add("Copy to other panel", kCopy, dual);
  add("Move to other panel", kMove, (local || options.archiveRemove) && dual);
  menu.AddSeparatorItem();
  if (single)
    add("Rename...", kRename, local || options.archiveRemove);
  else
    add("Bulk rename...", kBulkRename, local && options.folderLocal);
  add("Duplicate...", kDuplicate, single && local);
  add("Create symbolic link...", kCreateLink, single && local);
  add(options.archiveRemove ? "Delete from archive..." : "Move to Trash...",
      kDelete, local || options.archiveRemove);
  auto archives = new BMenu("Archives");
  auto pack = new BMenuItem("Create archive...", new BMessage(kCreateArchive));
  pack->SetEnabled(local);
  archives->AddItem(pack);
  auto extract =
      new BMenuItem("Extract to folder...", new BMessage(kExtractArchive));
  extract->SetEnabled(!local || (single && !directory));
  archives->AddItem(extract);
  auto update =
      new BMenuItem("Add or replace files...", new BMessage(kAddArchive));
  update->SetEnabled(options.archiveAdd);
  archives->AddItem(update);
  auto remove =
      new BMenuItem("Remove selected entries...", new BMessage(kRemoveArchive));
  remove->SetEnabled(options.archiveRemove);
  archives->AddItem(remove);
  menu.AddItem(archives);
  menu.AddSeparatorItem();
  add("Properties...", kProperties, single && local);
}
constexpr uint32 kSelectFiles = 'cfil', kSelectFolder = 'cfld';
void ContextSelector(BMenu &menu, bool folder, bool hasFiles) {
  auto files = new BMenuItem("Selected files", new BMessage(kSelectFiles));
  files->SetMarked(!folder);
  files->SetEnabled(hasFiles);
  menu.AddItem(files);
  auto currentFolder =
      new BMenuItem("This folder", new BMessage(kSelectFolder));
  currentFolder->SetMarked(folder);
  menu.AddItem(currentFolder);
  menu.AddSeparatorItem();
}
} // namespace
ContextAction ShowContextMenu(BPoint point, const ContextMenuOptions &options) {
  BPopUpMenu files("File actions", false, false),
      folder("Folder actions", false, false);
  ContextSelector(files, false, options.selected != 0);
  ContextSelector(folder, true, options.selected != 0);
  FileActions(files, options);
  FolderActions(folder, options);
  // Reserve an anchor that fits either menu. Switching context near an edge
  // must not move the selector under the pointer or lose the original owner.
  float fileWidth, fileHeight, folderWidth, folderHeight;
  files.GetPreferredSize(&fileWidth, &fileHeight);
  folder.GetPreferredSize(&folderWidth, &folderHeight);
  const BRect bounds = BScreen().Frame().InsetByCopy(8, 8);
  const float width =
      options.selected ? std::max(fileWidth, folderWidth) : folderWidth;
  const float height =
      options.selected ? std::max(fileHeight, folderHeight) : folderHeight;
  point.x = std::max(bounds.left, std::min(point.x, bounds.right - width));
  point.y = std::max(bounds.top, std::min(point.y, bounds.bottom - height));
  bool showingFolder = options.folder || !options.selected;
  for (;;) {
    const auto choice = Choose(showingFolder ? folder : files, point);
    if (choice.what == kSelectFiles && options.selected)
      showingFolder = false;
    else if (choice.what == kSelectFolder)
      showingFolder = true;
    else
      return choice;
  }
}
