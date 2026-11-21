// SPDX-License-Identifier: MIT
#pragma once

#include "ArchiveActions.h"

class InlineAction;
class OperationQueue;
class Pane;

namespace Listi {

class ClipboardFlow;
enum class Action;

// Owns the pane operation dialogs and submits the jobs frozen by those dialogs.
class FileOperationFlow {
public:
  FileOperationFlow(InlineAction &actionArea, OperationQueue &queue,
                    ClipboardFlow &clipboard, BMessenger owner);

  void SubmitTransfer(Job job);

  // Completion messages submit their frozen job, never the current pane selection.
  bool Handle(const BMessage &message, const Pane &source, const Pane &other,
              bool singlePane);

private:
  bool CanBegin();
  void AskInline(const char *text, const char *button, const BMessage &accepted,
                 const char *initial = nullptr,
                 const char *alternative = nullptr);
  void ShowArchive(uint32 action, const Pane &source, const Pane &target);
  void ConfirmArchiveAddition(const Job &job);
  void AskNamedEntry(Action action, const Pane &source, const Pane &target);
  void ShowBulkRename(const Pane &source);
  void BeginOperation(Action action, const Pane &source, const Pane &target,
                      bool singlePane);

  InlineAction &actionArea;
  OperationQueue &queue;
  ClipboardFlow &clipboard;
  BMessenger owner;
  ArchiveActions archiveActions;
  BMessenger bulkRenameWindow;
};

} // namespace Listi
