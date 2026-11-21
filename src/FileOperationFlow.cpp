// SPDX-License-Identifier: MIT
#include "FileOperationFlow.h"

#include "ArchiveJobs.h"
#include "ArchiveWindow.h"
#include "BulkRename.h"
#include "BulkRenameWindow.h"
#include "ClipboardFlow.h"
#include "Common.h"
#include "InlineAction.h"
#include "JobSnapshot.h"
#include "MainWindow.h"
#include "NamedEntry.h"
#include "OperationQueue.h"
#include "Pane.h"
#include "TransferCommand.h"
#include "VirtualFile.h"
#include <exception>
#include <utility>

namespace Listi {
namespace {
constexpr uint32 kNamedEntryAccepted = 'nacc', kConfirm = 'cfir';
constexpr char kPendingQuestion[] = "Finish or cancel the open question first.";
} // namespace

FileOperationFlow::FileOperationFlow(InlineAction &actionArea,
                                     OperationQueue &queue,
                                     ClipboardFlow &clipboard, BMessenger owner)
    : actionArea(actionArea), queue(queue), clipboard(clipboard), owner(owner) {
}

bool FileOperationFlow::CanBegin() {
  if (!actionArea.Pending())
    return true;
  queue.ErrorInfo(kPendingQuestion);
  return false;
}

void FileOperationFlow::AskInline(const char *text, const char *button,
                                  const BMessage &accepted, const char *initial,
                                  const char *alternative) {
  if (!actionArea.Ask(text, button, accepted, initial, alternative))
    queue.ErrorInfo(kPendingQuestion);
}

void FileOperationFlow::ShowArchive(uint32 action, const Pane &source,
                                    const Pane &target) {
  if (!CanBegin())
    return;
  if (action == kAddArchive)
    archiveActions.ShowAddPicker(source, owner);
  else if (action == kRemoveArchive) {
    auto prompt = ArchiveActions::ConfirmRemoval(source);
    AskInline(prompt.text.String(), prompt.button, prompt.accepted);
  } else {
    archiveActions.ShowJobWindow(action == kExtractArchive, source, target,
                                 owner);
  }
}

void FileOperationFlow::ConfirmArchiveAddition(const Job &job) {
  auto prompt = ArchiveActions::ConfirmAddition(job, {});
  AskInline(prompt.text.String(), prompt.button, prompt.accepted);
}

void FileOperationFlow::AskNamedEntry(Action action, const Pane &source,
                                      const Pane &target) {
  if (!CanBegin())
    return;
  auto job = PrepareNamedEntry(action, source.Selected(), target.Path(),
                               source.SelectedLocalIdentities());
  const bool duplicate = action == Action::Duplicate;
  std::string text = std::string(duplicate ? "Duplicate: " : "Link to: ") +
                     job.sources[0] + "\nIn: " + job.destination;
  if (!duplicate)
    text += "\nRelative: path from this folder. Absolute: full target path.";
  auto accepted = EncodeJob(job, kNamedEntryAccepted);
  accepted.AddString("newEntryIn", job.destination.c_str());
  AskInline(text.c_str(), duplicate ? "Duplicate" : "Relative link", accepted,
            job.entryName.c_str(), duplicate ? nullptr : "Absolute link");
}

void FileOperationFlow::ShowBulkRename(const Pane &source) {
  if (!CanBegin())
    return;
  if (bulkRenameWindow.IsValid() &&
      bulkRenameWindow.SendMessage(kBulkRename) == B_OK)
    return;
  auto job = FreezeBulkRename(source.Path(), source.Selected(),
                              source.SelectedLocalIdentities());
  bulkRenameWindow = OpenBulkRenameWindow(std::move(job), owner);
}

void FileOperationFlow::SubmitTransfer(Job job) {
  auto transfer = PrepareTransfer(std::move(job));
  if (transfer.action == Action::MoveArchive ||
      transfer.action == Action::EditArchive)
    ConfirmArchiveAddition(transfer);
  else
    queue.Enqueue(transfer);
}

void FileOperationFlow::BeginOperation(Action action, const Pane &source,
                                       const Pane &target, bool singlePane) {
  if (!CanBegin())
    return;
  const bool isTransfer = action == Action::Copy || action == Action::Move;
  if (singlePane && isTransfer) {
    PostMainWindowNotice(
        "Use Copy or Cut, then navigate and Paste. Panel transfers need Tvískipt or Smiðja.");
    return;
  }
  Job job;
  job.action = action;
  if (action != Action::MakeDirectory) {
    job.sources = source.Selected();
    job.expectedSources = source.SelectedLocalIdentities();
  }
  if (isTransfer)
    job.destination = target.Path();
  if ((action == Action::Trash || action == Action::Delete) &&
      !job.sources.empty() && ParseLocation(job.sources.front()).Archive()) {
    ShowArchive(kRemoveArchive, source, target);
    return;
  }
  if (action != Action::MakeDirectory && job.sources.empty()) {
    PostMainWindowNotice("Select one or more files in the active panel first.");
    return;
  }
  const bool rename = action == Action::Rename;
  if (rename || action == Action::MakeDirectory) {
    if (rename && job.sources.size() > 1) {
      ShowBulkRename(source);
      return;
    }
    job.destination = source.Path();
    if (job.destination == "@disks") {
      PostMainWindowNotice("Open a folder first.");
      return;
    }
    FreezeLocalJob(job, job.destination);
    FreezeArchiveTargets(job);
    const auto destination = ParseLocation(job.destination);
    if (destination.Archive())
      PrepareArchiveEditTarget(destination.archive);
    auto accepted = EncodeJob(job, kNameAccepted);
    const auto name = rename ? LocationLeaf(job.sources.front()) : "New folder";
    AskInline(rename ? "Rename selected entry"
                     : "Create a folder in the active panel",
              rename ? "Rename" : "Create folder", accepted, name.c_str());
    return;
  }
  if (isTransfer) {
    SubmitTransfer(std::move(job));
    return;
  }
  FreezeLocalJob(job);
  FreezeArchiveTargets(job);
  auto accepted = EncodeJob(job, kConfirm);
  BString text;
  const char *button;
  if (action == Action::Trash) {
    button = "Move to Trash";
    text << "Move " << int32(job.sources.size())
         << " selected entries to this volume's Trash?\n\nYou can restore "
            "them from Trash.\n"
         << job.sources[0].c_str();
  } else if (action == Action::Restore) {
    button = "Restore";
    text << "Restore " << int32(job.sources.size())
         << " selected entries to their original folders?\n\nExisting files "
            "will not be overwritten.";
  } else {
    button = "Delete permanently";
    text << "Permanently delete " << int32(job.sources.size())
         << " selected entries?\n\nThis cannot be undone.\n"
         << job.sources[0].c_str();
  }
  AskInline(text.String(), button, accepted);
}

bool FileOperationFlow::Handle(const BMessage &message, const Pane &source,
                               const Pane &other, bool singlePane) {
  const Pane &target = singlePane ? source : other;
  try {
    switch (message.what) {
    case kCreateArchive:
    case kExtractArchive:
    case kAddArchive:
    case kRemoveArchive:
      ShowArchive(message.what, source, target);
      break;
    case kArchiveAddChosen:
      ConfirmArchiveAddition(ArchiveActions::ReadAdditions(message));
      break;
    case kArchiveEditAccepted: {
      auto job = DecodeSubmittedJob(message,
                                    {Action::EditArchive, Action::MoveArchive});
      if (!job || message.GetInt32("which", 0) != 1)
        break;
      const std::string lease = message.GetString("clipboardLease", "");
      if (!lease.empty() && !clipboard.AcceptArchivePaste(*job, lease))
        break;
      queue.Enqueue(*job);
      break;
    }
    case kArchiveAccepted: {
      if (auto job = DecodeSubmittedJob(
              message, {Action::CreateArchive, Action::ExtractArchive}))
        queue.Enqueue(*job);
      break;
    }
    case kCopy:
      BeginOperation(Action::Copy, source, target, singlePane);
      break;
    case kMove:
      BeginOperation(Action::Move, source, target, singlePane);
      break;
    case kDelete:
      BeginOperation(Action::Trash, source, target, singlePane);
      break;
    case kRestore:
      BeginOperation(Action::Restore, source, target, singlePane);
      break;
    case kPermanentDelete:
      BeginOperation(Action::Delete, source, target, singlePane);
      break;
    case kDuplicate:
      AskNamedEntry(Action::Duplicate, source, target);
      break;
    case kCreateLink:
      AskNamedEntry(Action::Link, source, target);
      break;
    case kNamedEntryAccepted: {
      auto job = DecodeSubmittedJob(message, {Action::Link, Action::Duplicate});
      if (job) {
        job->entryName = message.GetString("name", "");
        job->relativeLink = message.GetInt32("which", 1) != 2;
        queue.Enqueue(*job);
      }
      break;
    }
    case kRename:
      BeginOperation(Action::Rename, source, target, singlePane);
      break;
    case kBulkRename:
      ShowBulkRename(source);
      break;
    case kUndoBulkRename:
      if (!message.IsSourceRemote() && !actionArea.Pending() &&
          queue.EnqueueBulkRenameUndo())
        queue.Show();
      break;
    case kBulkRenameAccepted: {
      if (auto job = DecodeSubmittedJob(message, {Action::BulkRename}))
        queue.Enqueue(*job);
      break;
    }
    case kMkdir:
      BeginOperation(Action::MakeDirectory, source, target, singlePane);
      break;
    case kConfirm: {
      auto job = DecodeSubmittedJob(
          message, {Action::Trash, Action::Restore, Action::Delete});
      if (job && message.GetInt32("which", 0) == 1)
        queue.Enqueue(*job);
      break;
    }
    case kNameAccepted: {
      auto job =
          DecodeSubmittedJob(message, {Action::Rename, Action::MakeDirectory});
      if (!job)
        break;
      job->destination =
          ChildLocation(job->destination, message.GetString("name", ""));
      if (ParseLocation(job->destination).Archive())
        *job = PrepareArchiveMutation(*job);
      queue.Enqueue(*job);
      break;
    }
    default:
      return false;
    }
  } catch (const std::exception &e) {
    PostMainWindowNotice(e.what());
  }
  return true;
}

} // namespace Listi
