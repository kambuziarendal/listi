// SPDX-License-Identifier: MIT
#pragma once
#include "FileOperation.h"
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>
namespace Listi {
struct ArchiveEditRecord {
  std::string directory, member, error;
};
std::vector<ArchiveEditRecord> ListArchiveEdits(const std::string &root);
// Persistent private working copies. Passwords are never written to the
// journal.
class ArchiveEditSession {
public:
  enum Observation { Waiting, Unchanged, Changed };
  static std::shared_ptr<ArchiveEditSession>
  Open(const std::string &member, const std::string &root, const Hooks & = {},
       const Job &shown = {});
  static std::shared_ptr<ArchiveEditSession>
  Resume(const std::string &directory);
  Observation Observe(const Hooks & = {});
  std::shared_ptr<ArchiveEditSession>
  StartFresh(const Hooks & = {}); // Explicit user choice; old copy is kept.
  bool Save(const Hooks & = {});  // false means the working copy is unchanged
  const std::string &Member() const { return member; }
  const std::string &ArchiveVersion() const { return target.expectedArchiveSets.at(target.destination); }
  const std::string &WorkingFile() const { return working; }
  const std::string &Directory() const { return directory; }

private:
  std::string directory, member, working, predecessor;
  static std::shared_ptr<ArchiveEditSession>
  Create(const std::string &member, const std::string &root,
         const std::string &predecessor, const Hooks &hooks, const Job &shown = {});
  Job target;
  unsigned permissions = 0644;
  struct stat directoryIdentity {
  }, workIdentity{}, observed{};
  bool haveObserved = false, checked = false, journalPending = false;
  int stable = 0;
  void ValidateDirectory() const;
  void ValidateArchive() const;
  void RequireActive() const;
  void Persist();
  void FinishSaveJournal();
  bool ChangedBytes(const Hooks &);
};
} // namespace Listi
// These functions are called only from the application's UI thread.
void OpenArchiveEditor(const std::string &member, const std::string &editor,
                       const Listi::Job &shown);
void ResumeArchiveEditors(const std::string &editor);
bool ArchiveEditorsAllowQuit();
