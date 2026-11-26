// SPDX-License-Identifier: MIT
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <stdint.h>
#include <string>
#include <vector>

namespace Listi {
enum class Action {
  Copy,
  Move,
  Delete,
  Trash,
  Restore,
  MakeDirectory,
  Rename,
  Duplicate,
  Link,
  CreateArchive,
  ExtractArchive,
  EditArchive,
  MoveArchive,
  BulkRename,
  ConvertImage,
  UndoBulkRename,
  MakeFile
};
enum class Decision { Replace, ReplaceAll, Skip, SkipAll, Abort, KeepBoth };
struct CopyConflict {
  std::string source, destination, proposed;
  int64_t sourceSize, destinationSize, sourceModified, destinationModified;
};
struct NodeIdentity {
  int32_t device = -1;
  int64_t inode = -1;
};
struct BulkRenameGuard {
  std::string contents, rootStamp;
};
struct Job {
  Action action = Action::Copy;
  std::vector<std::string> sources;
  std::string destination;
  // Optional identity guards for operations frozen from the system clipboard or a drop.
  std::map<std::string, NodeIdentity> expectedSources;
  // Archive set fingerprints captured before destination/password questions.
  std::map<std::string, std::string> expectedArchiveSets;
  NodeIdentity expectedDestination;
  int64_t tag = 0;
  // Only Duplicate/Link use an explicit leaf name, always without replacement.
  std::string entryName;
  bool relativeLink = true;
  // Selected Translation Kit output for ConvertImage.
  uint32_t imageFormat = 0;
  std::string imageMime;
  std::string archiveFormat = "zip";
  int archiveLevel = 6;
  // A process-local handle, never the password itself. Jobs cannot survive an
  // application restart with this handle, which is intentional.
  uint64_t archivePasswordToken = 0;
  bool archiveEncryptNames = false;
  // EditArchive uses destination as the local archive, sources as local entries
  // to add, archivePath as the internal destination folder, and archiveRemove
  // as normalized member paths without a leading slash.
  std::string archivePath;
  std::vector<std::string> archiveRemove;
  std::vector<std::string> archiveFolders;
  std::map<std::string, std::string> archiveRenames;
  // BulkRename maps each frozen source to its previewed leaf (also unchanged).
  std::map<std::string, std::string> bulkNames;
  // Undo guards are worker-owned; never supplied by a submitted BMessage.
  std::map<std::string, BulkRenameGuard> bulkFingerprints;
  std::string
      destinationFolder; // Captured parent for rename/new-folder questions.
  // Internal, never decoded from UI messages: a staged copy for an archive
  // move must not follow a destination merge into another mounted volume.
  bool copyForArchiveMove = false;
  // In-process guard for cross-window Copy, never decoded from messages.
  std::function<void()> checkDestinationWindow;
  // For confirmed Trash restores, bind the previewed original paths as well as the entries.
  std::map<std::string, std::string> restoreTargets;
};
class JobReportStore;
struct Hooks {
  std::function<bool()> proceed;
  std::function<void(const std::string &, uint64_t)> progress;
  std::function<Decision(const std::string &)> conflict;
  std::function<void(const std::string &, const std::string &)> fileDone;
  std::function<void(const std::string &, const std::string &)> fileFailed;
  // Observed during the real operation. The UI must not probe a remote server
  // solely for a progress estimate.
  std::function<void(const std::string &, uint64_t)> fileSize;
  bool privateStaging = false;
  // Optional disk-backed detail stream owned by the operation queue.
  std::shared_ptr<JobReportStore> report;
  // Only for regular-file collisions in a user-facing local copy.
  std::function<Decision(const CopyConflict &)> copyConflict;
  // Offered only by ordinary Copy, never Move or private staging.
  std::function<bool(const std::string &, const std::string &)> confirmFatCopy;
};
enum class Publication { NotAttempted, Unknown, Published };
struct UploadVerification {
  // Accepted upload length, not a count of unacknowledged partial writes.
  std::optional<uint64_t> bytesWritten;
  bool destinationSizeChecked = false;
  bool contentVerified = false;
  Publication publication = Publication::NotAttempted;
  bool sourceRetained = true; // Network uploads are copies, never deletions.
  std::vector<std::string> recoveryPaths;
};
struct EntryOutcome {
  std::string source, destination, status, detail;
  std::optional<UploadVerification> upload;
};
struct Result {
  int completed = 0, skipped = 0;
  bool cancelled = false;
  bool copyLimitations = false;
  int recoverableMoves = 0;
  std::vector<std::string> errors;
  std::vector<std::pair<std::string, std::string>> completedRoots;
  // A cancelled local move left a complete copy and removed no source entries.
  std::optional<std::pair<std::string, std::string>> retainedMoveCopy;
  // Selected roots and individual entries visited during a non-atomic transfer.
  std::vector<EntryOutcome> outcomes;
  std::optional<Job> bulkRenameUndo;
  bool bulkRenameRecoveryNeeded = false;
  // Do not reread this folder automatically after a removable transfer lost it.
  std::string interruptedCopyFolder;
  int partialCopies = 0; // Selected folders changed but not fully copied.
};
Result Run(const Job &, const Hooks &);
std::string Join(const std::string &, const std::string &);
std::string Leaf(const std::string &);
int SelfTest();
} // namespace Listi
