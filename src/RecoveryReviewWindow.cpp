// SPDX-License-Identifier: MIT
#include "RecoveryReviewWindow.h"
#include "AdaptiveScrollView.h"
#include "RecoveryInventory.h"
#include "ReadWorker.h"
#include "ReadStatus.h"
#include "ThemeTextView.h"
#include <Button.h>
#include <LayoutBuilder.h>
#include <Messenger.h>
#include <StringView.h>
#include <Window.h>
#include <exception>

namespace Listi {
namespace {
constexpr uint32 kCheckPaths = 'rcch';
// A stuck identity probe must not exhaust the slots for reading journals.
std::atomic<unsigned> journalReaders{0}, identityReaders{0};
struct ReviewRead {
  std::atomic<bool> closed{false}, done{false};
  std::string text, error;
};

class JournalWorker : public ReadWorker {
  std::string folder;
  std::shared_ptr<ReviewRead> read;
  RecoveryReview mode;
  std::atomic<unsigned> &readers;
public:
  JournalWorker(const std::string &folder, const std::shared_ptr<ReviewRead> &read,
                RecoveryReview mode, std::atomic<unsigned> &readers)
      : ReadWorker("Recovery inventory reader"), folder(folder), read(read), mode(mode),
        readers(readers) {}
  ~JournalWorker() override { --readers; }
  void MessageReceived(BMessage *message) override {
    if (message->what != kCheckPaths) {
      ReadWorker::MessageReceived(message);
      return;
    }
    try {
      read->text = ReviewRecovery(folder, mode, &read->closed);
    } catch (const std::exception &error) {
      read->error = error.what();
    }
    read->done.store(true);
  }
};

class ReviewWindow : public BWindow {
  std::string inventory;
  std::shared_ptr<ReviewRead> pending;
  Listi::ReadWorker *worker = nullptr;
  ThemeTextView *report;
  BStringView *status;
  BButton *check;
  bool hasJournal = false;

  void Collect() {
    if (!pending || !pending->done.load())
      return;
    if (pending->error.empty()) {
      report->SetText(pending->text.c_str());
      hasJournal = true;
    }
    status->SetText(pending->error.c_str());
    RetireReadWorker(worker);
    pending.reset();
    check->SetLabel(hasJournal ? "Check local paths" : "Retry journal");
    check->SetEnabled(true);
    SetPulseRate(0);
  }

  void Load() {
    if (pending)
      return;
    auto mode = hasJournal ? RecoveryReview::CheckLocalPaths : RecoveryReview::JournalOnly;
    auto &readers = hasJournal ? identityReaders : journalReaders;
    if (readers.fetch_add(1) >= 2) {
      --readers;
      status->SetText("Other recovery reads are still pending. Try again later.");
      return;
    }
    try {
      pending = std::make_shared<ReviewRead>();
      worker = new JournalWorker(inventory, pending, mode, readers);
    } catch (const std::exception &error) {
      --readers;
      pending.reset();
      status->SetText(error.what());
      return;
    }
    // StartReadWorker takes ownership even if all registered slots are full.
    BMessage begin(kCheckPaths);
    if (!StartReadWorker(worker) ||
        BMessenger(worker).SendMessage(&begin, static_cast<BHandler *>(nullptr), 0) != B_OK) {
      RetireReadWorker(worker);
      pending.reset();
      status->SetText("Cannot start a recovery reader. Try again later.");
      return;
    }
    check->SetEnabled(false);
    status->SetText(hasJournal ? "Checking local paths…" : "Reading recovery journal…");
    SetPulseRate(100000);
  }
public:
  explicit ReviewWindow(const std::string &folder)
      : BWindow(BRect(0, 0, 610, 365), "Listi recovery review", B_TITLED_WINDOW,
                B_CLOSE_ON_ESCAPE | B_AUTO_UPDATE_SIZE_LIMITS), inventory(folder) {
    report = new ThemeTextView("recovery records", B_DOCUMENT_BACKGROUND_COLOR,
                               B_DOCUMENT_TEXT_COLOR);
    report->MakeEditable(false);
    report->MakeSelectable(true);
    report->SetWordWrap(true);
    auto *scroll = new AdaptiveScrollView("recovery scroll", report, false);
    scroll->SetExplicitMinSize(BSize(320, 140));
    status = new ReadStatus("recovery status", "", [this] { Collect(); });
    check = new BButton("check paths", "Retry journal", new BMessage(kCheckPaths));
    BLayoutBuilder::Group<>(this, B_VERTICAL).SetInsets(8)
        .Add(scroll)
        .AddGroup(B_HORIZONTAL).Add(status).AddGlue().Add(check).End();
    Load();
  }
  ~ReviewWindow() override {
    if (pending)
      pending->closed.store(true);
    RetireReadWorker(worker);
  }
  void MessageReceived(BMessage *message) override {
    if (message->what == kCheckPaths)
      Load();
    else
      BWindow::MessageReceived(message);
  }
};
}
BWindow *OpenRecoveryReview(BRect owner, const std::string &inventory) {
  auto *window = new ReviewWindow(inventory);
  window->CenterIn(owner);
  window->Show();
  return window;
}
}
