// SPDX-License-Identifier: MIT
#include "WindowKeyFilter.h"
#include "FolderTabs.h"
#include "Common.h"
#include "InspectorEditor.h"
#include "Pane.h"
#include "TerminalPanel.h"
#include <TextView.h>
#include <Window.h>
#include <cstring>

namespace {
Pane *OwningPane(BView *view) {
  for (; view; view = view->Parent())
    if (auto pane = dynamic_cast<Pane *>(view))
      return pane;
  return nullptr;
}
} // namespace

namespace Listi {
WindowKeyFilter::WindowKeyFilter() : BMessageFilter(B_KEY_DOWN) {}

filter_result WindowKeyFilter::Filter(BMessage *m, BHandler **target) {
  const char *bytes = m->GetString("bytes", "");
  auto window = dynamic_cast<BWindow *>(Looper());
  if (window && HandleInspectorEditorKey(window->CurrentFocus(), *m))
    return B_SKIP_MESSAGE;
  for (auto view = window ? window->CurrentFocus() : nullptr; view;
       view = view->Parent()) {
    if (auto panel = dynamic_cast<Listi::TerminalPanel *>(view)) {
      if (panel->HasFocus())
        return panel->HandleKey(m) ? B_SKIP_MESSAGE : B_DISPATCH_MESSAGE;
      break;
    }
  }
  auto text =
      window ? dynamic_cast<BTextView *>(window->CurrentFocus()) : nullptr;
  // Window menu accelerators otherwise claim Command+A/C/V before the text
  // control can select/copy/paste. Route editing commands to the focused
  // view, not to the file panel. Explicit menu and button clicks still act on
  // files.
  if (bytes[0] == B_ESCAPE) {
    const char *name =
        text && text->Parent() ? text->Parent()->Name() : nullptr;
    if (name && !strcmp(name, "filter")) {
      if (auto parent = OwningPane(text->Parent())) {
        Looper()->PostMessage(kClearFilter, parent);
        return B_SKIP_MESSAGE;
      }
    }
    Looper()->PostMessage(kInlineCancel);
    return B_SKIP_MESSAGE;
  }
  if (text) {
    if (m->GetInt32("modifiers", 0) & B_COMMAND_KEY) {
      int32 key = m->GetInt32("raw_char", bytes[0]);
      uint32 edit = 0;
      switch (key) {
      case 'a':
      case 'A':
        edit = B_SELECT_ALL;
        break;
      case 'c':
      case 'C':
        edit = B_COPY;
        break;
      case 'v':
      case 'V':
        edit = B_PASTE;
        break;
      case 'x':
      case 'X':
        edit = B_CUT;
        break;
      }
      if (edit) {
        Looper()->PostMessage(edit, text);
        return B_SKIP_MESSAGE;
      }
      // Do not invoke file actions while editing a path, filter or command.
      // Command+F remains available to enter the active pane's name filter.
      if (key != 0 && strchr("eEmMrRnNdDiIsSlLtToO", key))
        return B_SKIP_MESSAGE;
      if ((key == 'y' || key == 'Y' || key == 'b' || key == 'B') &&
          (m->GetInt32("modifiers", 0) & B_SHIFT_KEY))
        return B_SKIP_MESSAGE;
    }
    if (bytes[0] == B_FUNCTION_KEY)
      return B_DISPATCH_MESSAGE;
  }
  // Browsing-only shortcuts: text fields and inline questions retain their
  // own editing keys. Resolve the actual focused pane, not a queued role.
  int32 modifiers = m->GetInt32("modifiers", 0);
  int32 raw = m->GetInt32("raw_char", bytes[0]);
  if (!text && window && OwningPane(window->CurrentFocus()) &&
      (modifiers & (B_COMMAND_KEY | B_CONTROL_KEY | B_OPTION_KEY | B_SHIFT_KEY)) ==
          (B_COMMAND_KEY | B_OPTION_KEY)) {
    uint32 tab = 0;
    switch (raw) {
    case 't': case 'T': tab = kTabNew; break;
    case 'w': case 'W': tab = kTabClose; break;
    case '[': tab = kTabPrevious; break;
    case ']': tab = kTabNext; break;
    }
    if (tab) {
      Looper()->PostMessage(tab);
      return B_SKIP_MESSAGE;
    }
  }
  if (!text &&
      (modifiers & (B_COMMAND_KEY | B_CONTROL_KEY | B_OPTION_KEY |
                    B_SHIFT_KEY)) == B_COMMAND_KEY &&
      (raw == '[' || raw == ']')) {
    auto pane = OwningPane(window ? window->CurrentFocus() : nullptr);
    if (pane) {
      Looper()->PostMessage(raw == '[' ? kBack : kForward, pane);
      return B_SKIP_MESSAGE;
    }
  }
  if (!text && (modifiers & B_COMMAND_KEY) && !(modifiers & B_SHIFT_KEY) &&
      strchr("cCxXvV", raw) && raw != 0) {
    auto pane = OwningPane(window ? window->CurrentFocus() : nullptr);
    if (!pane) {
      for (auto *view = window ? window->CurrentFocus() : nullptr; view; view = view->Parent()) {
        if (view->Name() && !strcmp(view->Name(), "flat results"))
          return B_DISPATCH_MESSAGE;
      }
      return B_SKIP_MESSAGE;
    }
  }
  if (bytes[0] == B_TAB) {
    auto view = dynamic_cast<BView *>(*target);
    while (view) {
      if (view->Name() && !strcmp(view->Name(), "files")) {
        Looper()->PostMessage(kSwitch);
        return B_SKIP_MESSAGE;
      }
      view = view->Parent();
    }
  }
  // Function keys are deliberately unassigned.
  return B_DISPATCH_MESSAGE;
}
} // namespace Listi
