// SPDX-License-Identifier: MIT
#include "ShelfController.h"

#include "JobSnapshot.h"
#include "MainWindow.h"
#include "OperationQueue.h"
#include "Pane.h"
#include "ShelfWindow.h"
#include <exception>

namespace Listi {

ShelfController::ShelfController(BMessenger owner,
                                 const std::array<Pane *, 2> &panes)
    : fOwner(owner), fPanes(panes) {}

bool ShelfController::Handle(const BMessage &message, Pane &active,
                             OperationQueue &queue) {
  switch (message.what) {
  case kShowShelf:
    OpenShelfWindow(fOwner, fShelf, fShelfWindow,
                    active.ConvertToScreen(active.Bounds()));
    break;
  case kAddToShelf:
    Add(active);
    break;
  case kRefreshShelf:
    Refresh();
    break;
  case kShelfChanged:
    // The Shelf window already rendered its own change.
    RefreshSidebars();
    break;
  case kSubmitShelfTransfer: {
    auto job = DecodeSubmittedJob(message, {Action::Copy, Action::Move});
    if (job && !job->sources.empty() &&
        job->expectedSources.size() == job->sources.size() &&
        job->expectedDestination.device >= 0)
      queue.Enqueue(*job);
    break;
  }
  default:
    return false;
  }
  return true;
}

void ShelfController::Add(Pane &active,
                          const std::map<std::string, NodeIdentity> &expected) {
  try {
    const auto added = fShelf->Add(
        active.Selected(), expected.empty() ? active.SelectedLocalIdentities() : expected);
    BString status;
    if (added)
      status << int32(added) << " added to Shelf. Open it from the Listi menu.";
    else
      status = "Already on the Shelf.";
    active.ShowStatus(status);
    Refresh();
  } catch (const std::exception &e) {
    PostMainWindowNotice(e.what());
  }
}

void ShelfController::RefreshSidebars() {
  for (auto *pane : fPanes)
    pane->RefreshShelf();
}

void ShelfController::Refresh() {
  RefreshSidebars();
  if (fShelfWindow.IsValid()) {
    BMessage refresh(kRefreshShelf);
    fShelfWindow.SendMessage(&refresh, (BHandler *)nullptr, 0);
  }
}

} // namespace Listi
