// SPDX-License-Identifier: MIT
#include "ImagePreview.h"
#include "InspectorDocument.h"
#include "InspectorEditor.h"
#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <Bitmap.h>
#include <Screen.h>
#include <algorithm>
#include <vector>
#include <Clipboard.h>
#include <Font.h>
#include <Message.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <TypeConstants.h>
#include <Window.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace {
void Check(bool ok, const char *claim) {
  std::cout << (ok ? "PASS " : "FAIL ") << claim << std::endl;
  if (!ok)
    std::exit(1);
}
template <class T> T *View(BWindow *window, const char *name) {
  auto *view = dynamic_cast<T *>(window->FindView(name));
  Check(view != nullptr, name);
  return view;
}
void Send(BMessenger target, uint32 what) {
  BMessage action(what), reply;
  Check(target.SendMessage(&action, &reply, 2000000, 2000000) == B_OK,
        "native editor command processed");
}
std::string Read(const std::string &path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), {}};
}

BButton *VisibleButton(BView *parent, const std::string &label) {
  for (int32 i = 0; i < parent->CountChildren(); ++i) {
    auto *child = parent->ChildAt(i);
    if (child->IsHidden(child))
      continue;
    auto *button = dynamic_cast<BButton *>(child);
    if (button && label == button->Label())
      return button;
    if (auto *nested = VisibleButton(child, label))
      return nested;
  }
  return nullptr;
}

// Read displayed pixels, rather than trusting the inspector caption or bitmap ID.
std::vector<uint32> Capture(ImagePreview *view, BRect local) {
  auto screenRect = view->ConvertToScreen(local);
  BBitmap image(local.OffsetToCopy(0, 0), B_RGB32);
  if (BScreen(view->Window()).ReadBitmap(&image, false, &screenRect) != B_OK)
    return {};
  std::vector<uint32> pixels;
  for (int y = 0; y <= image.Bounds().Height(); ++y) {
    auto *row = static_cast<uint8 *>(image.Bits()) + y * image.BytesPerRow();
    for (int x = 0; x <= image.Bounds().Width(); ++x, row += 4)
      pixels.push_back((uint32(row[2]) << 16) | (uint32(row[1]) << 8) | row[0]);
  }
  return pixels;
}

void CheckDiscardPreview(BApplication &app, const std::string &folder) {
  const auto path = folder + "/discard.txt", photo = folder + "/green.png";
  const std::string original = "Original text before editing.\nSecond preview line.\n";
  std::ofstream(path) << original;
  const unsigned char png[] = {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
      0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40,
      0x08, 0x02, 0x00, 0x00, 0x00, 0x25, 0x0b, 0xe6, 0x89, 0x00, 0x00, 0x00,
      0x51, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0xed, 0xcf, 0x31, 0x0d, 0x00,
      0x30, 0x08, 0x00, 0x30, 0x98, 0x09, 0x44, 0x61, 0x17, 0x81, 0x53, 0xc1,
      0x41, 0xd2, 0x3a, 0x68, 0xd6, 0x74, 0x5c, 0xf6, 0xe2, 0x38, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x81, 0x7d, 0x1f, 0xa7, 0x20,
      0x01, 0x96, 0x05, 0x71, 0x90, 0x7a, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
      0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
  std::ofstream(photo, std::ios::binary).write(reinterpret_cast<const char *>(png), sizeof(png));
  auto *host = new BWindow(BRect(30,30,352,410), "Inspector pixels", B_TITLED_WINDOW, 0);
  auto *view = new ImagePreview();
  host->AddChild(view);
  view->ResizeTo(312, 370);
  view->ShowFile(path);
  host->Show();
  host->Activate();
  auto await = [&](auto test, const char *label) {
    const auto deadline = system_time() + 10000000;
    bool passed = false;
    while (!passed && system_time() < deadline) {
      snooze(20000);
      host->Lock();
      passed = test();
      host->Unlock();
    }
    Check(passed, label);
  };
  const BRect excerpt(12, 38, 230, 88);
  std::vector<uint32> before;
  await([&] {
    if (!VisibleButton(view, "Edit")) return false;
    before = Capture(view, excerpt);
    return !before.empty() && std::count_if(before.begin(), before.end(), [&](uint32 p) {
      return p != before.back();
    }) > 80;
  }, "text appears before editing");
  host->Lock();
  Check(view->BeginEditing(path), "open text in Inspector");
  auto *body = View<BTextView>(host, "editor text");
  body->Insert("unwanted draft");
  auto *done = VisibleButton(view, "Done");
  Check(done && done->Invoke() == B_OK, "ask to close dirty editor");
  host->Unlock();
  bool discarded = false;
  const auto deadline = system_time() + 8000000;
  while (!discarded && system_time() < deadline) {
    snooze(20000);
    for (int i = 0; i < app.CountWindows(); ++i) {
      auto *alert = dynamic_cast<BAlert *>(app.WindowAt(i));
      if (!alert || !alert->Lock()) continue;
      auto *button = alert->ButtonAt(0);
      if (button && std::string(button->Label()) == "Discard")
        discarded = button->Invoke() == B_OK;
      alert->Unlock();
      if (discarded) break;
    }
  }
  Check(discarded, "choose Discard in the native confirmation");
  await([&] { return !view->HasEditor(); }, "draft editor closed");
  host->Lock();
  view->ShowFile(photo);
  host->Unlock();
  auto imageVisible = [&] {
    if (!VisibleButton(view, "Image…")) return false;
    const auto pixels = Capture(view, BRect(145, 169, 165, 189));
    return !pixels.empty() && std::count(pixels.begin(), pixels.end(), 0x17b44b) ==
                              static_cast<int>(pixels.size());
  };
  await(imageVisible, "PNG pixels cover the inspector centre after Discard");
  host->Lock();
  view->ShowFile(path);
  host->Unlock();
  await([&] { return VisibleButton(view, "Edit") && Capture(view, excerpt) == before; },
        "text renders with the same margins, line wrapping and pixels as before");
  host->Lock();
  view->Hide();
  view->Show();
  view->ShowFile(photo);
  host->Unlock();
  await(imageVisible, "PNG still draws after hide and re-enable");
  Check(Read(path) == original, "discard never wrote the draft");
  host->Lock(); host->Quit();
  unlink(photo.c_str()); unlink(path.c_str());
}

void CheckInlineEditor(BApplication &app, const std::string &path,
                       const std::string &otherPath) {
  const std::string original = "original text\n";
  const std::string other = "other file\n";
  const std::string draft = original + "unsaved text\n";
  std::ofstream(path) << original;
  std::ofstream(otherPath) << other;
  auto *host = new BWindow(BRect(40, 40, 500, 420), "Quick view editor test",
                           B_TITLED_WINDOW, 0);
  auto *preview = new ImagePreview();
  host->AddChild(preview);
  preview->ResizeTo(440, 350);
  preview->SetQuickViewCloseVisible(true);
  preview->ShowFile(path);
  host->Show();

  const bigtime_t previewDeadline = system_time() + 10000000;
  bool editInvoked = false;
  while (!editInvoked && system_time() < previewDeadline) {
    if (!host->Lock())
      Check(false, "lock Quick view while waiting for the preview");
    if (auto *edit = VisibleButton(preview, "Edit"); edit && edit->IsEnabled()) {
      auto *close = View<BButton>(host, "close quick view");
      Check(!close->IsHidden(close) && edit->Frame().right < close->Frame().left,
            "async text preview puts Edit to the left of Close");
      Check(edit->Invoke() == B_OK, "invoke quick view Edit button");
      editInvoked = true;
    }
    host->Unlock();
    if (!editInvoked)
      snooze(20000);
  }
  Check(editInvoked, "the loaded text preview offers Edit");
  Send(BMessenger(preview), 'test');
  Check(host->Lock(), "inspect the inline editor");
  Check(preview->HasEditor() && app.CountWindows() == 1,
        "Edit keeps the document inside Quick view");
  auto *body = View<BTextView>(host, "editor text");
  Check(original == body->Text(), "inline editor loads the selected text");
  body->Select(body->TextLength(), body->TextLength());
  body->Insert("unsaved text\n");
  body->Select(2, 7);
  Check(!preview->ShowFile(otherPath) && preview->File() == path &&
            preview->HasEditor() && draft == body->Text() && Read(path) == original,
        "changing selection keeps the unsaved draft without writing it");
  Check(preview->BeginEditing(path) &&
            body == host->FindView("editor text") && draft == body->Text(),
        "editing the same path keeps the existing draft");
  auto *detach = VisibleButton(preview, "Detach");
  Check(detach && detach->Invoke() == B_OK, "invoke inline editor Detach");
  host->Unlock();

  Send(BMessenger(preview), 'test');
  Check(host->Lock(), "inspect Quick view after detaching");
  Check(!preview->HasEditor() && preview->File() == otherPath,
        "detaching removes the inline editor and restores the latest preview");
  host->Unlock();
  BWindow *window = nullptr;
  for (int32 i = 0; i < app.CountWindows(); ++i)
    if (auto *candidate = app.WindowAt(i); candidate != host)
      window = candidate;
  Check(window && window->Lock(), "detached editor has its own window");
  BMessenger editor(window);
  body = View<BTextView>(window, "editor text");
  int32 from, to;
  body->GetSelection(&from, &to);
  Check(draft == body->Text() && from == 2 && to == 7,
        "detach preserves the unsaved draft and text selection");
  window->Unlock();
  Check(!InspectorEditorsAllowQuit(), "detached draft still counts as unsaved");
  Send(editor, 'ieSv');
  Check(Read(path) == draft && Read(otherPath) == other &&
            InspectorEditorsAllowQuit(),
        "detached Save writes the original path, not the new selection");
  editor.SendMessage(B_QUIT_REQUESTED);
  for (int i = 0; i < 100 && editor.IsValid(); ++i)
    snooze(20000);
  Check(!editor.IsValid() && host->Lock(), "close the detached editor and host");
  host->Quit();
}
}

int main() {
  BApplication app("application/x-vnd.Listi-InspectorEditorTest");
  char tmp[] = "/tmp/Listi-editor-XXXXXX";
  Check(mkdtemp(tmp) != nullptr, "private test directory");
  const std::string path = std::string(tmp) + "/example.py";
  const std::string disk = "#!/bin/python\r\nreturn café\r\nreturn two\r\n";
  const std::string original = "#!/bin/python\nreturn café\nreturn two\n";
  const std::string modified = "#!/bin/python\nyield café\nyield two\n";
  std::ofstream(path) << disk;
  OpenInspectorEditor(path, nullptr);
  auto window = app.WindowAt(0);
  Check(window && window->Lock(), "editor opened for complete document");
  BMessenger editor(window);
  auto body = View<BTextView>(window, "editor text");
  auto find = View<BTextControl>(window, "editor find");
  auto replacement = View<BTextControl>(window, "editor replacement");
  auto line = View<BTextControl>(window, "editor line");
  auto position = View<BStringView>(window, "editor position");
  Check(original == body->Text(),
        "file opened without CR characters in editing buffer");
  window->Unlock();
  Send(editor, 'ieTk');
  Check(window->Lock(), "inspect syntax colours");
  BFont font;
  rgb_color comment, keyword;
  body->GetFontAndColor(0, &font, &comment);
  body->GetFontAndColor(original.find("return"), &font, &keyword);
  Check(comment != keyword,
        "script comments and keywords have distinct colours");
  Check(!body->DoesWordWrap(), "source opens without line wrapping");
  window->Unlock();
  Send(editor, 'ieWr');
  Send(editor, 'ieFn');
  Check(window->Lock(), "find and replace panel");
  Check(body->DoesWordWrap() && !find->IsHidden(),
        "wrap toggled and find bar is visible");
  find->SetText("return");
  replacement->SetText("yield");
  window->Unlock();
  Send(editor, 'ieNx');
  Check(window->Lock(), "find result");
  int32 from, to;
  body->GetSelection(&from, &to);
  Check(original.substr(from, to - from) == "return", "find selects the match");
  window->Unlock();
  Send(editor, 'ieRa');
  Check(window->Lock(), "replacement in memory");
  Check(modified == body->Text() && Read(path) == disk,
        "replace all changes only the draft");
  body->MakeFocus();
  window->Unlock();
  Check(!InspectorEditorsAllowQuit(),
        "unsaved document prevents application quit");
  OpenInspectorEditor(path, nullptr);
  Check(app.CountWindows() == 1,
        "opening the same file preserves its existing draft");

  editor.SendMessage(B_QUIT_REQUESTED);
  BAlert *confirm = nullptr;
  for (int i = 0; i < 100 && !confirm; ++i) {
    for (int32 j = 0; j < app.CountWindows(); ++j)
      if (auto alert = dynamic_cast<BAlert *>(app.WindowAt(j)))
        confirm = alert;
    if (!confirm)
      snooze(20000);
  }
  Check(confirm && confirm->Lock(), "closing a dirty editor asks what to do");
  Check(confirm->ButtonAt(1)->Invoke() == B_OK, "cancel close");
  confirm->Unlock();

  Send(editor, 'ieUn');
  Check(window->Lock(), "undo result");
  Check(original == body->Text(), "one undo restores every replaced match");
  window->Unlock();
  Check(InspectorEditorsAllowQuit(), "undo to original clears dirty state");
  Check(window->Lock(), "prepare identical replacement");
  replacement->SetText("return");
  window->Unlock();
  Send(editor, 'ieRp');
  Check(InspectorEditorsAllowQuit(),
        "replacing a match with itself leaves the draft unchanged");
  Send(editor, 'ieRe');
  Check(window->Lock(), "inspect redo after identical replacement");
  Check(modified == body->Text(), "identical replacement preserves redo");
  window->Unlock();
  Send(editor, 'ieLn');
  Check(window->Lock(), "line navigation");
  line->SetText("2");
  window->Unlock();
  Send(editor, 'ieJp');
  Send(editor, 'ieTk');
  Check(window->Lock(), "inspect logical line");
  body->GetSelection(&from, &to);
  Check(modified == body->Text() && from == int32(modified.find("yield")) &&
            std::string(position->Text()) == "Ln 2, Col 1",
        "redo and go to logical line work with wrapping enabled");
  const int32 nextLine = modified.find('\n', from);
  body->Select(nextLine, nextLine);
  window->Unlock();
  Send(editor, 'ieTk');
  Check(window->Lock(), "inspect UTF-8 column");
  Check(std::string(position->Text()) == "Ln 2, Col 11",
        "column counts UTF-8 characters, not bytes");
  window->Unlock();
  Send(editor, 'ieSv');
  Check(Read(path) == "#!/bin/python\r\nyield café\r\nyield two\r\n" &&
            InspectorEditorsAllowQuit(),
        "save persists edits, preserves CRLF and permits quit");

  const auto clipboardName = "Listi-editor-paste-" + std::to_string(getpid());
  BClipboard clipboard(clipboardName.c_str());
  Check(clipboard.Lock(), "lock private paste fixture clipboard");
  const std::string copied = "one\r\ntwo\rthree";
  Check(clipboard.Clear() == B_OK &&
            clipboard.Data()->AddData("text/plain", B_MIME_TYPE, copied.data(),
                                      copied.size()) == B_OK &&
            clipboard.Commit() == B_OK,
        "copy mixed clipboard line endings");
  clipboard.Unlock();
  Check(window->Lock(), "paste onto a selection in the saved document");
  const int32 pasteAt = modified.find("yield");
  body->Select(pasteAt, pasteAt + 5);
  body->Paste(&clipboard);
  const std::string pasted = "#!/bin/python\none\ntwo\nthree café\nyield two\n";
  body->GetSelection(&from, &to);
  if (pasted != body->Text() || from != pasteAt + 13 || to != from) {
    const std::string actual(body->Text(), body->TextLength());
    std::cerr << "Paste selection: " << from << " to " << to
              << "; expected caret " << pasteAt + 13 << "\nPaste bytes:";
    for (unsigned char byte : actual)
      std::cerr << " " << static_cast<int>(byte);
    std::cerr << "\nExpected bytes:";
    for (unsigned char byte : pasted)
      std::cerr << " " << static_cast<int>(byte);
    std::cerr << std::endl;
  }
  Check(pasted == body->Text() && from == pasteAt + 13 && to == from,
        "paste normalizes CRLF and CR and advances the caret");
  window->Unlock();
  Send(editor, 'ieUn');
  Check(window->Lock(), "inspect paste undo");
  Check(modified == body->Text(),
        "one undo restores the replaced selection text");
  window->Unlock();
  Send(editor, 'ieRe');
  Send(editor, 'ieSv');
  const std::string pastedDisk =
      "#!/bin/python\r\none\r\ntwo\r\nthree café\r\nyield two\r\n";
  Check(Read(path) == pastedDisk && InspectorEditorsAllowQuit(),
        "redo and save preserve the document's original CRLF format");

  Check(clipboard.Lock(), "lock oversized paste fixture clipboard");
  const std::string oversized =
      std::string(Inspector::kDocumentLimit, 'x') + "\r\n";
  Check(clipboard.Clear() == B_OK &&
            clipboard.Data()->AddData("text/plain", B_MIME_TYPE,
                                      oversized.data(), oversized.size()) ==
                B_OK &&
            clipboard.Commit() == B_OK,
        "copy text that cannot fit after normalization");
  clipboard.Unlock();
  Check(window->Lock(), "inspect oversized normalized paste");
  body->Select(pasteAt, pasteAt + 3);
  body->Paste(&clipboard);
  body->GetSelection(&from, &to);
  Check(pasted == body->Text() && from == pasteAt && to == pasteAt + 3,
        "oversized normalized paste leaves the draft and selection intact");
  window->Unlock();
  editor.SendMessage(B_QUIT_REQUESTED);
  for (int i = 0; i < 100 && editor.IsValid(); ++i)
    snooze(20000);
  Check(!editor.IsValid(), "clean editor closes");
  const std::string inlinePath = std::string(tmp) + "/inline.txt";
  const std::string otherPath = std::string(tmp) + "/other.txt";
  CheckInlineEditor(app, inlinePath, otherPath);
  CheckDiscardPreview(app, tmp);
  unlink(inlinePath.c_str());
  unlink(otherPath.c_str());
  unlink(path.c_str());
  rmdir(tmp);
}
