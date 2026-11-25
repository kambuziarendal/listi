// SPDX-License-Identifier: MIT
#include "PaneContextMenu.h"
#include "FileClipboard.h"
#include "FileContextMenu.h"
#include "JobSnapshot.h"
#include "MainWindow.h"
#include "Pane.h"
#include "PropertiesWindow.h"
#include "SavedPlaces.h"
#include "Shelf.h"
#include "ShelfWindow.h"
#include "VirtualFile.h"
#include <exception>
#include <stdexcept>
#include <sys/stat.h>
#include <utility>

namespace Listi {

PaneContextMenuResult RunPaneContextMenu(
    const BMessage &request, Pane &pane, Pane &other, int owner,
    bool singlePane, bool dual, bool rememberView,
    const SavedPlaces &places, bool placesWritable) {
  const auto selected = pane.Selected();
  const std::string folder = pane.Path(), destination = other.Path();
  const bool background = request.GetBool("folderContext", false);
  if (!background && selected.empty())
    return {};
  const bool folderLocal = !pane.IsSearchList() && folder != "@disks" && !Listi::IsVirtual(folder);
  bool local = true;
  for (const auto &path : selected)
    local &= !Listi::IsVirtual(path);
  BMessage snapshot;
  std::exception_ptr fileSnapshotError;
  Listi::Job shelfSnapshot{Listi::Action::Copy, selected, ""};
  bool canAddToShelf = false;
  if (!selected.empty() && local) {
    try {
      snapshot = Listi::MakeFileSelection(selected, false, pane.SelectedLocalIdentities(kFileSelectionLimit));
    } catch (...) {
      fileSnapshotError = std::current_exception();
    }
    // Shelf accepts overlapping folders and distinct hard-link names;
    // the native transfer clipboard has stricter rules for those.
    if (folder != "@disks" &&
        selected.size() <= Listi::Shelf::kMaximumEntries) {
      try {
        shelfSnapshot.expectedSources = pane.SelectedLocalIdentities();
        FreezeLocalJob(shelfSnapshot);
        canAddToShelf = true;
      } catch (const std::runtime_error &) {
        // An unavailable selection cannot be saved to the Shelf.
      }
    }
  }
  struct stat folderBefore {};
  if (folderLocal && (stat(folder.c_str(), &folderBefore) ||
                      !S_ISDIR(folderBefore.st_mode)))
    throw std::runtime_error(
        "The folder is unavailable. Open the menu again.");
  struct stat before {};
  const bool destinationKnown = !stat(destination.c_str(), &before);
  BPoint screen;
  if (request.FindPoint("screen", &screen) != B_OK)
    return {};
  ContextMenuOptions options;
  options.folder = background;
  options.selected = selected.size();
  options.directory = request.GetBool("directory", false);
  options.local = local;
  options.dual = dual && !other.IsSearchList();
  options.folderLocal = folderLocal;
  options.history = folderLocal && pane.HasHistoryHint();
  const auto folderLocation = Listi::ParseLocation(folder);
  options.folderArchive = folderLocation.Archive();
  options.archiveAdd =
      folderLocation.Archive() ||
      (selected.size() == 1 && local && !options.directory);
  options.archiveRemove = !selected.empty();
  std::string selectedArchive;
  for (const auto &path : selected) {
    auto selectedLocation = Listi::ParseLocation(path);
    if (!selectedLocation.Archive() ||
        (!selectedArchive.empty() &&
         selectedArchive != selectedLocation.archive)) {
      options.archiveRemove = false;
      break;
    }
    selectedArchive = selectedLocation.archive;
  }
  options.fileType = pane.SelectedFileType();
  options.sort = pane.Sorting();
  options.view = pane.CurrentView();
  options.singlePane = singlePane;
  options.rememberView = rememberView;
  options.addToShelf = canAddToShelf;
  options.restoreSidebar = pane.SidebarCanBeRestored();
  options.addShortcut =
      options.directory && selected.size() == 1 && placesWritable &&
      !places.Find(selected.front()) &&
      places.Items().size() < Listi::SavedPlaces::Limit;
  const auto choice = ShowContextMenu(screen, options);
  const uint32 action = choice.what;
  if (!action)
    return {};
  if (!pane.IsSource() || selected != pane.Selected() ||
      folder != pane.Path() || destination != other.Path())
    throw std::runtime_error(
        "The panel changed. Open the file menu again.");
  if (action == kTabDetach) {
    BMessage detach(kTabDetach);
    detach.AddInt32("pane", owner);
    return {detach, {}};
  }
  // Browsing a directory (including a volume root) needs no transfer identity.
  if (action == kSelectedBrowser && options.directory && selected.size() == 1)
    return {BMessage(action), {}};
  if (action == kCopyPaths || action == kCopyNames || action == kCopyRelative) {
    CopySelectionText(action, selected, folder);
    pane.ShowStatus("Selection copied as text.");
    return {};
  }
  if (action == kRestoreSidebar) {
    BMessage restore(kRestoreSidebar);
    pane.MessageReceived(&restore);
    return {};
  }
  if (action == kAddShortcut) {
    if (!options.addShortcut)
      return {};
    // A shortcut stores an address, not a clipboard reference. Volume
    // roots and other browsable folders need no clipboard eligibility.
    struct stat shortcut {};
    if (local && (stat(selected.front().c_str(), &shortcut) ||
                  !S_ISDIR(shortcut.st_mode)))
      throw std::runtime_error("The selected folder is unavailable.");
    BMessage command(kPlaceAction);
    command.AddInt32("action", 0);
    command.AddString("path", selected.front().c_str());
    command.AddInt32("pane", owner);
    return {command, {}};
  }
  if (action == Listi::kAddToShelf) {
    if (options.addToShelf)
      return {BMessage(action), std::move(shelfSnapshot.expectedSources)};
    return {};
  }
  if (choice.folder) {
    struct stat folderAfter {};
    if (folderLocal && (stat(folder.c_str(), &folderAfter) ||
                        folderBefore.st_dev != folderAfter.st_dev ||
                        folderBefore.st_ino != folderAfter.st_ino))
      throw std::runtime_error(
          "The folder changed. Open the menu again.");
    if (action == kSort) {
      pane.Sort({choice.value, choice.ascending});
      return {};
    }
    if (action == kGridMode || action == kRememberView ||
        action == kForgetView) {
      BMessage command(action);
      if (action == kGridMode)
        command.AddInt32("mode", choice.value);
      pane.MessageReceived(&command);
      return {};
    }
    if (action == kProperties) {
      OpenFileProperties(folder, BMessenger(&pane));
      return {};
    }
    if (action == kRefresh) {
      pane.Reload();
      return {};
    }
  } else if (local) {
    if (fileSnapshotError)
      std::rethrow_exception(fileSnapshotError);
    Listi::ReadFileClipboard(snapshot);
  }
  if (action == kCopy || action == kMove || action == kCreateLink) {
    struct stat after {};
    if (destinationKnown &&
        (stat(destination.c_str(), &after) ||
         before.st_dev != after.st_dev || before.st_ino != after.st_ino))
      throw std::runtime_error(
          "The destination changed. Open the file menu again.");
  }
  BMessage command(action);
  if (action != kInvoke)
    return {command, {}};
  pane.MessageReceived(&command);
  return {};
}

} // namespace Listi
