// SPDX-License-Identifier: MIT
#include "ArchiveActions.h"

#include "ArchiveWindow.h"
#include "JobSnapshot.h"
#include "Pane.h"
#include "VirtualFile.h"
#include <Entry.h>
#include <FilePanel.h>
#include <Path.h>
#include <algorithm>
#include <stdexcept>
#include <sys/stat.h>

namespace Listi {

namespace {
void RequireScannedSources(const Pane &pane, const Job &job) {
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
ArchiveActions::~ArchiveActions() = default;

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
    archiveAddTarget = selected[0];
    archiveAddFolder = "/";
  }
  struct stat st;
  if (lstat(archiveAddTarget.c_str(), &st) || !S_ISREG(st.st_mode))
    throw std::runtime_error("Choose one local writable archive.");
  auto target = PrepareArchiveEditTarget(archiveAddTarget, archiveAddFolder);
  if (!location.Archive())
    RequireScannedSources(source, target);
  auto chosen = EncodeJob(target, kArchiveAddChosen);
  archiveAddPicker.reset(new BFilePanel(
      B_OPEN_PANEL, &owner, nullptr,
      node_flavor(B_FILE_NODE | B_DIRECTORY_NODE), true, &chosen));
  archiveAddPicker->SetPanelDirectory(ParentLocation(archiveAddTarget).c_str());
  archiveAddPicker->Show();
}

Job ArchiveActions::ReadAdditions(const BMessage &message) {
  std::vector<std::string> additions;
  entry_ref ref;
  BPath path;
  for (int32 i = 0; message.FindRef("refs", i, &ref) == B_OK; ++i) {
    if (path.SetTo(&ref) != B_OK)
      throw std::runtime_error("A selected file is unavailable.");
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

ArchivePrompt ArchiveActions::ConfirmRemoval(const Pane &source) {
  const auto selected = source.Selected();
  if (selected.empty())
    throw std::runtime_error("Select entries inside one writable archive.");
  auto location = ParseLocation(selected[0]);
  if (!location.Archive())
    throw std::runtime_error(
        "Open an archive and select the entries to remove.");
  auto job = PrepareArchiveEditJob(location.archive, {}, selected, location.path);
  const auto accepted = EncodeJob(job, kArchiveEditAccepted);
  BString text;
  text
      << "Remove " << int32(selected.size())
      << " selected entries from this archive?\n\n"
      << location.archive.c_str()
      << "\n\nThe archive is rebuilt safely. The original remains unchanged if the job fails.";
  return {text, "Remove from archive", accepted};
}
}
