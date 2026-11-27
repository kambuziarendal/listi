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
#include "Trash.h"
#include "ReadWorker.h"
#include <cerrno>
#include <sys/stat.h>
#include <exception>
#include <utility>

namespace Listi {
namespace {
constexpr uint32 kNamedEntryAccepted = 'nacc', kConfirm = 'cfir';
constexpr char kPendingQuestion[] = "Finish or cancel the open question first.";
constexpr uint32 kInspectTrash = 'trpr', kTrashReady = 'trpv';

std::string SelectedLabel(const Job &job) {
  if (job.sources.size() == 1)
    return "“" + LocationLeaf(job.sources.front()) + "”";
  return std::to_string(job.sources.size()) + " selected items";
}

std::string PreviewRestore(Job &job) {
  std::string preview = "Restore " + SelectedLabel(job) + "? Existing files will not be overwritten.";
  for (const auto &source : job.sources) {
    auto &target = job.restoreTargets[source];
    preview += "\n" + LocationLeaf(source) + " → ";
    try {
      target = CaptureRestoreTarget(source, job.expectedSources.at(source));
      preview += target.path;
      struct stat st;
      if (lstat(target.path.c_str(), &st) == 0)
        preview += " (already exists)";
      else if (errno != ENOENT)
        preview += " (cannot check destination)";
    } catch (const std::exception &) {
      preview += "Original location unavailable; will stay in Trash.";
    }
  }
  return preview;
}

std::string PreviewTrash(Job &job) {
  std::vector<std::string> unsupported;
  decltype(job.expectedSources) unsupportedIdentities;
  for (const auto &source : job.sources) {
    if (IsVirtual(source))
      throw std::runtime_error("Trash is available only on local supported volumes. Nothing was deleted.");
    try {
      CheckTrashSource(source, &job.expectedSources.at(source), false);
    } catch (const TrashUnsupported &) {
      unsupported.push_back(source);
      unsupportedIdentities.emplace(source, job.expectedSources.at(source));
    }
  }
  if (unsupported.empty())
    return "Move " + SelectedLabel(job) + " to Trash?\nYou can restore from Trash.";

  const bool mixed = unsupported.size() != job.sources.size();
  job.action = Action::Delete;
  job.sources = std::move(unsupported);
  job.expectedSources = std::move(unsupportedIdentities);
  std::string preview = "No Trash on ext4 volumes. Permanently delete " +
                        SelectedLabel(job) + "?\nThis cannot be undone.";
  if (mixed)
    preview += " Other selected items will stay where they are.";
  return preview;
}

class TrashProbe : public ReadWorker {
  BMessenger owner;
  Job job;
  int64 request;
public:
  TrashProbe(BMessenger owner, Job job, int64 request)
      : ReadWorker("Trash confirmation reader"), owner(owner), job(std::move(job)), request(request) {}
  void MessageReceived(BMessage *message) override {
    if (message->what != kInspectTrash) {
      ReadWorker::MessageReceived(message);
      return;
    }
    BMessage reply(kTrashReady);
    try {
      const auto preview = job.action == Action::Restore ? PreviewRestore(job) : PreviewTrash(job);
      // Confirm the frozen job, never the current pane or selection.
      reply = EncodeJob(job, kTrashReady);
      RequireJobField(reply.AddString("preview", preview.c_str()));
    } catch (const std::exception &error) {
      reply.MakeEmpty(); // Never deliver a valid job without its confirmation text.
      reply.what = kTrashReady;
      reply.AddString("error", error.what());
    }
    reply.AddInt64("request", request);
    owner.SendMessage(&reply, static_cast<BHandler *>(nullptr), 50000);
  }
};
} // namespace

FileOperationFlow::~FileOperationFlow() { RetireReadWorker(trashProbe); }

FileOperationFlow::FileOperationFlow(InlineAction &actionArea,
                                     OperationQueue &queue,
                                     ClipboardFlow &clipboard, BMessenger owner)
    : actionArea(actionArea), queue(queue), clipboard(clipboard), owner(owner) {
}

bool FileOperationFlow::CanBegin() {
  if (!actionArea.Pending()) {
    RetireReadWorker(trashProbe);
    return true;
  }
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
  if (action == kAddArchive) {
    archiveActions.ShowAddPicker(source, owner);
    ShowInspectionProgress();
  } else if (action == kRemoveArchive) {
    archiveActions.InspectRemoval(source, owner);
    ShowInspectionProgress();
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
  Job destination;
  if (action == Action::Link)
    target.BindTransferDestination(destination);
  auto job = PrepareNamedEntry(action, source.Selected(), destination.destination,
                               source.SelectedLocalIdentities(), destination.expectedDestination);
  const bool duplicate = action == Action::Duplicate;
  std::string text = std::string(duplicate ? "Duplicate: " : "Link to: ") +
                     job.sources[0] + "\nIn: " + job.destination;
  if (!duplicate)
    text += "\nRelative: path from this folder. Absolute: full target path.";
  auto accepted = EncodeJob(job, kNamedEntryAccepted);
  RequireJobField(accepted.AddString("newEntryIn", job.destination.c_str()));
  AskInline(text.c_str(), duplicate ? "Duplicate" : "Relative link", accepted,
            job.entryName.c_str(), duplicate ? nullptr : "Absolute link");
}

void FileOperationFlow::AskEntryName(const Job &job, const Job *inspectedArchive) {
  const bool rename = job.action == Action::Rename, file = job.action == Action::MakeFile;
  auto accepted = EncodeJob(job, kNameAccepted);
  if (inspectedArchive) {
    auto target = EncodeJob(*inspectedArchive, 0);
    RequireJobField(accepted.AddMessage("inspectedArchive", &target));
  }
  if (file)
    RequireJobField(accepted.AddString("newEntryIn", job.destination.c_str()));
  const auto name = rename ? LocationLeaf(job.sources.front()) : file ? "New file" : "New folder";
  const auto question = rename ? "Rename: " + job.sources.front() : "Create in: " + job.destination;
  AskInline(question.c_str(), rename ? "Rename" : file ? "Create file" : "Create folder",
            accepted, name.c_str());
}

void FileOperationFlow::ShowInspectionProgress() {
  if (!actionArea.Ask("Checking archive format…", nullptr, BMessage())) {
    archiveActions.CancelInspection();
    throw std::runtime_error(kPendingQuestion);
  }
  inspectionQuestion = actionArea.Ticket();
}

void FileOperationFlow::FinishInspection(const Pane &source) {
  if (!actionArea.Pending() || actionArea.Ticket() != inspectionQuestion) {
    archiveActions.CancelInspection();
    return;
  }
  Job command, target;
  bool ready;
  try {
    ready = archiveActions.PollInspection(source, command, target);
  } catch (...) {
    actionArea.Close();
    throw;
  }
  if (archiveActions.Inspecting())
    return;
  actionArea.Close();
  if (!ready)
    return;
  if (command.action == Action::EditArchive)
    archiveActions.OpenAddPicker(target, owner);
  else if (command.action == Action::Delete) {
    auto prompt = ArchiveActions::ConfirmRemoval(target, command.sources.size());
    AskInline(prompt.text.String(), prompt.button, prompt.accepted);
  } else
    AskEntryName(command, &target);
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
  const bool create = action == Action::MakeDirectory || action == Action::MakeFile;
  if (!create) {
    job.sources = source.Selected();
    source.ExportModel().BindArchiveView(job, source.Path());
    job.expectedSources = source.SelectedLocalIdentities();
    if (!job.sources.empty() && IsVirtual(job.sources.front()) &&
        !ParseLocation(job.sources.front()).Archive())
      job.expectedRemoteSources = source.ExportModel().SelectedRemoteEntries();
  }
  if (isTransfer)
    target.BindTransferDestination(job);
  if ((action == Action::Trash || action == Action::Delete) &&
      !job.sources.empty() && ParseLocation(job.sources.front()).Archive()) {
    ShowArchive(kRemoveArchive, source, target);
    return;
  }
  if (!create && job.sources.empty()) {
    PostMainWindowNotice("Select one or more files in the active panel first.");
    return;
  }
  const bool rename = action == Action::Rename;
  if (rename || create) {
    if (rename && job.sources.size() > 1) {
      ShowBulkRename(source);
      return;
    }
    job.destination = source.Path();
    if (job.destination == "@disks" ||
        (action == Action::MakeFile && IsVirtual(job.destination))) {
      PostMainWindowNotice("Open a folder first.");
      return;
    }
    if ((create && !IsVirtual(job.destination)) ||
        (IsVirtual(job.destination) && !ParseLocation(job.destination).Archive()))
      source.BindTransferDestination(job);
    FreezeLocalJob(job, job.destination);
    if (ParseLocation(job.destination).Archive()) {
      archiveActions.InspectNamedChange(source, owner, std::move(job));
      ShowInspectionProgress();
    } else {
      FreezeArchiveTargets(job);
      AskEntryName(job);
    }
    return;
  }
  if (isTransfer) {
    SubmitTransfer(std::move(job));
    return;
  }
  FreezeLocalJob(job);
  FreezeArchiveTargets(job);
  if (action == Action::Restore || action == Action::Trash) {
    trashProbe = new TrashProbe(owner, std::move(job), ++trashRequest);
    if (!StartReadWorker(trashProbe) || trashProbe->PostMessage(kInspectTrash) != B_OK) {
      RetireReadWorker(trashProbe);
      throw std::runtime_error("Cannot check Trash. Try again.");
    }
    return;
  }
  auto accepted = EncodeJob(job, kConfirm);
  const auto text = "Permanently delete " + SelectedLabel(job) + "?\nThis cannot be undone.";
  AskInline(text.c_str(), "Delete permanently", accepted);
}

bool FileOperationFlow::Handle(const BMessage &message, const Pane &source,
                               const Pane &other, bool singlePane) {
  const Pane &target = singlePane ? source : other;
  try {
    switch (message.what) {
    case kArchiveInspectionTick:
      if (archiveActions.InspectionTick(message))
        FinishInspection(source);
      break;
    case kInlineCancel:
      if (!archiveActions.Inspecting() || actionArea.Ticket() != inspectionQuestion)
        return false;
      archiveActions.CancelInspection();
      actionArea.Close();
      owner.SendMessage(kInlineClosed);
      break;
    case kInlineClosed:
      if (archiveActions.Inspecting() && actionArea.Ticket() != inspectionQuestion)
        archiveActions.CancelInspection();
      return false;
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
      if (lease.empty())
        queue.Enqueue(*job);
      else
        clipboard.SubmitArchivePaste(*job, lease);
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
    case kNewFile:
      BeginOperation(Action::MakeFile, source, target, singlePane);
      break;
    case kMkdir:
      BeginOperation(Action::MakeDirectory, source, target, singlePane);
      break;
    case kTrashReady: {
      if (!trashProbe || message.IsSourceRemote() || message.GetInt64("request", -1) != trashRequest)
        break;
      RetireReadWorker(trashProbe);
      if (const char *error = message.GetString("error", nullptr)) {
        PostMainWindowNotice(error);
        break;
      }
      auto job = DecodeSubmittedJob(message, {Action::Trash, Action::Restore, Action::Delete});
      if (!job)
        break;
      auto accepted = message;
      accepted.what = kConfirm;
      const char *button = job->action == Action::Restore ? "Restore"
                           : job->action == Action::Trash ? "Move to Trash"
                                                          : "Delete permanently";
      AskInline(message.GetString("preview", ""), button, accepted);
      break;
    }
    case kConfirm: {
      auto job = DecodeSubmittedJob(
          message, {Action::Trash, Action::Restore, Action::Delete});
      if (job && message.GetInt32("which", 0) == 1)
        queue.Enqueue(*job);
      break;
    }
    case kNameAccepted: {
      auto job =
          DecodeSubmittedJob(message, {Action::Rename, Action::MakeDirectory, Action::MakeFile});
      if (!job)
        break;
      job->destination =
          ChildLocation(job->destination, message.GetString("name", ""));
      if (ParseLocation(job->destination).Archive()) {
        BMessage target;
        RequireJobField(message.FindMessage("inspectedArchive", &target));
        *job = PrepareArchiveMemberChange(*job, DecodeJob(target));
      }
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
