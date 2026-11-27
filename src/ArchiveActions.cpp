// SPDX-License-Identifier: MIT
#include "ArchiveActions.h"

#include "ArchiveWindow.h"
#include "ArchiveJobChecks.h"
#include "JobSnapshot.h"
#include "Pane.h"
#include "VirtualFile.h"
#include "ReadWorker.h"
#include <MessageRunner.h>
#include <Entry.h>
#include <FilePanel.h>
#include <Path.h>
#include <algorithm>
#include <stdexcept>
#include <sys/stat.h>

namespace Listi {

// Only the reader writes target/error, published by done. It owns no views;
// retiring a blocked device read only revokes its ticket.
struct ArchiveInspection {
  Job command, target;
  BMessenger pane;
  int64 revision;
  std::string archive, folder, error;
  std::atomic<bool> cancelled{false}, done{false};
};
namespace {
constexpr uint32 kReadArchiveFormat = 'arif';
class FormatReader : public ReadWorker {
  std::shared_ptr<ArchiveInspection> task;
public:
  explicit FormatReader(std::shared_ptr<ArchiveInspection> task)
      : ReadWorker("archive format reader"), task(std::move(task)) {}
  void MessageReceived(BMessage *message) override {
    if (message->what != kReadArchiveFormat) {
      ReadWorker::MessageReceived(message);
      return;
    }
    try {
      Hooks hooks;
      hooks.proceed = [this] { return !task->cancelled.load(); };
      task->target = PrepareArchiveEditTarget(task->archive, task->folder, hooks);
      ArchiveJobChecks::RequireArchiveVersions(task->command, task->target);
      ArchiveJobChecks::Guards(task->command);
      if (task->command.action == Action::Delete)
        SetArchiveEditRemovals(task->target, task->command.sources);
    } catch (const std::exception &e) {
      task->error = e.what();
    }
    task->done = true;
  }
};

void RequireScannedSources(const Pane &pane, const Job &job) {
  Job view;
  pane.ExportModel().BindArchiveView(view, pane.Path());
  ArchiveJobChecks::RequireArchiveVersions(view, job);
  // Split archives can have additional guards for parts not selected in the
  // panel. Match each selected local row, not the size of the full part set.
  for (const auto &[path, displayed] : pane.SelectedLocalIdentities()) {
    const auto found = job.expectedSources.find(path);
    if (found == job.expectedSources.end() ||
        found->second.device != displayed.device ||
        found->second.inode != displayed.inode)
      throw std::runtime_error("The selected entry was replaced: " + path);
  }
}
} // namespace

ArchiveActions::ArchiveActions() = default;
ArchiveActions::~ArchiveActions() { CancelInspection(); }

void ArchiveActions::CancelInspection() {
  if (inspection) inspection->cancelled = true;
  inspection.reset();
  timer.reset();
  RetireReadWorker(reader);
}

void ArchiveActions::Inspect(const Pane &source, BMessenger owner, Job command,
                             const std::string &archive, const std::string &folder) {
  CancelInspection();
  if (source.SelectionRevision() < 0)
    throw std::runtime_error("Wait for the folder to load and try again.");
  source.ExportModel().BindArchiveView(command, source.Path());
  ArchiveJobChecks::VerifyArchiveSets(command);
  auto task = std::make_shared<ArchiveInspection>();
  task->pane = BMessenger(&source);
  task->revision = source.SelectionRevision();
  task->command = std::move(command);
  task->archive = archive;
  task->folder = folder;
  BMessage tick(kArchiveInspectionTick);
  RequireJobField(tick.AddInt64("request", ++requestId));
  inspection = task;
  timer = std::make_unique<BMessageRunner>(owner, &tick, 75000);
  reader = new FormatReader(task);
  if (!StartReadWorker(reader) || timer->InitCheck() != B_OK ||
      reader->PostMessage(kReadArchiveFormat) != B_OK) {
    CancelInspection();
    throw std::runtime_error("Could not inspect the archive. Try again.");
  }
}

bool ArchiveActions::InspectionTick(const BMessage &message) const {
  return inspection && message.GetInt64("request", -1) == requestId;
}

bool ArchiveActions::PollInspection(const Pane &source, Job &command, Job &target) {
  auto task = inspection;
  if (source.IsHidden() || task->pane != BMessenger(&source) ||
      source.SelectionRevision() != task->revision) {
    CancelInspection();
    return false;
  }
  if (!task->done)
    return false;
  CancelInspection();
  if (!task->command.sources.empty() && source.Selected() != task->command.sources)
    return false;
  if (!task->error.empty())
    throw std::runtime_error(task->error);
  ArchiveJobChecks::VerifyArchiveSets(task->target);
  ArchiveJobChecks::Guards(task->target);
  command = std::move(task->command);
  target = std::move(task->target);
  return true;
}

void ArchiveActions::InspectNamedChange(const Pane &source, BMessenger owner, Job command) {
  const auto location = ParseLocation(command.destination);
  Inspect(source, owner, std::move(command), location.archive, "/");
}

void ArchiveActions::ShowJobWindow(bool extract, const Pane &source,
                                   const Pane &destination, BMessenger owner) {
  if (archiveWindow.IsValid()) {
    archiveWindow.SendMessage('arav');
    return;
  }
  auto job = PrepareArchiveJob(extract, source.Selected());
  RequireScannedSources(source, job);
  std::string folder = destination.Path();
  if (folder == "@disks" || IsVirtual(folder)) {
    auto loc = ParseLocation(job.sources[0]);
    folder = ParentLocation(loc.Archive() ? loc.archive : job.sources[0]);
  }
  archiveWindow = OpenArchiveWindow(job, folder, owner);
}

void ArchiveActions::ShowAddPicker(const Pane &source, BMessenger owner) {
  Job command;
  command.action = Action::EditArchive;
  std::string archiveAddTarget, archiveAddFolder;
  const auto folder = source.Path();
  auto location = ParseLocation(folder);
  if (location.Archive()) {
    archiveAddTarget = location.archive;
    archiveAddFolder = location.path;
  } else {
    const auto selected = source.Selected();
    if (selected.size() != 1 || IsVirtual(selected[0]))
      throw std::runtime_error(
          "Select one local writable archive, or open a folder inside it.");
    command.sources = selected;
    command.expectedSources = source.SelectedLocalIdentities();
    FreezeLocalJob(command);
    archiveAddTarget = selected[0];
    archiveAddFolder = "/";
  }
  struct stat st;
  if (lstat(archiveAddTarget.c_str(), &st) || !S_ISREG(st.st_mode))
    throw std::runtime_error("Choose one local writable archive.");
  Inspect(source, owner, std::move(command), archiveAddTarget, archiveAddFolder);
}

void ArchiveActions::OpenAddPicker(const Job &target, BMessenger owner) {
  auto chosen = EncodeJob(target, kArchiveAddChosen);
  archiveAddPicker.reset(new BFilePanel(
      B_OPEN_PANEL, &owner, nullptr,
      node_flavor(B_FILE_NODE | B_DIRECTORY_NODE), true, &chosen));
  archiveAddPicker->SetPanelDirectory(target.destinationFolder.c_str());
  archiveAddPicker->Show();
}

Job ArchiveActions::ReadAdditions(const BMessage &message) {
  type_code type;
  int32 count;
  if (message.GetInfo("refs", &type, &count) != B_OK || type != B_REF_TYPE || count < 1)
    throw std::runtime_error("The file selection is incomplete. Select the files again.");
  std::vector<std::string> additions;
  for (int32 i = 0; i < count; ++i) {
    entry_ref ref;
    BPath path;
    if (message.FindRef("refs", i, &ref) != B_OK || path.SetTo(&ref) != B_OK)
      throw std::runtime_error("A selected file is unavailable. Select the files again.");
    additions.emplace_back(path.Path());
  }
  auto job = DecodeJob(message);
  SetArchiveEditAdditions(job, additions);
  return job;
}

ArchivePrompt ArchiveActions::ConfirmAddition(const Job &job,
                                             const std::string &lease) {
  auto accepted = EncodeJob(job, kArchiveEditAccepted);
  if (!lease.empty())
    RequireJobField(accepted.AddString("clipboardLease", lease.c_str()));
  BString text;
  if (job.action == Action::MoveArchive) {
    text << "Move " << int32(job.sources.size()) << " selected entries to:\n"
         << job.destination.c_str();
    const bool sameArchive =
        !job.sources.empty() &&
        ParseLocation(job.sources.front()).archive ==
            ParseLocation(job.destination).archive;
    text
        << (sameArchive
                ? "\nEntries move within this archive. Existing names are never replaced."
                : "\nSource entries are removed only after the saved destination is verified."
                  " If interrupted after copying, both copies may remain. Existing names require a choice.");
    if (!job.sources.empty() && ParseLocation(job.sources.front()).Local())
      text
          << "\nFiles with links or filesystem attributes must be copied instead, so the original metadata is retained.";
    return {text, "Move entries", accepted};
  }
  if (!job.archiveRenames.empty())
    text << "Move selected entries inside this archive:\n";
  else
    text << "Add " << int32(job.sources.size()) << " selected entries to:\n";
  text << job.destination.c_str();
  if (!job.archivePath.empty())
    text << "\nInside: /" << job.archivePath.c_str();
  const auto shown = std::min<size_t>(job.sources.size(), 3);
  for (size_t i = 0; i < shown; ++i)
    text << "\n" << LocationLeaf(job.sources[i]).c_str();
  if (job.sources.size() > shown)
    text << "\n…and " << int32(job.sources.size() - shown) << " more";
  text
      << (job.archiveRenames.empty()
              ? "\nOriginal files are kept. Existing names require a Replace or Skip choice."
              : "\nEntries move within this archive. Existing names are never replaced.");
  return {text, "Update archive", accepted};
}

void ArchiveActions::InspectRemoval(const Pane &source, BMessenger owner) {
  const auto selected = source.Selected();
  if (selected.empty())
    throw std::runtime_error("Select entries inside one writable archive.");
  auto location = ParseLocation(selected[0]);
  if (!location.Archive())
    throw std::runtime_error(
        "Open an archive and select the entries to remove.");
  Job command;
  command.action = Action::Delete;
  command.sources = selected;
  Inspect(source, owner, std::move(command), location.archive, location.path);
}

ArchivePrompt ArchiveActions::ConfirmRemoval(const Job &job, size_t selectedCount) {
  const auto accepted = EncodeJob(job, kArchiveEditAccepted);
  BString text;
  text
      << "Remove " << int32(selectedCount)
      << " selected entries from this archive?\n\n"
      << job.destination.c_str()
      << "\n\nThe archive is rebuilt safely. The original remains unchanged if the job fails.";
  return {text, "Remove from archive", accepted};
}
}
