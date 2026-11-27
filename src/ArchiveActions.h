// SPDX-License-Identifier: MIT
#pragma once

#include <Message.h>
#include <Messenger.h>
#include <String.h>
#include <memory>
#include <string>

class BFilePanel;
class BMessageRunner;
class Pane;

namespace Listi {
struct Job;
class ReadWorker;
struct ArchiveInspection;

constexpr uint32 kArchiveAddChosen = 'aadc', kArchiveEditAccepted = 'aeda',
                 kArchiveInspectionTick = 'arit';

struct ArchivePrompt {
  BString text;
  const char *button;
  BMessage accepted;
};

class ArchiveActions {
public:
  ArchiveActions();
  ~ArchiveActions();

  void ShowJobWindow(bool extract, const Pane &source, const Pane &destination,
                     BMessenger owner);
  void ShowAddPicker(const Pane &source, BMessenger owner);
  void InspectNamedChange(const Pane &source, BMessenger owner, Job command);
  bool InspectionTick(const BMessage &) const;
  bool PollInspection(const Pane &source, Job &command, Job &target);
  bool Inspecting() const { return inspection != nullptr; }
  void CancelInspection();
  void OpenAddPicker(const Job &target, BMessenger owner);
  static Job ReadAdditions(const BMessage &message);
  static ArchivePrompt ConfirmAddition(const Job &job,
                                       const std::string &lease);
  void InspectRemoval(const Pane &source, BMessenger owner);
  static ArchivePrompt ConfirmRemoval(const Job &target, size_t selectedCount);

private:
  void Inspect(const Pane &, BMessenger owner, Job command, const std::string &archive,
               const std::string &folder);
  std::shared_ptr<ArchiveInspection> inspection;
  ReadWorker *reader = nullptr;
  std::unique_ptr<BMessageRunner> timer;
  int64 requestId = 0;
  std::unique_ptr<BFilePanel> archiveAddPicker;
  BMessenger archiveWindow;
};
}
