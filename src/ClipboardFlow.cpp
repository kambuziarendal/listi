// SPDX-License-Identifier: MIT
#include "ClipboardFlow.h"
#include "ArchiveActions.h"
#include "Common.h"
#include "InlineAction.h"
#include "MainWindow.h"
#include "OperationQueue.h"
#include "Pane.h"
#include <exception>
#include <utility>

namespace Listi {

ClipboardFlow::ClipboardFlow(InlineAction &actionArea, OperationQueue &queue)
    : actionArea(actionArea), queue(queue) {}

void ClipboardFlow::AskToSaveFile(Pane &pane, ClipboardContent content) {
  Job destination;
  pane.BindTransferDestination(destination);
  auto file = PrepareClipboardFile(destination.destination, content, be_clipboard,
                                   destination.expectedDestination);
  const auto prompt = (content == ClipboardContent::Image
                           ? "Save clipboard PNG in: "
                           : "Save clipboard text in: ") +
                      file.folder;
  BMessage accepted(kClipboardFileAccepted);
  accepted.AddString("newEntryIn", file.folder.c_str());
  if (actionArea.Ask(prompt.c_str(), "Save", accepted,
                     file.suggestedName.c_str()))
    pendingClipboardFile = std::move(file);
}

std::optional<std::string> ClipboardFlow::SaveFile(const std::string &name) {
  if (!pendingClipboardFile)
    return std::nullopt;
  auto file = std::move(*pendingClipboardFile);
  pendingClipboardFile.reset();
  try {
    SaveClipboardFile(file, name);
    return std::move(file.folder);
  } catch (const std::exception &e) {
    PostMainWindowNotice(e.what());
    return std::nullopt;
  }
}

void ClipboardFlow::Begin(uint32 action, Pane &pane) {
  // Accept and cancel are delivered to the window after the prompt closes.
  // Keep the frozen contents (or cut lease) until that reply is processed.
  if (actionArea.Pending() || pendingClipboardFile || archiveClipboardPending) {
    queue.ErrorInfo("Finish or cancel the open question first.");
    return;
  }
  try {
    if (action == kClipboardSaveText || action == kClipboardSaveImage) {
      const auto content = action == kClipboardSaveText
                               ? ClipboardContent::Text
                               : ClipboardContent::Image;
      AskToSaveFile(pane, content);
      return;
    }
    if (action != kClipboardPaste) {
      auto selected = pane.Selected();
      if (action == kCopyPaths) {
        PutPathsAsText(selected);
        pane.ShowStatus(selected.size() == 1 ? "Path copied as text."
                                             : "Paths copied as text.");
        return;
      }
      Job view;
      pane.ExportModel().BindArchiveView(view, pane.Path());
      PutFileClipboard(selected, action == kClipboardCut, be_clipboard,
                       pane.SelectedLocalIdentities(kFileSelectionLimit), view.expectedArchiveSets);
      BString status;
      status
          << int32(selected.size())
          << (action == kClipboardCut
                  ? " marked for moving. Paste in a destination folder."
                  : " copied to clipboard. Paste in a destination folder.");
      pane.ShowStatus(status);
      return;
    }
    if (!clipboardJobs.empty()) {
      queue.ErrorInfo(
          "A clipboard transfer is still running. Let it finish before pasting again.");
      return;
    }
    if (auto content = DetectClipboardSaveContent()) {
      AskToSaveFile(pane, *content);
      return;
    }
    Job destination;
    pane.BindTransferDestination(destination);
    clipboardPaste = PrepareFilePaste(destination.destination, be_clipboard,
                                      destination.expectedArchiveSets,
                                      destination.expectedDestination);
    if (clipboardPaste.jobs.size() == 1 &&
        (clipboardPaste.jobs[0].action == Action::EditArchive ||
         clipboardPaste.jobs[0].action == Action::MoveArchive)) {
      archiveClipboardPending = !clipboardPaste.lease.empty();
      auto prompt = ArchiveActions::ConfirmAddition(clipboardPaste.jobs[0],
                                                   clipboardPaste.lease);
      if (!actionArea.Ask(prompt.text.String(), prompt.button, prompt.accepted))
        CancelPrompt();
      if (!archiveClipboardPending)
        clipboardPaste = {};
      return;
    }
    clipboardMoved.clear();
    for (auto &job : clipboardPaste.jobs)
      EnqueuePaste(job);
    if (clipboardJobs.empty())
      FinishPaste({});
  } catch (const std::exception &e) {
    PostMainWindowNotice(e.what());
  }
}

void ClipboardFlow::CancelPrompt() {
  pendingClipboardFile.reset();
  if (!archiveClipboardPending)
    return;
  FinishPaste({});
  archiveClipboardPending = false;
}

void ClipboardFlow::EnqueuePaste(Job &job) {
  job.tag = ++clipboardSerial;
  if (queue.Enqueue(job))
    clipboardJobs.insert(job.tag);
}

void ClipboardFlow::SubmitArchivePaste(Job &job, const std::string &lease) {
  if (!archiveClipboardPending || lease != clipboardPaste.lease)
    return;
  EnqueuePaste(job);
  clipboardMoved.clear();
  archiveClipboardPending = false;
  if (clipboardJobs.empty())
    FinishPaste({});
}

void ClipboardFlow::JobFinished(const BMessage &done) {
  if (!clipboardJobs.erase(done.GetInt64("tag", 0)))
    return;
  const char *path;
  for (int32 i = 0; done.FindString("moved", i, &path) == B_OK; ++i)
    clipboardMoved.emplace_back(path);
  if (clipboardJobs.empty()) {
    FinishPaste(clipboardMoved);
    clipboardMoved.clear();
  }
}

void ClipboardFlow::FinishPaste(const std::vector<std::string> &moved) {
  try {
    FinishFilePaste(clipboardPaste, moved);
  } catch (const std::exception &e) {
    PostMainWindowNotice(e.what());
  }
  clipboardPaste = {};
}

} // namespace Listi
