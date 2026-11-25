// SPDX-License-Identifier: MIT
#pragma once
#include "PaneModel.h"
#include <Message.h>
#include <View.h>

namespace Listi {
// Only ordinary local directories, never "..", links, or virtual entries.
inline std::string FolderDropTarget(const std::string &location,
                                    const PaneEntry *row) {
  if (!row || row->parent || !row->directory || row->kind != 1 ||
      location.empty() || location.front() != '/' || row->path.empty() ||
      row->path.front() != '/')
    return {};
  return row->path;
}

inline bool IsFileHover(uint32 transit, const BMessage *drag) {
  return transit != B_EXITED_VIEW && transit != B_OUTSIDE_VIEW && drag &&
         (drag->HasRef("refs") || drag->HasString("listi:path"));
}

inline void SetFolderHover(BView *view, std::string &current,
                           const std::string &next) {
  if (current != next) {
    current = next;
    view->Invalidate();
  }
}

inline void DrawFolderHover(BView *view, BRect rect) {
  view->PushState();
  view->SetHighUIColor(B_KEYBOARD_NAVIGATION_COLOR);
  view->SetPenSize(3);
  view->StrokeRect(rect.InsetByCopy(2, 2));
  view->PopState();
}
}
