// SPDX-License-Identifier: MIT
#pragma once

#include "Common.h"
#include "TerminalPanel.h"
#include <iterator>

struct WorkspaceAction {
  const char *label;
  const char *description;
  uint32 message;
  const char *shortcut;
};

// Array indices are stored in named layouts. Append new commands, never reorder.
inline constexpr WorkspaceAction kWorkspaceActions[] = {
    {"View", "View", kView, "Shift+V"},
    {"Edit", "Edit", kEdit, "E"},
    {"Copy", "Copy to other panel", kCopy, "Shift+C"},
    {"Move", "Move to other panel", kMove, "M"},
    {"Rename", "Rename", kRename, "R"},
    {"Trash", "Move to Trash", kDelete, "D"},
    {"All", "Select all", kAll, "A"},
    {"None", "Deselect all", kNone, "Shift+A"},
    {"Invert", "Invert selection", kInvert, "I"},
    {"New folder", "New folder", kMkdir, "N"},
    {"Terminal", "Terminal here", kTerminal, "T"},
    {"Operations", "Operations", kShowJobs, "O"},
    {"CLI window", "CLI in its own window", Listi::kShowFloatingTerminal,
     "Shift+T"}};
inline const std::vector<int32> kCompactWorkspaceActions = {0, 2, 3, 4, 9};
static_assert(std::size(kWorkspaceActions) == WorkspaceLayout::ActionButtonCount);
