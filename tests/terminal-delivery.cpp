// SPDX-License-Identifier: MIT
#include "FolderTree.h"
#include "ImagePreview.h"
#include "Pane.h"
#include <Application.h>
#include <Bitmap.h>
#include <LayoutBuilder.h>
#include <OutlineListView.h>
#include <StringItem.h>
#include <TextView.h>
#include <Window.h>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {
std::atomic<uint32> dropNext{0};
std::atomic<int> dropped{0}, thumbnailPaints{0};
void Check(bool ok, const char *label) {
  std::cout << (ok ? "PASS " : "FAIL ") << label << std::endl;
  if (!ok)
    throw std::runtime_error(label);
}

template<class Predicate>
bool Wait(BWindow *window, Predicate ready) {
  auto until = system_time() + 5000000;
  do {
    window->Lock();
    window->UpdateIfNeeded();
    const bool done = ready();
    window->Unlock();
    if (done)
      return true;
    snooze(20000);
  } while (system_time() < until);
  return false;
}

BWindow *Window(const char *title, BView *child) {
  auto *window = new BWindow(BRect(10, 10, 810, 540), title, B_TITLED_WINDOW, 0);
  BLayoutBuilder::Group<>(window, B_VERTICAL).Add(child);
  window->SetPulseRate(50000);
  window->Show();
  return window;
}
}
extern "C" status_t __real__ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl(
    const BMessenger *, BMessage *, BHandler *, bigtime_t);
extern "C" status_t __wrap__ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl(
    const BMessenger *target, BMessage *message, BHandler *reply, bigtime_t timeout) {
  uint32 expected = message->what;
  if (expected && dropNext.compare_exchange_strong(expected, 0)) {
    ++dropped;
    return B_TIMED_OUT;
  }
  return __real__ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl(
      target, message, reply, timeout);
}
extern "C" void __real__ZN5BView10DrawBitmapEPK7BBitmap5BRectS3_(
    BView *, const BBitmap *, BRect, BRect);
extern "C" void __wrap__ZN5BView10DrawBitmapEPK7BBitmap5BRectS3_(
    BView *view, const BBitmap *picture, BRect source, BRect dest) {
  if (std::strcmp(view->Name(), "file grid") == 0 &&
      picture->Bounds().IntegerWidth() == 1 && picture->Bounds().IntegerHeight() == 1)
    ++thumbnailPaints;
  __real__ZN5BView10DrawBitmapEPK7BBitmap5BRectS3_(view, picture, source, dest);
}

int main() {
  BApplication app("application/x-vnd.Listi-TerminalDelivery");
  char dir[] = "/tmp/Listi-terminal-XXXXXX";
  if (!mkdtemp(dir))
    return 2;
  const std::string note = std::string(dir) + "/note.txt";
  const std::string photo = std::string(dir) + "/photo.bmp";
  std::ofstream(note) << "UI read the terminal reply";
  // 2x2, 24-bit BMP, 8-byte scanlines; native Tracker icons are 16/96 px.
  unsigned char bmp[70]{};
  bmp[0] = 'B'; bmp[1] = 'M'; bmp[2] = sizeof(bmp); bmp[10] = 54;
  bmp[14] = 40; bmp[18] = bmp[22] = 2; bmp[26] = 1; bmp[28] = 24;
  memset(bmp + 54, 180, 16);
  std::ofstream(photo, std::ios::binary).write(reinterpret_cast<char *>(bmp), sizeof(bmp));

  auto *preview = new ImagePreview();
  auto *window = Window("Terminal preview", preview);
  for (int pass = 0; pass < 2; ++pass) {
    window->Lock();
    preview->ShowFile("");
    dropNext = pass == 0 ? 'pird' : 0;
    preview->ShowFile(note);
    window->Unlock();
    Check(Wait(window, [&] {
      auto *text = dynamic_cast<BTextView *>(preview->FindView("preview text"));
      return text && !text->IsHidden() && std::string(text->Text()) == "UI read the terminal reply";
    }), "preview leaves Loading and displays text");
  }
  Check(dropped == 1, "preview completion notification timed out");
  window->Lock(); window->Quit();

  auto *tree = new FolderTree(nullptr);
  window = Window("Terminal tree", tree);
  for (int pass = 0; pass < 2; ++pass) {
    window->Lock();
    tree->Reset();
    dropNext = pass == 0 ? 'trld' : 0;
    tree->ShowPath("/");
    window->Unlock();
    Check(Wait(window, [&] {
      auto *list = dynamic_cast<BOutlineListView *>(tree->FindView("folder tree"));
      if (!list || !list->FullListCountItems())
        return false;
      auto *root = list->FullListItemAt(0);
      auto *child = dynamic_cast<BStringItem *>(list->ItemUnderAt(root, true, 0));
      return child && child->IsEnabled() && std::string(child->Text()) != "Loading...";
    }), "tree leaves pending and expands its real children");
  }
  Check(dropped == 2, "tree completion notification timed out");
  window->Lock(); window->Quit();

  Preferences prefs;
  prefs.SetPaneMode(0, 3);
  auto *pane = new Pane(0, prefs);
  window = Window("Terminal thumbnails", pane);
  for (int pass = 0; pass < 2; ++pass) {
    window->Lock();
    thumbnailPaints = 0;
    dropNext = pass == 0 ? 'thrd' : 0;
    pane->Navigate(dir);
    window->Unlock();
    Check(Wait(window, [&] { return !pane->IsScanning() && thumbnailPaints > 0; }),
          "thumbnail result is consumed and its decoded pixels are painted");
  }
  Check(dropped == 3, "thumbnail completion notification timed out");
  window->Lock(); window->Quit();
  unlink(note.c_str()); unlink(photo.c_str()); rmdir(dir);
}
