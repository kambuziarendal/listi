// SPDX-License-Identifier: MIT
#include "BitmapMailbox.h"
#include "ImagePreview.h"
#include "TerminalReplies.h"
#include "VirtualFile.h"
#include <Application.h>
#include <iostream>

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  BApplication app("application/x-vnd.Kambuzi-Listi-PreviewTest");
  int checks = 0, failures = 0;
  auto check = [&](bool ok, const char *label) {
    ++checks;
    failures += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << label << std::endl;
  };
  Listi::TransferControl control;
  try {
    std::unique_ptr<BBitmap> image(LoadPreviewBitmap(argv[1], control));
    check(image && image->Bounds().Width() == 319 && image->Bounds().Height() == 239,
          "native decoder reads synthetic 320x240 image");
    std::unique_ptr<BBitmap> thumb(LoadPreviewBitmap(argv[1], control, 128));
    check(thumb && thumb->Bounds().Width() < 128 && thumb->Bounds().Height() < 128,
          "native thumbnail is bounded to 128 pixels");
    BitmapMailbox mailbox(1);
    auto discarded = mailbox.Put(image.release());
    auto current = mailbox.Put(thumb.release());
    check(!mailbox.Take(discarded), "bounded replies discard superseded ownership");
    auto received = mailbox.Take(current);
    check(received && !mailbox.Take(current), "reply transfers ownership exactly once");
    auto cleared = mailbox.Put(received.release());
    mailbox.Clear();
    check(!mailbox.Take(cleared), "navigation clears unconsumed bitmap replies");
    TerminalReplies terminal;
    std::atomic<int64> generation{1};
    for (uint32 what : {'pird', 'thrd', 'trld'}) {
      BMessage result(what);
      result.AddInt64("serial", 1);
      result.AddString("path", "/fixture");
      terminal.Deliver(result, generation, [] { return B_TIMED_OUT; });
      auto retained = terminal.Take();
      check(retained.size() == 1 && retained.front().what == what &&
                std::string(retained.front().GetString("path", "")) == "/fixture" &&
                terminal.Take().empty(),
            "lost UI notification retains its complete result exactly once");
      terminal.Deliver(result, generation, [] { return B_OK; });
      check(terminal.Take().empty(), "normal retry does not duplicate completion");
    }
    int calls = 0;
    control.proceed = [&] { return ++calls < 3; };
    bool cancelled = false;
    try {
      std::unique_ptr<BBitmap> ignored(LoadPreviewBitmap(argv[1], control));
    } catch (const Listi::Cancelled &) {
      cancelled = true;
    }
    check(cancelled && calls >= 3, "cancellation during native translation is propagated");
  } catch (const std::exception &error) {
    std::cerr << error.what() << std::endl;
    ++failures;
  }
  std::cout << checks << " preview checks, " << failures << " failures\n";
  return failures != 0;
}
