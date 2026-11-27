// SPDX-License-Identifier: MIT
#include "Pane.h"
#include "UnavailableVolume.h"
#include <Window.h>
#include <Entry.h>
#include <Roster.h>
#include "ArchiveEditor.h"
#include "ArchiveJobChecks.h"
#include "ImagePreview.h"
#include "InlineAction.h"
#include "MainWindow.h"
#include "MainWindowMenu.h"
#include "OpenWith.h"
#include "OperationQueue.h"
#include "PropertiesWindow.h"
#include "VirtualFile.h"

namespace {
constexpr uint32 kPreviewPattern = 'sppv';
}

void Pane::BindTransferDestination(Listi::Job &job) const {
  job.destination = Path();
  if (!job.destination.empty() && job.destination.front() == '/') {
    const auto [device, node] = FolderIdentity();
    if (!LocationValid() || device < 0 || node < 0 || IsSearchList())
      throw std::runtime_error("Wait for the destination folder to load, or reload it.");
    job.expectedDestination = {int32_t(device), node};
  } else {
    entries.BindArchiveView(job, job.destination);
    entries.BindRemoteView(job, job.destination);
  }
}

bool Pane::HandleSelectionCommand(const BMessage &request, InlineAction &prompt,
                                  OperationQueue &queue) {
  switch (request.what) {
  case kSelectPattern:
  case kPreviewPattern:
  case Listi::kApplyPattern:
    SelectByPattern(request, prompt, queue);
    return true;
  case kSelectType:
    if (!scanning && entries.SelectSameFileType())
      SendSelection();
    else
      PostMainWindowNotice(
          "Choose a file with a filename extension. Only visible files of that "
          "type are selected; folders and links are excluded.");
    return true;
  case kAll:
  case kNone:
  case kInvert: {
    const int mode =
        request.what == kInvert ? 2 : (request.what == kAll ? 1 : 0);
    Select(mode);
    return true;
  }
  default:
    return false;
  }
}

void Pane::Select(int mode) {
  entries.Select(mode);
  SendSelection();
}

size_t Pane::MatchPattern(const char *pattern, bool apply) {
  const size_t count = entries.MatchPattern(pattern, apply);
  if (apply && count > 0)
    SendSelection();
  return count;
}

void Pane::SelectByPattern(const BMessage &request, InlineAction &prompt,
                           OperationQueue &queue) {
  const int64 revision = SelectionRevision();
  const char *busy = "Finish or cancel the open question first.";
  if (request.what == kSelectPattern) {
    if (revision < 0) {
      SetStatus("Load a folder before selecting by pattern.");
      return;
    }
    BMessage preview(kPreviewPattern);
    preview.AddInt32("pane", id);
    preview.AddInt64("revision", revision);
    if (!prompt.Ask(
            "Match visible names, including folders. Use * or ? (case-sensitive).",
            "Preview", preview, "*"))
      queue.ErrorInfo(busy);
    return;
  }
  if (revision < 0 || request.GetInt32("pane", -1) != id ||
      request.GetInt64("revision", -1) != revision) {
    SetStatus("Panel contents changed. Start Select by pattern again.");
    return;
  }
  const char *pattern = request.GetString("name", "");
  const bool apply = request.what == Listi::kApplyPattern;
  const size_t count = MatchPattern(pattern, apply);
  if (count == 0) {
    SetStatus("0 matches. Selection unchanged.");
  } else if (!apply) {
    BMessage accepted(request);
    accepted.what = Listi::kApplyPattern;
    auto question =
        std::to_string(count) +
        (count == 1 ? " visible entry matches " : " visible entries match ") +
        pattern + ". Replace the selection?";
    if (!prompt.Ask(question.c_str(), "Select", accepted))
      queue.ErrorInfo(busy);
  }
}

bool Pane::HandleOpenCommand(
    uint32 action, const BString &editor, const BMessenger &service,
    const std::function<void(const std::string &)> &openPreview) {
  const char *missingSelection;
  switch (action) {
  case kFileTypeIcon:
    missingSelection = "Select one local file or folder.";
    break;
  case kOpenWith:
    missingSelection = "Select one local file for Open with.";
    break;
  case kProperties:
    missingSelection = "Select one local file or folder for Properties.";
    break;
  case kPreviewOtherPanel:
  case kView:
  case kEdit:
  case kEditExternally:
  case kOpenSystem:
    missingSelection = "Select exactly one file.";
    break;
  default:
    return false;
  }

  const auto path = SelectedPath();
  if (Listi::InterruptedVolumes().Contains(path)) {
    ShowStatus(Listi::kUnavailableVolumeText);
    return true;
  }
  if (path.empty()) {
    PostMainWindowNotice(missingSelection);
  } else if (action == kFileTypeIcon) {
    BEntry entry(path.c_str(), false);
    entry_ref ref;
    BMessage refs(B_REFS_RECEIVED);
    if (path.front() != '/' || Listi::IsVirtual(path) || !entry.Exists() ||
        entry.IsSymLink() || entry.GetRef(&ref) != B_OK) {
      PostMainWindowNotice("Choose a local file or folder, not a symbolic link.");
    } else {
      status_t status = refs.AddRef("refs", &ref);
      if (status == B_OK)
        status = be_roster->Launch("application/x-vnd.haiku-filetypes", &refs);
      if (status != B_OK && status != B_ALREADY_RUNNING)
        PostMainWindowNotice("Could not open Haiku FileTypes for this item.");
    }
  } else if (action == kOpenWith) {
    OpenWithFile(path);
  } else if (action == kProperties) {
    OpenFileProperties(path, BMessenger(this));
  } else if (action == kPreviewOtherPanel) {
    openPreview({});
  } else if (action == kEdit && !Listi::IsVirtual(path)) {
    openPreview(path);
  } else if (action == kView) {
    ImagePreview::OpenWindow(path);
  } else {
    const bool externalEdit = action != kOpenSystem;
    if (externalEdit && Listi::ParseLocation(path).Archive()) {
      try {
        Listi::Job shown;
        entries.BindArchiveView(shown, path);
        Listi::ArchiveJobChecks::VerifyArchiveSets(shown);
        OpenArchiveEditor(path, editor.String(), shown);
      } catch (const std::exception &error) {
        PostMainWindowNotice(error.what());
      }
    } else {
      BMessage open(kOpenPaths);
      open.AddMessenger("origin", BMessenger(Window()));
      open.AddString("path", path.c_str());
      open.AddString("mode", externalEdit ? "edit" : "system");
      if (externalEdit)
        open.AddString("application", editor);
      service.SendMessage(&open);
    }
  }
  return true;
}
