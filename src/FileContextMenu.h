// SPDX-License-Identifier: MIT
#pragma once
#include <Point.h>
#include "PaneModel.h"
#include <SupportDefs.h>
struct ContextAction {
  uint32 what = 0;
  bool folder = false;
  int value = 0;
  bool ascending = true;
};
struct ContextMenuOptions {
  bool folder = false, history = false;
  size_t selected = 0;
  bool directory = false, local = false, dual = false, folderLocal = false;
  bool archiveAdd = false, archiveRemove = false, folderArchive = false;
  bool restoreSidebar = false, addShortcut = false;
  bool rememberView = false, addToShelf = false, singlePane = false;
  std::string fileType;
  PaneSort sort;
  int view = 0;
  uint32 diskAction = 0;
};
ContextAction ShowContextMenu(BPoint screen, const ContextMenuOptions &options);
