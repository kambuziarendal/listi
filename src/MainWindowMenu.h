// SPDX-License-Identifier: MIT
#pragma once
#include <MenuBar.h>

struct WorkspaceAction;
struct WorkspaceLayout;

constexpr uint32 kResumeArchiveEdits = 'ared', kEditExternally = 'edex',
                 kNetwork = 'netw',
                 kOpenSystem = 'osys', kToggleBank = 'atbk',
                 kToggleFullBank = 'atbf';

class MainWindowMenu : public BMenuBar {
public:
  MainWindowMenu();

  void SetTransferDirection(const char *direction);
  void SetWorkspace(const WorkspaceLayout &layout, bool quickView);
  void SetFolderTree(bool available, bool visible);
  void SetPreview(const WorkspaceLayout &layout, bool showingQuickView,
                  bool inspectorDetached = false);
  void SetFileActions(int32 viewMode, bool flat, bool canOpenFlatView,
                      bool canSaveClipboard, bool canUndoBulkRename);
  void SetFunctionButtonState(bool available, bool requested, bool overflow,
                              bool fullAvailable, bool full);
  void ClearFunctionActions();
  void AddFunctionAction(const WorkspaceAction &action);

private:
  void AddFilesMenu();
  void AddSelectMenu();
  void AddViewMenu();
  void AddListiMenu();

  BMenuItem *saveClipboardTextItem, *saveClipboardImageItem, *undoBulkRenameItem;
  BMenuItem *einnMenu, *askrMenu, *tviskiptMenu, *smidjaMenu;
  BMenuItem *previewMenu, *inspectorDockItem, *quickViewMenu, *flatViewItem, *bankToggle,
            *fullBankToggle, *treeToggle;
  BMenu *fileView, *compactActions;
};
