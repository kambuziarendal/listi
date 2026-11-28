// SPDX-License-Identifier: MIT
#include "InspectorTextView.h"
#include "InspectorDocument.h"
#include "InspectorSyntax.h"
#include <Beep.h>
#include <Clipboard.h>
#include <Input.h>
#include <Message.h>
#include <TypeConstants.h>
#include <algorithm>
#include <utility>

namespace Inspector {

InspectorTextView::InspectorTextView(const std::string &path,
                                     const Document &source, std::function<void()> limitNotice)
    : ScrollTextView("editor text"), onLimit(std::move(limitNotice)),
      document(source), language(Syntax::Language(path, source.Text())) {
  SetFontAndColor(be_fixed_font);
  SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
  SetLowUIColor(B_DOCUMENT_BACKGROUND_COLOR);
  SetHighUIColor(B_DOCUMENT_TEXT_COLOR);
  SetStylable(true);
  SetDoesUndo(false);
  SetWordWrap(language == Syntax::Family::Plain || language == Syntax::Family::Markdown);
  SetInsets(8, 6, 8, 6);
  SetTabWidth(be_fixed_font->StringWidth("    "));
  SetText(source.Text().c_str());
  recording = true;
}

void InspectorTextView::MessageReceived(BMessage *message) {
  if (message->what == B_INPUT_METHOD_EVENT) {
    const int32 op = message->GetInt32("be:opcode", -1);
    if (op == B_INPUT_METHOD_STARTED)
      composing = true;
    else if (op == B_INPUT_METHOD_STOPPED ||
             (op == B_INPUT_METHOD_CHANGED &&
              message->GetBool("be:confirmed", false)))
      composing = false;
  }
  BTextView::MessageReceived(message);
}

void InspectorTextView::RejectOversize() const {
  beep();
  if (onLimit)
    onLimit();
}

bool InspectorTextView::Fits(int32 from, int32 to, std::string_view insert) const {
  auto current = Content();
  const size_t budget = document.TextBudget();
  if (insert.size() > budget)
    return false;
  return document.EncodedTextBytes(current.substr(0, from)) +
         document.EncodedTextBytes(insert) +
         document.EncodedTextBytes(current.substr(to)) <= budget;
}

void InspectorTextView::KeyDown(const char *bytes, int32 count) {
  if (count > 0 && (static_cast<unsigned char>(bytes[0]) >= 32 ||
                     bytes[0] == B_ENTER || bytes[0] == B_TAB) &&
      bytes[0] != B_DELETE && !(modifiers() & B_COMMAND_KEY)) {
    int32 from, to;
    GetSelection(&from, &to);
    if (!Fits(from, to, {bytes, static_cast<size_t>(count)})) {
      RejectOversize();
      return;
    }
  }
  BTextView::KeyDown(bytes, count);
}

void InspectorTextView::Paste(BClipboard *clipboard) {
  const void *data = nullptr;
  ssize_t size = 0;
  if (!IsEditable() || !clipboard->Lock()) {
    BTextView::Paste(clipboard);
    return;
  }
  const auto *message = clipboard->Data();
  std::string_view bytes;
  if (message &&
      message->FindData("text/plain", B_MIME_TYPE, &data, &size) == B_OK &&
      size > 0)
    bytes = {static_cast<const char *>(data), static_cast<size_t>(size)};
  bytes = bytes.substr(0, bytes.find('\0'));
  if (bytes.empty()) {
    clipboard->Unlock();
    return;
  }
  int32 from, to;
  GetSelection(&from, &to);
  const size_t keptBytes = TextLength() - (to - from);
  const size_t limit = std::min(kDocumentLimit, static_cast<size_t>(MaxBytes()));
  const size_t available = limit - std::min(limit, keptBytes);
  // CRLF is the only shrinking conversion, so oversized clipboard data
  // cannot produce a fitting edit even in the best case.
  if (bytes.size() / 2 + bytes.size() % 2 > available) {
    clipboard->Unlock();
    RejectOversize();
    return;
  }
  std::string text;
  text.reserve(std::min(bytes.size(), available));
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (bytes[i] == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n')
      continue;
    if (text.size() == available) {
      clipboard->Unlock();
      RejectOversize();
      return;
    }
    text += bytes[i] == '\r' ? '\n' : bytes[i];
  }
  clipboard->Unlock();

  if (!Fits(from, to, text)) {
    RejectOversize();
    return;
  }
  Select(from, to); // Cancel inline input before replacing the selection.
  Delete(from, to);
  const auto insertedBytes = static_cast<int32>(text.size());
  Insert(from, text.data(), insertedBytes);
  Select(from + insertedBytes, from + insertedBytes);
  ScrollToSelection();
}

std::string_view InspectorTextView::Content() const {
  return {Text(), static_cast<size_t>(TextLength())};
}

void InspectorTextView::InsertText(const char *text, int32 size, int32 at,
                                   const text_run_array *) {
  if (recording && size > 0 && !Fits(at, at, {text, static_cast<size_t>(size)})) {
    RejectOversize();
    return;
  }
  if (recording && size > 0)
    pending.push_back({at, true, std::string(text, size)});
  // Files are plain text. Never import font runs from a rich clipboard.
  ScrollTextView::InsertText(text, size, at, nullptr);
  ++revision;
  needsColour = true;
}

void InspectorTextView::DeleteText(int32 from, int32 to) {
  if (recording && to > from)
    pending.push_back({from, false, std::string(Text() + from, to - from)});
  ScrollTextView::DeleteText(from, to);
  ++revision;
  needsColour = true;
}

size_t InspectorTextView::Cost(const Edit &edit) {
  size_t bytes = 0;
  for (const auto &change : edit)
    bytes += sizeof(Change) + change.bytes.size();
  return bytes;
}

bool InspectorTextView::FinishEdit() {
  if (pending.empty() || composing)
    return false;
  undoBytes += Cost(pending);
  undo.push_back(std::move(pending));
  pending.clear();
  redo.clear();
  while (undo.size() > 1 &&
         (undo.size() > 512 || undoBytes > 16 * 1024 * 1024)) {
    undoBytes -= Cost(undo.front());
    undo.pop_front();
  }
  return true;
}

bool InspectorTextView::ApplyHistory(bool forward) {
  auto &from = forward ? redo : undo;
  auto &to = forward ? undo : redo;
  if (from.empty())
    return false;
  auto &edit = from.back();
  const size_t bytes = Cost(edit);
  int32 caret = 0;
  recording = false;
  auto apply = [&](const Change &change) {
    caret = change.offset;
    const int32 size = static_cast<int32>(change.bytes.size());
    if (forward == change.insert) {
      Insert(caret, change.bytes.data(), size);
      caret += size;
    } else {
      Delete(caret, caret + size);
    }
  };
  if (forward) {
    for (const auto &change : edit)
      apply(change);
    undoBytes += bytes;
  } else {
    for (auto it = edit.rbegin(); it != edit.rend(); ++it)
      apply(*it);
    undoBytes -= bytes;
  }
  recording = true;
  to.push_back(std::move(edit));
  from.pop_back();
  Select(caret, caret);
  ScrollToSelection();
  return true;
}

void InspectorTextView::RefreshColours() {
  // Applying font runs during composition cancels the active input method.
  if (!needsColour || composing)
    return;
  using namespace Syntax;
  const auto content = Content();
  const rgb_color normal = ui_color(B_DOCUMENT_TEXT_COLOR);
  const rgb_color background = ui_color(B_DOCUMENT_BACKGROUND_COLOR);
  // Do not feed thousands of BFont objects into the native layout on large
  // files. Plain formatting never limits editing, navigation or saving.
  auto runs = ColourRuns(content, content.size() > 128 * 1024 ? Family::Plain
                                                              : language);
  if (runs.size() > 4096)
    runs = {{0, Ink::Text}};
  const bool dark = background.Brightness() < 128;
  const rgb_color palette[] = {
      normal,
      dark ? rgb_color{143, 193, 151, 255} : rgb_color{52, 105, 61, 255},
      dark ? rgb_color{243, 177, 135, 255} : rgb_color{146, 65, 23, 255},
      dark ? rgb_color{159, 192, 255, 255} : rgb_color{45, 75, 154, 255},
      dark ? rgb_color{217, 168, 240, 255} : rgb_color{120, 52, 142, 255}};
  text_run_array *styles = BTextView::AllocRunArray(runs.size());
  if (!styles)
    return;
  for (size_t i = 0; i < runs.size(); ++i) {
    styles->runs[i].offset = runs[i].offset;
    styles->runs[i].font = *be_fixed_font;
    styles->runs[i].color = palette[static_cast<size_t>(runs[i].ink)];
  }
  SetRunArray(0, TextLength(), styles);
  BTextView::FreeRunArray(styles);
  needsColour = false;
}

} // namespace Inspector
