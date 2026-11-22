// SPDX-License-Identifier: MIT
#pragma once
#include <memory>
class BMessage;
#include <string>
#include <sys/stat.h>

namespace Listi {
enum class RecoveryPhase { Intent, Writing, Ready, Publishing, Published };
class RecoveryRecord {
public:
  RecoveryRecord(const std::string &kind, const std::string &path,
                 const struct stat &identity, const std::string &inventory = {});
  // Reserve the record before creating the object. The unique record ID is
  // also used in the stage name; stage creation must still be exclusive.
  RecoveryRecord(const std::string &kind, const std::string &folder,
                 const std::string &prefix, const std::string &source,
                 const std::string &destination, const std::string &inventory = {});
  ~RecoveryRecord();
  RecoveryRecord(const RecoveryRecord &) = delete;
  const std::string &Stage() const { return stage; }
  const std::string &Backup() const { return backup; }
  void Advance(RecoveryPhase next, const struct stat *bound = nullptr);
  void PrepareCopyPublication();
  void NoteFailure(bool cancelled) noexcept;
  void Retire();
private:
  void SavePhase(RecoveryPhase next);
  void Reserve(const std::string &directory);
  void Save();
  std::unique_ptr<BMessage> message;
  std::string path, stage, backup;
  struct stat identity{}, inventoryIdentity{};
  RecoveryPhase phase = RecoveryPhase::Intent;
};
std::string ReviewRecovery(const std::string &inventory = {});
} // namespace Listi
