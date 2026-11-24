// SPDX-License-Identifier: MIT
#pragma once
#include "Common.h"
#include <Menu.h>
#include <MenuItem.h>

inline void AddCopyTextMenu(BMenu &parent, bool names, bool full, bool relative) {
  auto menu = new BMenu("Copy as");
  auto add = [&](const char *label, uint32 action, bool enabled) {
    auto item = new BMenuItem(label, new BMessage(action));
    item->SetEnabled(enabled);
    menu->AddItem(item);
  };
  add("Names", kCopyNames, names);
  add("Full paths", kCopyPaths, full);
  add("Relative paths", kCopyRelative, relative);
  parent.AddItem(menu);
}
