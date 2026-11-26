// SPDX-License-Identifier: MIT
#pragma once

#include "ClipboardTextFile.h"
#include <set>

class InlineAction;
class OperationQueue;
class Pane;

namespace Listi {

constexpr uint32 kClipboardFileAccepted = 'ctac';

// Owns frozen clipboard content and tracks the jobs that share a paste lease.
class ClipboardFlow {
public:
  ClipboardFlow(InlineAction &, OperationQueue &);

  void Begin(uint32 action, Pane &pane);
  std::optional<std::string> SaveFile(const std::string &name);
  void CancelPrompt();
  bool ArchivePromptPending() const { return archiveClipboardPending; }
  bool Busy() const { return !clipboardJobs.empty(); }
  void SubmitArchivePaste(Job &job, const std::string &lease);
  void JobFinished(const BMessage &done);

private:
  void AskToSaveFile(const char *folder, ClipboardContent content);
  void EnqueuePaste(Job &job);
  void FinishPaste(const std::vector<std::string> &moved);

  InlineAction &actionArea;
  OperationQueue &queue;
  std::optional<ClipboardFile> pendingClipboardFile;
  ClipboardPaste clipboardPaste;
  bool archiveClipboardPending = false;
  int64 clipboardSerial = 0;
  std::set<int64> clipboardJobs;
  std::vector<std::string> clipboardMoved;
};

} // namespace Listi
