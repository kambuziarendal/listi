// SPDX-License-Identifier: MIT
#pragma once
#include "ScrollTextView.h"
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace Inspector {
namespace Syntax {
enum class Family;
}

// Plain-text editing surface shared by the embedded and detached editor.
// Each native input event (including a selection replacement) is one undo.
class InspectorTextView : public ScrollTextView {
public:
  InspectorTextView(const std::string &path, const std::string &text);
  void MessageReceived(BMessage *) override;
  void Paste(BClipboard *) override;

  std::string_view Content() const;
  uint64 Revision() const { return revision; }
  bool FinishEdit();
  bool CanUndo() const { return !undo.empty(); }
  bool CanRedo() const { return !redo.empty(); }
  bool ApplyHistory(bool forward);

  void InvalidateColours() { needsColour = true; }
  void RefreshColours();

protected:
  void InsertText(const char *, int32 size, int32 offset,
                  const text_run_array *) override;
  void DeleteText(int32 from, int32 to) override;

private:
  struct Change {
    int32 offset;
    bool insert;
    std::string bytes;
  };
  using Edit = std::vector<Change>;
  static size_t Cost(const Edit &edit);

  Syntax::Family language;
  std::deque<Edit> undo, redo;
  Edit pending;
  size_t undoBytes = 0;
  uint64 revision = 0;
  bool recording = false, composing = false, needsColour = true;
};

} // namespace Inspector
