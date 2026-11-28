// SPDX-License-Identifier: MIT
// Compile on POSIX: c++ -std=c++17 -Isrc tests/virtual-operation-faults.cpp
// src/JobReportStore.cpp src/DirectoryFinalization.cpp src/VirtualOperation.cpp
// src/VirtualFile.cpp src/VirtualLocation.cpp src/Sha256.cpp src/RemovableCopy.cpp
// -pthread -lsqlite3 -o /tmp/listi-faults
// Real staging/dispatch code, deterministic fake network commit failures.
#include "ArchiveVolumes.h"
#include "FileOperation.h"
#include "FolderCompare.h"
#include "LocalTransferSelection.h"
#include <filesystem>
#if defined(__HAIKU__) || defined(LISTI_DOWNLOAD_STAGE_PROBE)
#include "DownloadStageProbe.h"
#endif
#include "JobReportStore.h"
#include "VirtualFile.h"
#include "SftpFile.h"
#include "UnavailableVolume.h"
#include <algorithm>
#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <sys/stat.h>
#include <unistd.h>
// The portable transport fixture has no Haiku BMessage or settings store.
// Native tests link the real durable inventory; only its storage is mocked here.
#ifndef __HAIKU__
#include "RecoveryInventory.h"
class BMessage {};
namespace Listi {
RecoveryRecord::RecoveryRecord(const std::string &kind, const std::string &folder,
    const std::string &prefix, const std::string &, const std::string &,
    const std::string &) {
  static unsigned sequence = 0;
  const auto name = folder + "/" + prefix + std::to_string(++sequence);
  stage = kind == "upload" ? name + ".part" : name;
  backup = name + ".previous";
}
RecoveryRecord::~RecoveryRecord() = default;
void RecoveryRecord::Advance(RecoveryPhase next, const struct stat *) {
  if (int(next) != int(phase) + 1)
    throw std::logic_error("Unexpected journal transition");
  phase = next;
}
void RecoveryRecord::Retire() {}
void RecoveryRecord::NoteFailure(bool) noexcept {}
}
#endif
namespace {
struct Remote {
  std::string data;
  int64_t modified = 1;
  long nanoseconds = 0;
  bool directory = false;
  std::string identity;
  std::optional<unsigned> permissions;
  bool modifiedKnown = true;
};
std::map<std::string, Remote> files;
std::map<std::string, Listi::SftpPeer> peers;
unsigned mutationRequests = 0, statRequests = 0;
bool foldDirectoryNames = false;
std::string StoragePath(std::string path) {
  constexpr char alias[] = "/drafts";
  const auto at = path.find(alias);
  if (foldDirectoryNames && path.rfind("sftp://fixture/out/", 0) == 0 && at != std::string::npos &&
      (path.size() == at + sizeof(alias) - 1 || path[at + sizeof(alias) - 1] == '/'))
    path.replace(at, sizeof(alias) - 1, "/Drafts");
  if (foldDirectoryNames && path.rfind("sftp://fixture/out/", 0) == 0 &&
      Listi::LocationLeaf(path) == "report")
    path.replace(path.size() - 6, 6, "Report");
  return path;
}
bool corruptUpload = false, denyReadback = false, cancelReadback = false;
bool cancelAtReadyCheck = false, cancelAfterStore = false, hideStoredPart = false;
bool uncertainRollback = false, replaceStageOnRead = false;
std::string unresolvedCanonical, canonicalAlias, omittedSftpName, specialSftpName;
unsigned specialLookups = 0, specialReads = 0;
std::string changeIdentityOnRead, tightenOnRead, downgradeListing;
std::string changedAfterList, changedAfterRemove;
std::string lostRemoval, unreadableLookup, lostMove, swapDuringMove;
bool moveBeforeLoss = false, loseMoveLookup = false;
unsigned lostMoveRequests = 0;
bool loseLookupAfterRemoval = false, giveCreatedFoldersIds = false;
std::map<std::string, unsigned> listingCounts, listingRows;
unsigned folderReads = 0;
unsigned postCancelReadCalls = 0;
bool commitFailed = false, expireDuringCleanup = false;
size_t cleanupReadBytes = 0;
bool editSmbSourceOnRead = false;
bool truncateUpload = false, omitStageSize = false, growSourceOnRead = false;
bool failCommit = false, failRollback = false, failPartCleanup = false,
     failBackupCleanup = false, cancelAfterBackup = false, cancelled = false,
     mutateDestination = false, loseBackupReply = false, loseCommitReply = false;
const std::string destination = "sftp://fixture/out/input";
std::string unreadableSource, refuseRemoval;
std::function<void(const std::string &)> afterPrefix;
std::vector<std::pair<std::string, size_t>> prefixReads;
enum class LocalPublish { Unexpected, Limited, FlushFailure, CancelAfterPublish };
LocalPublish localPublish = LocalPublish::Unexpected;
bool expectPrivatePublication = false;
bool suffix(const std::string &path, const char *part) {
  return path.find(part) != std::string::npos;
}
}
namespace Listi {
Result RunVirtual(const Job &, const Hooks &);
Result Run(const Job &job, const Hooks &hooks) {
  if (localPublish == LocalPublish::Unexpected ||
      hooks.privateStaging != expectPrivatePublication ||
      hooks.report || hooks.copyConflict || job.sources.size() != 1)
    throw std::runtime_error("Unexpected local dispatch");
  const auto &source = job.sources.front();
  const auto target = ChildLocation(job.destination, LocationLeaf(source));
  Result result;
  auto &entry = result.outcomes.emplace_back(EntryOutcome{source, target, "unprocessed", ""});
  if (access(target.c_str(), F_OK) == 0) {
    const auto decision = hooks.conflict(target);
    if (decision == Decision::Skip) {
      result.skipped = 1;
      entry.status = "skipped";
      return result;
    }
    if (decision == Decision::Replace && hooks.checkReplacementSource)
      hooks.checkReplacementSource();
    if (decision == Decision::Abort) {
      result.cancelled = true;
      entry.detail = "Cancelled before commit.";
      return result;
    }
  }
  // Exercise real virtual staging. Only the native publication result is
  // replaced by deterministic faults in this portable fixture.
  std::ifstream staged(source, std::ios::binary);
  std::ofstream output(target, std::ios::binary);
  output << staged.rdbuf();
  output.close();
  if (!staged || !output)
    throw std::runtime_error("Failed to publish fixture file");
  if (localPublish == LocalPublish::Limited) {
    result.completed = 1;
    result.copyLimitations = true;
    entry.status = "copied";
    entry.detail = "Destination could not preserve metadata.";
  } else {
    entry.status = "published-flush-failed";
    if (localPublish == LocalPublish::CancelAfterPublish) {
      result.cancelled = true;
      entry.detail = "Published before cancellation; durability is unknown.";
    } else {
      entry.detail = "Published, but directory flush failed.";
      result.errors.push_back(entry.detail);
      result.interruptedCopyFolder = job.destination;
    }
  }
  return result;
}
std::vector<FileEntry> ListArchive(const Location &, const TransferControl &) {
  throw std::runtime_error("Unexpected archive dispatch");
}
bool StatArchive(const Location &, FileEntry &, const TransferControl &) { throw std::runtime_error("Unexpected archive dispatch"); }
void ReadArchive(const Location &, const DataSink &, const TransferControl &) {
  throw std::runtime_error("Unexpected archive dispatch");
}
bool StatNetwork(const Location &l, FileEntry &entry, const TransferControl &c,
                 const FileEntry *) {
  ++statRequests;
  if (l.URL() == specialSftpName) {
    ++specialLookups;
    throw UnsupportedSftpEntry("Unsupported SFTP fixture entry");
  }
  if (expireDuringCleanup && c.cleanup)
    c.cleanup->deadline = NetworkCleanupBudget::Clock::now();
  // The target is statted before staging, then again after Ready.
  if (cancelAtReadyCheck && l.URL() == destination && files.size() == 2)
    cancelled = true;
  c.Check();
  if ((hideStoredPart && suffix(l.URL(), ".part") && files.count(l.URL())) ||
      (uncertainRollback && commitFailed && c.cleanup && l.URL() == destination))
    throw std::runtime_error("FTP LIST cannot confirm absence");
  if (l.URL() == unreadableLookup)
    throw std::runtime_error("Fixture lookup unavailable");
  if (c.requireExactName && StoragePath(l.URL()) != l.URL())
    throw std::runtime_error("SFTP entry no longer has its selected name");
  auto it = files.find(StoragePath(l.URL()));
  if (it == files.end()) {
    if (l.path != "/out" && l.path != "/" &&
        !(l.scheme == "sftp" && l.path == "/in"))
      return false;
    entry.directory = true;
    entry.providerIdentity = "fixture-parent";
    return true;
  }
  entry.path = l.URL(); entry.name = LocationLeaf(entry.path);
  entry.directory = it->second.directory;
  entry.permissions = it->second.permissions.value_or(entry.directory ? 0755 : 0644);
  entry.providerIdentity = it->second.identity;
  entry.sizeKnown = !omitStageSize || !suffix(l.URL(), ".part");
  entry.size = it->second.data.size();
  if (it->second.modifiedKnown)
    entry.SetModification(it->second.modified, it->second.nanoseconds);
  return true;
}
SftpPeer BindSftpPeer(const Location &entry, const TransferControl &control) {
  control.Check();
  if (!peers.count(entry.host))
    throw std::runtime_error("Cannot identify fixture peer");
  return peers.at(entry.host);
}
std::string CanonicalSftpPath(const Location &entry, const TransferControl &control) {
  control.Check();
  if (entry.URL() == unresolvedCanonical)
    throw std::runtime_error("Canonical path unavailable for fixture entry");
  if (entry.URL() == canonicalAlias)
    return "/work";
  return ParseLocation(StoragePath(entry.URL())).path;
}
std::set<std::string> ListSftpNames(const Location &folder, const TransferControl &c) {
  std::set<std::string> names;
  if (folder.path == "/") {
    names.insert("in");
    names.insert("out");
  }
  for (const auto &[path, file] : files) {
    c.Check();
    if (ParentLocation(path) == folder.URL() && path != omittedSftpName)
      names.insert(LocationLeaf(path));
  }
  return names;
}
ArchiveVolumes FindArchiveVolumes(const std::string &) {
  throw std::runtime_error("Unexpected archive comparison");
}
std::string ArchiveVolumes::Fingerprint() const {
  throw std::runtime_error("Unexpected archive fingerprint");
}
bool RecheckFtpListingEntry(const Location &, FileEntry &, const TransferControl &) {
  throw std::runtime_error("Unexpected FTP comparison");
}
std::vector<FileEntry> ListNetwork(const Location &l, const TransferControl &c,
                                   ListingMethod *method) {
  ++listingCounts[l.URL()];
  if (method)
    *method = l.URL() == downgradeListing && ++folderReads > 1
                  ? ListingMethod::FtpList : ListingMethod::Provider;
  std::vector<FileEntry> out;
  for (auto &item : files) {
    if (ParentLocation(item.first) == l.URL()) {
      FileEntry entry;
      StatNetwork(ParseLocation(item.first), entry, c);
      out.push_back(entry);
    }
  }
  if (l.URL() == changedAfterList)
    files.at(changedAfterList).identity = "new-folder";
  listingRows[l.URL()] += out.size();
  return out;
}
void ReadNetwork(const Location &l, const DataSink &sink, const TransferControl &c) {
  if (l.URL() == specialSftpName) {
    ++specialReads;
    throw std::runtime_error("Unexpected read of a special SFTP fixture entry");
  }
  if (cancelled)
    ++postCancelReadCalls;
  c.Check();
  if (l.URL() == unreadableSource)
    throw std::runtime_error("Source read denied");
  if (cancelReadback && suffix(l.URL(), ".part")) {
    cancelled = true;
    c.Check();
  }
  if (denyReadback && suffix(l.URL(), ".part")) throw std::runtime_error("readback denied");
  auto &data = files.at(StoragePath(l.URL())).data;
  if (commitFailed)
    cleanupReadBytes += data.size();
  if (growSourceOnRead && l.URL() == "sftp://fixture/in/input") {
    std::string grown = data + "more data";
    sink(grown.data(), grown.size());
  } else {
    sink(data.data(), data.size());
  }
  if (l.URL() == tightenOnRead)
    files.at(l.URL()).permissions = 0600;
  if (l.URL() == changeIdentityOnRead || (replaceStageOnRead && suffix(l.URL(), ".part")))
    files.at(l.URL()).identity = "replaced";
  if (editSmbSourceOnRead && l.scheme == "smb") {
    data = "changed";
    files.at(l.URL()).nanoseconds = 654000000;
  }
  c.Tick(data.size());
}
void ReadSftpPrefix(const Location &l, const FileEntry &, size_t bytes, const DataSink &sink,
                    const TransferControl &c) {
  c.Check();
  prefixReads.emplace_back(l.URL(), bytes);
  if (l.URL() == specialSftpName || l.URL() == unreadableSource)
    throw WitnessAccessDenied("Witness read denied");
  const auto &data = files.at(StoragePath(l.URL())).data;
  if (bytes > data.size())
    throw std::runtime_error("Short witness");
  sink(data.data(), bytes);
  if (afterPrefix)
    afterPrefix(l.URL());
  c.Tick(bytes);
}
void UploadNetwork(const std::string &local, const Location &l, const TransferControl &c) {
  ++mutationRequests;
  std::ifstream stream(local);
  std::string data(std::istreambuf_iterator<char>(stream), {});
  if (corruptUpload && !data.empty()) data.front() ^= 1;
  files[StoragePath(l.URL())] = {truncateUpload ? data.substr(0, data.size() / 2) : data, 1};
  if (mutateDestination) files[destination] = {"concurrent edit", 2};
  c.Tick(data.size());
  if (cancelAfterStore)
    cancelled = true;
}
void ChangeNetwork(const Location &l, const char *action,
                   const std::string &target, const TransferControl &c) {
  c.Check();
  ++mutationRequests;
  if (c.mutationSubmitted)
    *c.mutationSubmitted = true;
  std::string source = StoragePath(l.URL());
  if (std::string(action) == "mkdir") {
    files.emplace(source, Remote{"", 1, 0, true, giveCreatedFoldersIds ? "created:" + source : ""});
    return;
  }
  if (std::string(action) == "rmdir") {
    for (const auto &child : files)
      if (ParentLocation(child.first) == source)
        throw std::runtime_error("Directory not empty");
    if (!files.at(source).directory)
      throw std::runtime_error("Not a directory");
    files.erase(source);
    return;
  }
  if (std::string(action) == "remove") {
    if (source == refuseRemoval || files.at(source).directory)
      throw std::runtime_error("Deletion refused");
    if ((suffix(source, ".part") && failPartCleanup) ||
        (suffix(source, ".previous") && failBackupCleanup))
      throw std::runtime_error("injected cleanup failure");
    files.erase(source);
    if (ParentLocation(source) == changedAfterRemove)
      files.at(changedAfterRemove).identity = "new-folder";
    if (source == lostRemoval) {
      if (loseLookupAfterRemoval)
        unreadableLookup = source;
      throw std::runtime_error("Removal acknowledgement lost");
    }
    return;
  }
  if (std::string(action) != "rename")
    throw std::runtime_error("Unexpected operation");
  if (suffix(source, ".part") && failCommit) {
    commitFailed = true;
    throw std::runtime_error("injected commit failure");
  }
  if (suffix(source, ".previous") && failRollback)
    throw std::runtime_error("injected rename failure");
  if (!files.count(source) || files.count(StoragePath(target)))
    throw std::runtime_error("rename precondition");
  if (source == lostMove) {
    ++lostMoveRequests;
    if (loseMoveLookup)
      unreadableLookup = target;
    if (!moveBeforeLoss)
      throw std::runtime_error("Lost rename reply");
  }
  files[StoragePath(target)] = files.at(source); files.erase(source);
  if (source == swapDuringMove) {
    files["smb://fixture/aside/" + LocationLeaf(target)] = files.at(target);
    files.erase(target);
    files.at(ParentLocation(target)).identity = "replacement";
  }
  if (source == lostMove || (loseBackupReply && suffix(target, ".previous")) ||
      (loseCommitReply && suffix(source, ".part")))
    throw std::runtime_error("reply lost after server rename");
  if (cancelAfterBackup && suffix(target, ".previous")) cancelled = true;
}
}
int main() {
  using namespace Listi;
  char pattern[] = "/tmp/listi-faults-XXXXXX";
  char *dir = mkdtemp(pattern);
  if (!dir) return 1;
  std::string source = std::string(dir) + "/input";
  { std::ofstream stream(source); stream << "new contents"; }
  Job job{Action::Copy, {source}, "sftp://fixture/out"};
  int checks = 0, failures = 0, done = 0;
  auto check = [&](bool ok, const char *label) {
    ++checks; failures += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << label << '\n';
  };
  auto reset = [&] {
    peers.clear();
    peers["fixture"] = {"remote:22", "fixture-key", 1, false};
    mutationRequests = statRequests = 0;
    postCancelReadCalls = 0;
    commitFailed = expireDuringCleanup = false;
    cleanupReadBytes = 0;
    cancelAtReadyCheck = cancelAfterStore = hideStoredPart = uncertainRollback = replaceStageOnRead = false;
    changeIdentityOnRead.clear();
    tightenOnRead.clear();
    unreadableLookup.clear();
    unresolvedCanonical.clear();
    canonicalAlias.clear();
    omittedSftpName.clear();
    specialSftpName.clear();
    afterPrefix = {};
    prefixReads.clear();
    specialLookups = specialReads = 0;
    changedAfterList.clear();
    changedAfterRemove.clear();
    lostRemoval.clear();
    lostMove.clear();
    swapDuringMove.clear();
    lostMoveRequests = 0;
    moveBeforeLoss = loseMoveLookup = false;
    giveCreatedFoldersIds = loseLookupAfterRemoval = false;
    listingCounts.clear();
    listingRows.clear();
    downgradeListing.clear();
    folderReads = 0;
    cancelReadback = corruptUpload = denyReadback = truncateUpload = omitStageSize = false;
    files = {{destination, {"old contents", 1}}}; done = 0;
    failCommit = failRollback = failPartCleanup = failBackupCleanup = false;
    cancelAfterBackup = cancelled = mutateDestination = loseBackupReply = loseCommitReply = false;
  };
  auto has = [&](const char *part) {
    for (auto &item : files) if (suffix(item.first, part)) return true;
    return false;
  };
  auto errorHas = [](const Result &r, const char *text) {
    for (auto &error : r.errors) if (error.find(text) != std::string::npos) return true;
    return false;
  };
  auto reportedCopy = [&](const Result &r, const char *suffixText, const std::string &bytes) {
    if (r.outcomes.size() != 1 || !r.outcomes.front().upload)
      return false;
    for (const auto &path : r.outcomes.front().upload->recoveryPaths) {
      const auto found = files.find(path);
      if (suffix(path, suffixText) && found != files.end() && found->second.data == bytes)
        return true;
    }
    return false;
  };
  Hooks hooks;
  hooks.proceed = [&] { return !cancelled; };
  hooks.conflict = [](const std::string &) { return Decision::Replace; };
  hooks.fileDone = [&](const std::string &, const std::string &) { ++done; };
  reset(); auto r = RunVirtual(job, hooks);
  check(r.completed == 1 && r.errors.empty() && done == 1 &&
        files.size() == 1 && files.at(destination).data == "new contents" &&
        r.outcomes.size() == 1 && r.outcomes.front().upload &&
        r.outcomes.front().upload->bytesWritten == 12 &&
        r.outcomes.front().upload->destinationSizeChecked &&
        r.outcomes.front().upload->contentVerified &&
        r.outcomes.front().upload->publication == Publication::Published,
        "successful replacement commits once and removes its backup");
  reset();
  replaceStageOnRead = true;
  r = RunVirtual(job, hooks);
  check(!r.completed && !done && !r.errors.empty() && files.at(destination).data == "old contents" &&
            reportedCopy(r, ".part", "new contents") && !r.outcomes.front().upload->contentVerified,
        "staged upload replaced during readback cannot be published or silently removed");
  for (bool cancel : {false, true}) {
    reset();
    hideStoredPart = true;
    cancelAfterStore = cancel;
    r = RunVirtual(job, hooks);
    check(!r.completed && !done && r.cancelled == cancel &&
              r.outcomes.size() == 1 && r.outcomes.front().upload &&
              r.outcomes.front().upload->bytesWritten == 12 &&
              r.outcomes.front().upload->publication == Publication::NotAttempted &&
              reportedCopy(r, ".part", "new contents") && errorHas(r, ".part") &&
              files.at(destination).data == "old contents" && !postCancelReadCalls,
          cancel ? "cancel after acknowledged STOR retains the exact hidden recovery name"
                 : "unknown stage lookup after acknowledged STOR retains the exact recovery name");
  }
  reset();
  failCommit = uncertainRollback = true;
  r = RunVirtual(job, hooks);
  check(!r.completed && !done && !files.count(destination) &&
            reportedCopy(r, ".previous", "old contents") && errorHas(r, ".previous"),
        "unknown final name during rollback prevents restoring over it and reports the backup");
  reset(); growSourceOnRead = true;
  const std::string remote = "sftp://fixture/in/input";
  files[remote] = {"initial", 1};
  r = RunVirtual(Job{Action::Copy, {remote}, "sftp://fixture/out"}, hooks);
  check(!r.completed && !done && !has(".part") &&
        errorHas(r, "exceeded its declared size") &&
        files.at(remote).data == "initial" &&
        files.at(destination).data == "old contents",
        "staging rejects actual bytes beyond the declared source size before upload");
  growSourceOnRead = false;
  for (bool replaced : {false, true}) {
    reset();
    files[remote] = {"stable bytes", 1, 0, false, "selected"};
    if (replaced)
      changeIdentityOnRead = remote;
    r = RunVirtual({Action::Copy, {remote}, "sftp://fixture/out"}, hooks);
    check(replaced ? (!r.completed && !done && errorHas(r, "Source changed") &&
                      files.at(destination).data == "old contents")
                   : (r.completed == 1 && done == 1 && r.errors.empty() &&
                      files.at(destination).data == "stable bytes"),
          replaced ? "changed provider identity with equal size and time stops transfer"
                   : "stable provider identity permits transfer");
  }
  const std::string smb = "smb://fixture/in/edited";
  const std::string downloaded = std::string(dir) + "/edited";
  reset();
  files[smb] = {"initial", 1, 123000000};
  editSmbSourceOnRead = true;
  r = RunVirtual(Job{Action::Copy, {smb}, dir}, hooks);
  check(!r.completed && !done && !r.errors.empty() && access(downloaded.c_str(), F_OK),
        "same-size SMB source edit with a distinct fractional revision is rejected");
  editSmbSourceOnRead = false;
  const std::string downloadFolder = std::string(dir) + "/downloads";
  if (mkdir(downloadFolder.c_str(), 0700)) return 1;
  const auto localTarget = downloadFolder + "/input";
  const Job downloadJob{Action::Copy, {remote}, downloadFolder};
  auto download = [&](LocalPublish publication, Decision decision, bool report,
                      bool privateStaging = false, const char *edit = nullptr, bool hasTime = true) {
    reset();
    localPublish = publication;
    expectPrivatePublication = privateStaging;
    files[remote] = {"downloaded payload", 1};
    files[remote].modifiedKnown = hasTime;
    Hooks local = hooks;
    local.privateStaging = privateStaging;
    local.copyConflict = [](const CopyConflict &) {
      throw std::runtime_error("Synthetic file should not reach user conflict preview");
      return Decision::Abort;
    };
    local.conflict = [=](const std::string &target) {
      if (target != localTarget)
        throw std::runtime_error("Wrong conflict target");
      if (edit)
        files.at(remote).data = edit;
      return decision;
    };
    local.fileDone = [&](const std::string &from, const std::string &to) {
      if (from == remote && to == localTarget)
        ++done;
    };
    if (report)
      local.report = std::make_shared<JobReportStore>();
    auto result = RunVirtual(downloadJob, local);
    expectPrivatePublication = false;
    localPublish = LocalPublish::Unexpected;
    return std::make_pair(std::move(result), local.report);
  };
  auto actualPaths = [&](const std::vector<EntryOutcome> &rows, const char *status) {
    return rows.size() == 1 && rows.front().source == remote &&
           rows.front().destination == localTarget && rows.front().status == status;
  };
  auto localContents = [&] {
    std::ifstream stream(localTarget);
    return std::string(std::istreambuf_iterator<char>(stream), {});
  };
  auto [limited, report] = download(LocalPublish::Limited, Decision::Replace, true);
  auto page = report->Page(0, 10);
  check(limited.completed == 1 && !limited.skipped && !limited.cancelled &&
            limited.copyLimitations && limited.errors.empty() && limited.outcomes.empty() &&
            report->OutcomeCount() == 1 && report->ErrorCount() == 0 &&
            actualPaths(page.outcomes, "copied") &&
            page.outcomes.front().detail == "Destination could not preserve metadata." &&
            done == 1 && localContents() == "downloaded payload",
        "download reports one copied outcome with actual paths and limitations");
  std::ofstream(localTarget) << "existing data";
  auto skipped = download(LocalPublish::Limited, Decision::Skip, false).first;
  check(!skipped.completed && skipped.skipped == 1 && !skipped.cancelled && !done &&
            skipped.errors.empty() && actualPaths(skipped.outcomes, "skipped") &&
            localContents() == "existing data",
        "download collision is recorded and counted only once when skipped");
  for (bool hasTime : {false, true}) {
    for (const char *edit : {"Downloaded payload", "different length"}) {
      for (auto answer : {Decision::Replace, Decision::Skip, Decision::ReplaceAll}) {
        std::ofstream(localTarget) << "existing data";
        auto changed = download(LocalPublish::Limited, answer, false, false, edit, hasTime).first;
        check(!changed.completed && !done && localContents() == "existing data" &&
                  files.at(remote).data == edit &&
                  (answer == Decision::Skip ? changed.skipped == 1 && changed.errors.empty()
                                            : errorHas(changed, "Source changed before replacement")),
              "a changed network source cannot replace a local conflict; Skip still skips");
      }
    }
  }
  std::ofstream(localTarget) << "existing data";
  auto aborted = download(LocalPublish::Limited, Decision::Abort, false).first;
  check(aborted.cancelled && !aborted.skipped && !aborted.completed && !done &&
            aborted.errors.empty() && actualPaths(aborted.outcomes, "unprocessed") &&
            localContents() == "existing data",
        "cancelled download at a conflict reports unprocessed and preserves the target");
  auto [failed, failedReport] = download(LocalPublish::FlushFailure, Decision::Replace, true);
  page = failedReport->Page(0, 10);
  check(!failed.completed && !failed.skipped && !failed.cancelled && !done &&
            failed.interruptedCopyFolder == downloadFolder && failedReport->ErrorCount() == 1 &&
            failedReport->OutcomeCount() == 1 && actualPaths(page.outcomes, "published-flush-failed") &&
            localContents() == "downloaded payload",
        "download preserves the published outcome on a durability failure and reports one error");
  auto cancelledPublish = download(LocalPublish::CancelAfterPublish, Decision::Replace, false).first;
  check(cancelledPublish.cancelled && !cancelledPublish.completed && !cancelledPublish.skipped &&
            !done && cancelledPublish.errors.empty() &&
            actualPaths(cancelledPublish.outcomes, "published-flush-failed") &&
            cancelledPublish.outcomes.front().detail.find("durability is unknown") != std::string::npos &&
            localContents() == "downloaded payload",
        "download cancelled after publish retains the durability outcome without claiming completion");
  auto privatePublish = download(LocalPublish::Limited, Decision::Replace, false, true).first;
  check(privatePublish.completed == 1 && privatePublish.errors.empty() &&
            privatePublish.outcomes.empty() && done == 1,
        "archive staging keeps strict local publication policy and hides internal receipts");
  unlink(localTarget.c_str());

#if defined(__HAIKU__) || defined(LISTI_DOWNLOAD_STAGE_PROBE)
  {
    namespace Probe = DownloadStageProbe;
    const auto first = std::string("sftp://fixture/in/") + Probe::smallName;
    const auto second = std::string("sftp://fixture/in/") + Probe::largeName;
    reset();
    files[first] = {std::string(32768, 'a'), 1};
    files[second] = {std::string(Probe::largeSize, 'b'), 2};
    localPublish = LocalPublish::Limited;
    Job batch{Action::Copy, {first, second}, downloadFolder};
    Probe::Exercise(batch, RunVirtual, "SFTP provider fixture");

    const auto smaller = downloadFolder + "/" + Probe::smallName;
    const auto larger = downloadFolder + "/" + Probe::largeName;
    std::ofstream(smaller) << "keep";
    std::ofstream(larger) << "keep";
    files[second].identity = "captured";
    StatFile(second, batch.expectedRemoteSources[second]);
    Hooks changed;
    changed.conflict = [&](const std::string &) {
      files[second].identity = "replaced";
      return Decision::SkipAll;
    };
    const auto stale = RunVirtual(batch, changed);
    check(stale.skipped == 1 && !stale.completed &&
              errorHas(stale, "Selected network entry changed") && Probe::Read(larger) == "keep",
          "Skip All does not silence a stale provider source identity");
    files[second].identity = "captured";
    bool protectedNow = false;
    changed.conflict = [&](const std::string &) { protectedNow = true; return Decision::SkipAll; };
    changed.checkOverwrite = [&](const std::string &path, const struct stat &) {
      if (protectedNow && path == larger)
        throw std::runtime_error("protected after first conflict");
    };
    const auto guarded = RunVirtual(batch, changed);
    check(guarded.skipped == 1 && !guarded.completed &&
              errorHas(guarded, "protected after first conflict") && Probe::Read(larger) == "keep",
          "Skip All rechecks protected targets after the first conflict");
    unlink(smaller.c_str());
    unlink(larger.c_str());
    localPublish = LocalPublish::Unexpected;
    reset();
  }
#endif

  const std::string sourceFolder = "sftp://fixture/in/folder";
  const std::string healthy = sourceFolder + "/a-readable";
  const std::string denied = sourceFolder + "/z-unreadable";
  files[sourceFolder] = {"", 1, 0, true};
  files[healthy] = {"healthy payload", 1};
  files[denied] = {"not readable", 1};
  localPublish = LocalPublish::Limited;
  unreadableSource = denied;
  auto partial = RunVirtual({Action::Copy, {sourceFolder}, downloadFolder}, {});
  unreadableSource.clear();
  localPublish = LocalPublish::Unexpected;
  const auto copiedFolder = downloadFolder + "/folder";
  check(!partial.completed && partial.partialCopies == 1 &&
            errorHas(partial, "Source read denied") && partial.outcomes.size() == 3 &&
            partial.outcomes.front().status == "folder created" &&
            partial.outcomes[1].source == healthy && partial.outcomes[1].status == "copied" &&
            partial.outcomes.back().source == sourceFolder &&
            partial.outcomes.back().destination == copiedFolder &&
            partial.outcomes.back().status == "partial" &&
            !access((copiedFolder + "/a-readable").c_str(), F_OK) &&
            access((copiedFolder + "/z-unreadable").c_str(), F_OK),
        "folder download keeps the successful child receipt after a later read failure");
  unlink((copiedFolder + "/a-readable").c_str());
  rmdir(copiedFolder.c_str());
  reset();
  const auto nestedRemote = sourceFolder + "/nested";
  const auto delivered = nestedRemote + "/visible";
  files[sourceFolder] = {"", 1, 0, true};
  files[nestedRemote] = {"", 1, 0, true};
  files[delivered] = {"visible bytes"};
  downgradeListing = nestedRemote;
  localPublish = LocalPublish::Limited;
  partial = RunVirtual({Action::Copy, {sourceFolder}, downloadFolder}, hooks);
  localPublish = LocalPublish::Unexpected;
  check(!partial.completed && done == 1 && partial.partialCopies == 1 &&
            errorHas(partial, nestedRemote.c_str()) && errorHas(partial, "Only entries shown") &&
            partial.outcomes.size() == 4 && partial.outcomes[0].status == "folder created" &&
            partial.outcomes[1].status == "folder created" && partial.outcomes[2].source == delivered &&
            partial.outcomes[2].status == "copied" && partial.outcomes.back().source == sourceFolder &&
            partial.outcomes.back().status == "partial",
        "a nested FTP fallback on final listing retains the file receipt but not folder completion");
  unlink((copiedFolder + "/nested/visible").c_str());
  rmdir((copiedFolder + "/nested").c_str());
  rmdir(copiedFolder.c_str());
  auto readBytes = [](const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
  };
  for (bool existing : {false, true}) {
    for (bool lastChild : {false, true}) {
      reset();
      files[sourceFolder] = {"", 1, 0, true, "folder-id", 0755};
      files[healthy] = {"readable memo", 1};
      if (!lastChild)
        files[denied] = {"other readable memo", 1};
      const auto keep = copiedFolder + "/unrelated";
      if (existing) {
        mkdir(copiedFolder.c_str(), 0755);
        chmod(copiedFolder.c_str(), 0755);
        std::ofstream(keep) << "untouched";
      }
      localPublish = LocalPublish::Limited;
      Hooks tightened = hooks;
      tightened.fileDone = [&](const auto &path, const auto &) {
        if (path == healthy)
          files[sourceFolder].permissions = 0700;
      };
      r = RunVirtual({Action::Copy, {sourceFolder}, downloadFolder}, tightened);
      const auto published = copiedFolder + "/a-readable";
      struct stat finalDirectory;
      check(!r.completed && r.partialCopies == 1 && errorHas(r, "Source folder changed") &&
                !lstat(copiedFolder.c_str(), &finalDirectory) &&
                (finalDirectory.st_mode & 0777) == (existing ? 0755 : 0700) &&
                readBytes(published) == "readable memo" &&
                access((copiedFolder + "/z-unreadable").c_str(), F_OK) &&
                (!existing || readBytes(keep) == "untouched") &&
                std::any_of(r.outcomes.begin(), r.outcomes.end(), [&](const auto &row) {
                  return row.source == healthy && row.destination == published && row.status == "copied";
                }),
            "source access tightening stops further download or final completion with correct child receipt");
      unlink(published.c_str());
      if (existing)
        unlink(keep.c_str());
      rmdir(copiedFolder.c_str());
    }
  }
  localPublish = LocalPublish::Unexpected;
  rmdir(downloadFolder.c_str());
  const timespec referenceTime[] = {{0, UTIME_OMIT}, {1, 654000000}};
  FileEntry expected;
  expected.SetModification(0, 123400000);
  const auto prior = expected;
  expected.SetModification(0, 0);
  check(prior.modifiedFraction == "1234" && expected.modifiedKnown &&
            expected.modifiedFraction.empty() && !prior.SameModification(expected),
        "epoch fractions normalize, distinguish revisions, and clear on reuse");
  struct stat reference{};
  check(!utimensat(AT_FDCWD, source.c_str(), referenceTime, 0) &&
            !stat(source.c_str(), &reference) && StatFile(source, expected) &&
            expected.modified == reference.st_mtim.tv_sec &&
            expected.ModifiedNanoseconds() == reference.st_mtim.tv_nsec,
        "StatFile preserves the local filesystem timestamp precision");
  reset(); truncateUpload = true; r = RunVirtual(job, hooks);
  check(!r.completed && !done && !r.errors.empty() && files.size() == 2 && has(".part") && errorHas(r, ".part") &&
        files.at(destination).data == "old contents",
        "acknowledged short upload cannot replace the original");
  for (const auto publication : {Publication::NotAttempted, Publication::Unknown, Publication::Published}) {
    reset();
    truncateUpload = publication == Publication::NotAttempted;
    loseCommitReply = publication == Publication::Unknown;
    auto reporting = hooks;
    reporting.report = std::make_shared<JobReportStore>();
    r = RunVirtual(job, reporting);
    const auto page = reporting.report->Page(0, 10);
    const bool published = publication == Publication::Published;
    const bool attempted = publication != Publication::NotAttempted;
    const auto &expectedLabel = !attempted ? "Upload not published" :
                                published ? "Completed" : "Outcome unknown";
    const auto &expectedDetail = !attempted ? "Final name not published." :
                                 published ? "Destination published." : "Destination publication not confirmed.";
    check(page.outcomes.size() == 1 && r.completed == int(published) &&
              files.at(destination).data == (attempted ? "new contents" : "old contents") &&
              OutcomeLabel(page.outcomes[0].status) == expectedLabel &&
              page.outcomes[0].detail.find(expectedDetail) != std::string::npos &&
              page.outcomes[0].detail.find("Bytes acknowledged by transport: 12") != std::string::npos &&
              (page.outcomes[0].detail.find("Staged contents verified.") != std::string::npos) == attempted &&
              (published || page.outcomes[0].detail.find("Recovery path: ") != std::string::npos),
          "Job Details distinguishes an acknowledged upload from publication at the final name");
  }
  check(OutcomeLabel("source-retained") == "Copied; source retained",
        "local move receipt still identifies a delivered copy when the original remains");
  reset(); omitStageSize = true; r = RunVirtual(job, hooks);
  check(r.completed == 1 && done == 1 && r.errors.empty() && files.size() == 1 &&
        files.at(destination).data == "new contents",
        "valid upload with no server size is verified by reading back the stage");
  reset(); omitStageSize = truncateUpload = true; r = RunVirtual(job, hooks);
  check(!r.completed && !done && !r.errors.empty() && files.size() == 2 && has(".part") && errorHas(r, ".part") &&
        files.at(destination).data == "old contents",
        "short upload with no server size cannot replace the original");
  for (bool knownSize : {false, true}) {
    reset(); corruptUpload = true; omitStageSize = !knownSize; r = RunVirtual(job, hooks);
    check(!r.completed && !done && !r.errors.empty() && files.size() == 2 && has(".part") && errorHas(r, ".part") &&
          files.at(destination).data == "old contents",
          "same-length corruption cannot replace the original");
  }
  reset(); denyReadback = true; r = RunVirtual(job, hooks);
  check(!r.completed && !done && !r.errors.empty() && files.size() == 2 && has(".part") && errorHas(r, ".part") &&
        files.at(destination).data == "old contents", "unreadable stage cannot publish");
  denyReadback = false; r = RunVirtual(job, hooks);
  check(r.completed == 1 && done == 1 && files.at(destination).data == "new contents",
        "retry after failed verification succeeds");
  reset(); cancelReadback = true; r = RunVirtual(job, hooks);
  check(r.cancelled && !r.completed && !done && files.size() == 2 && has(".part") &&
        files.at(destination).data == "old contents" && r.outcomes.front().upload &&
        !r.outcomes.front().upload->contentVerified &&
        r.outcomes.front().upload->publication == Publication::NotAttempted,
        "cancelled readback never touches the original target");
  reset(); loseBackupReply = true; r = RunVirtual(job, hooks);
  check(!r.completed && !done && !r.errors.empty() && files.size()==1 &&
        files.at(destination).data=="old contents",
        "lost backup rename acknowledgement restores original after server commit");
  reset(); loseBackupReply = failRollback = true; r = RunVirtual(job, hooks);
  check(!r.completed && !done && !files.count(destination) && has(".previous") && errorHas(r,".previous"),
        "lost backup acknowledgement plus recovery failure reports retained original path");
  reset(); loseCommitReply = true; r = RunVirtual(job, hooks);
  check(!r.completed && !done && files.at(destination).data=="new contents" &&
        has(".previous") && errorHas(r,"not acknowledged") && errorHas(r,".previous"),
        "lost publication acknowledgement never overwrites new bytes with old backup");
  reset(); failCommit = true; r = RunVirtual(job, hooks);
  check(r.completed == 0 && !r.errors.empty() && done == 0 && files.size() == 1 &&
        files.at(destination).data == "old contents",
        "failed commit restores original and removes staged upload");
  reset();
  const std::string largeOriginal(2 * 1024 * 1024, 'o');
  files.at(destination).data = largeOriginal;
  failCommit = true;
  r = RunVirtual(job, hooks);
  check(commitFailed && !r.cancelled && !r.completed && !done && !files.count(destination) &&
            cleanupReadBytes <= 1024 * 1024 && reportedCopy(r, ".previous", largeOriginal) &&
            errorHas(r, ".previous"),
        "failed publication leaves oversized original at reported backup without a full reread");
  reset();
  failCommit = expireDuringCleanup = true;
  r = RunVirtual(job, hooks);
  check(commitFailed && !r.cancelled && !r.completed && !done && !files.count(destination) &&
            cleanupReadBytes == 0 && reportedCopy(r, ".previous", "old contents") &&
            reportedCopy(r, ".part", "new contents"),
        "expired cleanup deadline retains both recovery paths without further reads");
  for (bool afterBackup : {false, true}) {
    reset();
    cancelAtReadyCheck = !afterBackup;
    cancelAfterBackup = afterBackup;
    r = RunVirtual(job, hooks);
    check(r.cancelled && !r.completed && !done && postCancelReadCalls == 0,
          afterBackup ? "cancel after backup starts no further remote reads"
                      : "cancel after Ready starts no further remote reads");
    std::ifstream input(source);
    std::string retained(std::istreambuf_iterator<char>(input), {});
    const bool verifiedButUnpublished = r.outcomes.size() == 1 && r.outcomes.front().upload &&
        r.outcomes.front().upload->contentVerified &&
        r.outcomes.front().upload->publication == Publication::NotAttempted;
    const auto target = files.find(destination);
    const bool targetIntact = target != files.end() && target->second.data == "old contents";
    check(verifiedButUnpublished && retained == "new contents" && files.size() == 2 &&
              reportedCopy(r, ".part", retained) &&
              (afterBackup ? target == files.end() && reportedCopy(r, ".previous", "old contents")
                           : targetIntact),
          afterBackup ? "cancel after backup retains source and reports both recovery files"
                      : "cancel after Ready retains source and old target and reports staged bytes");
  }
  reset(); failCommit = failRollback = true; r = RunVirtual(job, hooks);
  check(r.completed == 0 && done == 0 && !files.count(destination) &&
        has(".previous") && !has(".part") && errorHas(r, ".previous"),
        "rollback failure retains original and reports exact recovery path");
  reset(); failBackupCleanup = true; r = RunVirtual(job, hooks);
  check(r.completed == 0 && done == 0 && files.at(destination).data == "new contents" &&
        has(".previous") && errorHas(r, "New file committed") && errorHas(r, ".previous"),
        "post-commit backup failure reports committed bytes without a false success event");
  reset(); failCommit = failPartCleanup = true; r = RunVirtual(job, hooks);
  check(r.completed == 0 && done == 0 && files.at(destination).data == "old contents" &&
        has(".part") && !has(".previous") && errorHas(r, ".part"),
        "staging cleanup failure reports retained temporary path after restoring original");
  reset(); mutateDestination = true; r = RunVirtual(job, hooks);
  check(r.completed == 0 && done == 0 && files.size() == 1 &&
        files.at(destination).data == "concurrent edit",
        "observable destination race leaves concurrent edit untouched");
  for (const char *edit : {"Old contents", "different length"}) {
    for (auto answer : {Decision::Replace, Decision::Skip, Decision::ReplaceAll}) {
      reset();
      Hooks edited = hooks;
      edited.conflict = [&](const std::string &target) {
        files.at(target).data = edit;
        return answer;
      };
      r = RunVirtual(job, edited);
      check(!r.completed && !done && files.size() == 1 && files.at(destination).data == edit &&
                (answer == Decision::Skip ? r.skipped == 1 && r.errors.empty()
                                          : errorHas(r, "Destination changed")),
            "upload preserves edits during the conflict even with unchanged size and time");
    }
  }
  reset(); hooks.conflict = [](const std::string &) { return Decision::Skip; };
  r = RunVirtual(job, hooks);
  check(r.completed == 0 && r.skipped == 1 && done == 0 &&
        files.at(destination).data == "old contents", "skipped upload is not completed");
  reset(); hooks.conflict = [](const std::string &) { return Decision::Abort; };
  r = RunVirtual(job, hooks);
  check(r.cancelled && done == 0 && files.size() == 1 &&
        files.at(destination).data == "old contents", "aborted conflict leaves original untouched");
  reset(); hooks.conflict = [](const std::string &) { return Decision::Replace; };
  r = RunVirtual({Action::Copy, {"sftp://FIXTURE/out/input"}, "sftp://fixture/out"}, hooks);
  check(!r.errors.empty() && !r.completed && files.size() == 1 &&
        files.at(destination).data == "old contents",
        "equivalent host casing cannot bypass self-copy rejection");
  reset(); r = RunVirtual({Action::Copy, {"sftp://fixture/out/%69nput"}, "sftp://fixture/out"}, hooks);
  check(!r.errors.empty() && !r.completed && files.at(destination).data == "old contents",
        "equivalent URL escaping cannot bypass self-copy rejection");
  reset(); r = RunVirtual({Action::Copy, {"sftp://fixture:22/out/input"}, "sftp://fixture/out"}, hooks);
  check(!r.errors.empty() && !r.completed && files.at(destination).data == "old contents",
        "explicit default port cannot bypass self-copy rejection");
  const std::string in = "sftp://fixture/in/selection";
  const std::string out = "sftp://fixture/out/selection";
  const std::string pickedFile = "sftp://fixture/input";
  const std::string work = "sftp://fixture/work";
  for (auto action : {Action::Copy, Action::Move}) {
    for (bool reverse : {false, true}) {
      for (bool occupied : {false, true}) {
        for (const auto &target : {work, work + "/sub"}) {
          reset();
          files.clear();
          files[pickedFile] = {"selected"};
          files[work] = files[work + "/sub"] = {"", 1, 0, true};
          if (occupied)
            files[target + "/input"] = {"keep"};
          const auto count = files.size();
          const auto alias = "sftp://FIXTURE:22/%77ork";
          Job overlap{action, reverse ? std::vector<std::string>{alias, pickedFile}
                                      : std::vector<std::string>{pickedFile, alias}, target};
          StatFile(pickedFile, overlap.expectedRemoteSources[pickedFile]);
          StatFile(work, overlap.expectedRemoteSources[alias]);
          r = RunVirtual(overlap, hooks);
          check(!r.completed && !r.partialCopies && !done && files.size() == count &&
                    errorHas(r, "contains the destination") && files.at(pickedFile).data == "selected" &&
                    files.at(work).directory && files.at(work + "/sub").directory &&
                    (occupied ? files.at(target + "/input").data == "keep"
                              : !files.count(target + "/input")),
                "overlap rejects the entire remote selection before copying or moving any root");
        }
      }
    }
  }
  reset();
  const std::string drafts = "sftp://fixture/drafts", other = work + "copy";
  files[drafts] = files[work] = files[other] = {"", 1, 0, true};
  files[work + "/one"] = {"first"};
  files[drafts + "/two"] = {"second"};
  r = RunVirtual({Action::Copy, {work, drafts}, other}, hooks);
  check(r.completed == 2 && r.errors.empty() && files.at(other + "/work/one").data == "first" &&
            files.at(other + "/drafts/two").data == "second" &&
            files.at(work + "/one").data == "first" && files.at(drafts + "/two").data == "second",
        "two remote folders copy to a distinct destination sharing a path prefix");
  for (bool mergeFolder : {false, true}) {
    reset();
    const auto target = std::string("sftp://fixture/out");
    const auto input = std::string("sftp://fixture/in/input");
    const auto selected = mergeFolder ? input + "/child" : input;
    if (mergeFolder)
      files[input] = files[destination] = {"", 1, 0, true};
    files[selected] = {"chosen bytes"};
    unreadableLookup = target + "/unselected-special";
    files[unreadableLookup] = {"untouched"};
    r = RunVirtual({Action::Copy, {input}, target}, hooks);
    check(r.completed == 1 && r.errors.empty() &&
              files.at(mergeFolder ? destination + "/child" : destination).data == "chosen bytes" &&
              files.at(unreadableLookup).data == "untouched",
          "existing SFTP target checks literal names without inspecting unrelated siblings");
  }
  auto fixtureTree = [] {
    std::map<std::string, std::pair<bool, std::string>> tree;
    for (const auto &[path, entry] : files)
      tree[path] = {entry.directory, entry.data};
    return tree;
  };
  for (bool collision : {false, true}) {
    reset();
    const std::string project = "sftp://fixture/project";
    const auto target = project + "/album", selection = target + "/album";
    const auto childName = collision ? "/album" : "/next";
    const auto child = selection + childName;
    files[project] = files[target] = files[selection] = files[child] = {"", 1, 0, true};
    files[selection + "/memo"] = {"selected"};
    files[child + "/memo"] = {"child data"};
    files[selection + "/z-last"] = {"end"};
    files[target + "/unrelated"] = {"old"};
    auto expected = fixtureTree();
    if (!collision) {
      expected[target + "/next"] = {true, ""};
      expected[target + "/next/memo"] = {false, "child data"};
      expected[target + "/memo"] = {false, "selected"};
      expected[target + "/z-last"] = {false, "end"};
    }
    int prompts = 0;
    Hooks observe = hooks;
    observe.conflict = [&](const auto &) { ++prompts; return Decision::Replace; };
    r = RunVirtual({Action::Copy, {selection}, project}, observe);
    check(r.completed == int(!collision) && !r.partialCopies && r.errors.empty() == !collision &&
              !prompts && fixtureTree() == expected,
          collision ? "upward SFTP merge rejects a projected child inside the source before delivery"
                    : "non-overlapping upward SFTP merge delivers all files without changing the source");
  }
  {
    const auto later = std::string(dir) + "/queued-later";
    std::ofstream(later) << "selected";
    Job queued{Action::Copy, {source, later}, "sftp://fixture/out"};
    for (const auto &path : queued.sources) {
      struct stat st;
      if (lstat(path.c_str(), &st))
        return 1;
      queued.expectedSources[path] = {int32_t(st.st_dev), int64_t(st.st_ino)};
    }
    std::filesystem::rename(later, later + ".original");
    std::ofstream(later) << "replacement";
    for (bool displayed : {false, true}) {
      reset();
      if (displayed)
        queued.displayedRemoteDestination = "fixture-parent";
      auto before = fixtureTree();
      r = RunVirtual(queued, hooks);
      check(!r.completed && !r.partialCopies && !statRequests && !mutationRequests &&
                errorHas(r, "Selected source changed") && r.outcomes.size() == 2 &&
                fixtureTree() == before && readBytes(later) == "replacement" &&
                readBytes(later + ".original") == "selected",
            "stale later local selection rejects the upload before SFTP destination checks");
    }
    reset();
    cancelled = true;
    r = RunVirtual(queued, hooks);
    check(r.cancelled && !r.completed && !statRequests && !mutationRequests,
          "cancelled upload does not inspect the replaced source or contact its destination");
    reset();
    InterruptedVolumes().Block(later);
    r = RunVirtual(queued, hooks);
    check(!r.completed && errorHas(r, kUnavailableVolumeText) && !statRequests && !mutationRequests,
          "blocked local source is rejected before identity lookup or SFTP I/O");
    unlink(later.c_str());
    unlink((later + ".original").c_str());
  }
  // A local daemon's /out may expose a directory with an unrelated local
  // prefix. A new output name, not just a replacement, must respect the
  // selection. Collect all sources before examining either projected root.
  const auto localTree = std::string(dir) + "/guarded-local";
  std::filesystem::create_directories(localTree + "/inner");
  std::ofstream(localTree + "/inner/memo") << "original";
  std::ofstream(localTree + "/inner/.hidden") << "preserve";
  for (bool reverse : {false, true}) {
    reset();
    peers.at("fixture").mayBeLocal = true;
    files.clear();
    files["sftp://fixture/out/memo"] = {"original"};
    omittedSftpName = "sftp://fixture/out/.hidden";
    files[omittedSftpName] = {"preserve"};
    const auto before = fixtureTree();
    const std::vector<std::string> selection = reverse
        ? std::vector<std::string>{localTree, source} : std::vector<std::string>{source, localTree};
    int prompts = 0;
    Hooks observed = hooks;
    observed.conflict = [&](const auto &) { ++prompts; return Decision::Replace; };
    r = RunVirtual({Action::Copy, selection, "sftp://fixture/out"}, observed);
    check(!r.completed && !r.partialCopies && !prompts && !mutationRequests &&
              errorHas(r, "selected local source") && fixtureTree() == before &&
              readBytes(localTree + "/inner/memo") == "original" &&
              readBytes(localTree + "/inner/.hidden") == "preserve",
          "a possible remapped local directory rejects every root before creating a new name");
  }
  for (bool sameBytes : {false, true}) {
    reset();
    peers.at("fixture").mayBeLocal = true;
    files.clear();
    const std::string remoteMemo = "sftp://fixture/out/memo";
    files[remoteMemo] = {sameBytes ? "original" : "separate"};
    int prompts = 0;
    Hooks observed = hooks;
    observed.conflict = [&](const auto &) { ++prompts; return Decision::Replace; };
    r = RunVirtual({Action::Copy, {localTree + "/inner/memo"}, "sftp://fixture/out"}, observed);
    check(!r.partialCopies && r.completed == int(!sameBytes) && prompts == int(!sameBytes) &&
              r.errors.empty() == !sameBytes && (mutationRequests != 0) == !sameBytes &&
              files.at(remoteMemo).data == "original",
          "local file conflict requires evidence that the actual replacement is outside the selection");
  }
  std::filesystem::remove_all(localTree);
  for (const std::string old : {"old", "old-version"}) {
    reset();
    peers.at("fixture").mayBeLocal = true;
    const auto backup = std::string(dir) + "/backup";
    std::filesystem::create_directory(backup);
    std::ofstream(backup + "/common.txt") << "shared";
    std::ofstream(backup + "/edit.txt") << "new-version";
    const std::string remoteBackup = "sftp://fixture/out/backup";
    files[remoteBackup] = {"", 1, 0, true};
    files[remoteBackup + "/common.txt"] = {"shared"};
    files[remoteBackup + "/edit.txt"] = {old};
    int prompts = 0;
    Hooks observed = hooks;
    observed.conflict = [&](const auto &) { ++prompts; return Decision::Replace; };
    r = RunVirtual({Action::Copy, {backup}, "sftp://fixture/out"}, observed);
    check(r.completed == 1 && r.errors.empty() && !r.partialCopies && prompts == 2 &&
              mutationRequests && files.at(remoteBackup + "/common.txt").data == "shared" &&
              files.at(remoteBackup + "/edit.txt").data == "new-version" &&
              readBytes(backup + "/common.txt") == "shared" &&
              readBytes(backup + "/edit.txt") == "new-version",
          "an independent backup can update an edited sibling beside a byte-identical file");
    std::filesystem::remove_all(backup);
  }
  for (const std::string scenario : {"new folder", "replacement", "safe first", "separate"}) {
    reset();
    peers.at("fixture").mayBeLocal = true;
    files.clear();
    const auto root = std::string(dir) + "/remapped";
    const auto tree = root + "/album/album";
    const std::string remoteTree = "sftp://fixture/out/album/album";
    const bool replacing = scenario == "replacement";
    const bool separate = scenario == "separate";
    const auto target = separate ? root + "/independent" : root;
    std::filesystem::create_directories(tree + "/album");
    files[remoteTree] = files[remoteTree + "/album"] = {"", 1, 0, true};
    if (replacing) {
      std::ofstream(tree + "/memo") << "selected-original";
      std::ofstream(tree + "/album/memo") << "inner-data";
      files[remoteTree + "/memo"] = {"selected-original"};
      files[remoteTree + "/album/memo"] = {"inner-data"};
    } else {
      std::filesystem::create_directory(tree + "/album/new-empty");
      files[remoteTree + "/album/new-empty"] = {"", 1, 0, true};
    }
    if (separate) {
      std::filesystem::create_directory(target);
      std::ofstream(target + "/marker") << "unrelated";
    }
    std::vector<std::string> selected{remoteTree};
    if (scenario == "safe first") {
      selected.insert(selected.begin(), "sftp://fixture/out/first");
      files[selected.front()] = {"safe bytes"};
    }
    localPublish = LocalPublish::Limited;
    int prompts = 0;
    Hooks observed = hooks;
    observed.conflict = [&](const auto &) { ++prompts; return Decision::Replace; };
    const auto before = fixtureTree();
    r = RunVirtual({Action::Copy, selected, target}, observed);
    localPublish = LocalPublish::Unexpected;
    check(!r.partialCopies && !mutationRequests && !prompts && fixtureTree() == before &&
              !std::filesystem::exists(tree + "/new-empty") &&
              !std::filesystem::exists(target + "/first") &&
              (!replacing || readBytes(tree + "/memo") == "selected-original") &&
              (separate ? r.completed == 1 && r.errors.empty() &&
                              std::filesystem::is_directory(target + "/album/album/new-empty") &&
                              readBytes(target + "/marker") == "unrelated"
                        : !r.completed && r.outcomes.size() == selected.size() &&
                              errorHas(r, "selected SFTP source")),
          ("remapped download preflight: " + scenario).c_str());
    std::filesystem::remove_all(root);
  }
  for (const bool overlap : {false, true}) {
    reset();
    peers.at("fixture").mayBeLocal = true;
    files.clear();
    const auto base = std::string(dir) + "/download-backup";
    const auto target = base + "/album";
    std::filesystem::create_directories(target);
    if (chmod(target.c_str(), 0700))
      return 1;
    std::ofstream(target + "/common.txt") << "shared";
    std::ofstream(target + "/edit.txt") << (overlap ? "new-version" : "old-version");
    const std::string remoteTree = "sftp://fixture/out/album";
    files[remoteTree] = {"", 1, 0, true};
    files[remoteTree + "/common.txt"] = {"shared"};
    files[remoteTree + "/edit.txt"] = {"new-version"};
    const auto before = fixtureTree();
    int prompts = 0;
    Hooks observed = hooks;
    observed.conflict = [&](const auto &) { ++prompts; return Decision::Replace; };
    localPublish = LocalPublish::Limited;
    r = RunVirtual({Action::Copy, {remoteTree}, base}, observed);
    localPublish = LocalPublish::Unexpected;
    check(r.completed == int(!overlap) && !r.partialCopies && prompts == (overlap ? 0 : 2) &&
              r.errors.empty() == !overlap && fixtureTree() == before && !mutationRequests &&
              readBytes(target + "/common.txt") == "shared" &&
              readBytes(target + "/edit.txt") == "new-version",
          overlap ? "identical download tree views still reject a possible source alias"
                  : "same-size edited downloaded sibling distinguishes an independent backup");
    std::filesystem::remove_all(base);
  }

  for (const bool upload : {false, true}) {
    for (const bool overlap : {false, true}) {
      reset();
      peers.at("fixture").mayBeLocal = true;
      files.clear();
      const auto base = std::string(dir) + "/fifo-copy";
      const auto localMemo = base + "/memo.txt";
      const auto localPipe = base + "/a.pipe";
      const std::string remoteMemo = "sftp://fixture/out/memo.txt";
      specialSftpName = "sftp://fixture/out/a.pipe";
      files[specialSftpName] = {};
      files[remoteMemo] = {(upload && !overlap) ? "old" : "new-version"};
      std::filesystem::create_directory(base);
      if (mkfifo(localPipe.c_str(), 0600))
        return 1;
      std::ofstream(localMemo) << ((upload || overlap) ? "new-version" : "old");
      const auto before = fixtureTree();
      int prompts = 0;
      Hooks observed = hooks;
      observed.conflict = [&](const auto &) { ++prompts; return Decision::Replace; };
      localPublish = LocalPublish::Limited;
      const Job memoJob = upload ? Job{Action::Copy, {localMemo}, "sftp://fixture/out"}
                                 : Job{Action::Copy, {remoteMemo}, base};
      r = RunVirtual(memoJob, observed);
      localPublish = LocalPublish::Unexpected;
      struct stat pipe;
      check(r.completed == int(!overlap) && !r.partialCopies && prompts == int(!overlap) &&
                r.errors.empty() == !overlap && !specialLookups && !specialReads &&
                (mutationRequests != 0) == (upload && !overlap) &&
                (!overlap || fixtureTree() == before) &&
                readBytes(localMemo) == "new-version" && files.at(remoteMemo).data == "new-version" &&
                !lstat(localPipe.c_str(), &pipe) && S_ISFIFO(pipe.st_mode),
            overlap ? "an unselected FIFO does not establish disjointness or permit overlap"
                    : "the selected file distinguishes independent folders without accessing a FIFO sibling");
      std::filesystem::remove_all(base);
    }
  }
  for (const bool upload : {false, true}) {
    for (const bool directory : {false, true}) {
      reset();
      peers.at("fixture").mayBeLocal = true;
      files.clear();
      const auto base = std::string(dir) + "/prefix-backup";
      const auto local = base + "/album";
      const auto localEdit = local + "/edit.bin";
      const std::string remoteOut = "sftp://fixture/out";
      const auto remote = directory ? remoteOut + "/album" : remoteOut;
      const auto remoteEdit = remote + "/edit.bin";
      std::filesystem::create_directories(local);
      if (chmod(local.c_str(), 0700))
        return 1;
      std::string prior(1024 * 1024 + 1, 'x'), edit = prior;
      edit.front() = 'y';
      std::ofstream(localEdit) << (upload ? edit : prior);
      files[remoteEdit] = {upload ? prior : edit};
      if (directory) {
        files[remote] = {"", 1, 0, true};
        files[remote + "/common.txt"] = {"shared"};
        std::ofstream(local + "/common.txt") << "shared";
      } else {
        unreadableSource = remote + "/a-extra.txt";
        files[unreadableSource] = {"old"};
        std::ofstream(local + "/a-extra.txt") << "old";
        if (chmod((local + "/a-extra.txt").c_str(), 0000))
          return 1;
      }
      auto selected = upload ? (directory ? local : localEdit) : (directory ? remote : remoteEdit);
      auto target = upload ? remoteOut : (directory ? base : local);
      localPublish = LocalPublish::Limited;
      r = RunVirtual({Action::Copy, {selected}, target}, hooks);
      localPublish = LocalPublish::Unexpected;
      const bool boundedEdit = std::find(prefixReads.begin(), prefixReads.end(),
          std::make_pair(remoteEdit, size_t(1024 * 1024))) != prefixReads.end();
      check(r.completed == 1 && r.errors.empty() && boundedEdit &&
                (directory || prefixReads.size() == 1) &&
                readBytes(localEdit) == edit && files.at(remoteEdit).data == edit,
            directory ? "a large changed file distinguishes separate selected folder trees"
                      : "the selected large-file prefix is read before an unreadable sibling");
      unreadableSource.clear();
      std::filesystem::remove_all(base);
    }
  }
  {
    reset();
    const auto local = std::string(dir) + "/equal-prefix";
    const auto localFile = local + "/memo.bin";
    const std::string remote = "sftp://fixture/out";
    const auto remoteFile = remote + "/memo.bin";
    std::filesystem::create_directory(local);
    std::string data(1024 * 1024 + 1, 'x');
    std::ofstream(localFile) << data;
    data.back() = 'y';
    files[remoteFile] = {data};
    LocalTransferSelection selection;
    selection.AddOutput(localFile, {});
    bool rejected = false;
    try {
      selection.CheckSourceFolder({"memo.bin"}, remote, "memo.bin", {});
    } catch (const std::runtime_error &error) {
      rejected = std::string_view(error.what()).find("Cannot distinguish") !=
                 std::string_view::npos;
    }
    check(rejected && prefixReads == decltype(prefixReads){{remoteFile, 1024 * 1024}} &&
              mutationRequests == 0 && readBytes(localFile).back() == 'x' &&
              files.at(remoteFile).data.back() == 'y',
          "an equal bounded prefix does not authorize a transfer");
    std::filesystem::remove_all(local);
  }
  for (const bool upload : {false, true}) {
    reset();
    const auto local = std::string(dir) + "/unstable-witness";
    const auto localFile = local + "/memo.txt";
    const std::string remote = "sftp://fixture/out";
    const auto remoteFile = remote + "/memo.txt";
    std::filesystem::create_directory(local);
    std::ofstream(localFile) << "old";
    files[remoteFile] = {"new"};
    LocalTransferSelection selection;
    FileEntry remoteEntry;
    StatFile(remoteFile, remoteEntry);
    if (upload)
      selection.Add(localFile, {}, {});
    else
      selection.AddOutput(localFile, {});
    afterPrefix = [&](const std::string &) {
      std::ofstream(local + "/replacement") << "new";
      std::filesystem::rename(local + "/replacement", localFile);
    };
    bool rejected = false;
    try {
      if (upload)
        selection.CheckParent({"memo.txt"}, remoteFile, &remoteEntry, {});
      else
        selection.CheckSourceFolder({"memo.txt"}, remote, "memo.txt", {});
    } catch (const std::runtime_error &error) {
      rejected = std::string_view(error.what()).find("changed during transfer preflight") !=
                 std::string_view::npos;
    }
    check(rejected && prefixReads.size() == 1 && mutationRequests == 0 &&
              readBytes(localFile) == "new" && files.at(remoteFile).data == "new",
          upload ? "read-only upload preflight rejects a changing local witness"
                 : "read-only download preflight rejects a changing local witness");
    afterPrefix = {};
    std::filesystem::remove_all(local);
  }
  {
    reset();
    const auto local = std::string(dir) + "/cached-witness";
    const auto localFile = local + "/memo.txt";
    const std::string remote = "sftp://fixture/out";
    const auto remoteFile = remote + "/memo.txt";
    std::filesystem::create_directory(local);
    std::ofstream(localFile) << "old";
    files[remoteFile] = {"new"};
    LocalTransferSelection selection;
    selection.AddOutput(localFile, {});
    selection.CheckSourceFolder({"memo.txt"}, remote, "", {});
    selection.RecheckWitnesses({});
    std::ofstream(local + "/replacement") << "edit";
    std::filesystem::rename(local + "/replacement", localFile);
    bool rejected = false;
    try {
      selection.CheckSourceFolder({"memo.txt"}, remote, "", {});
    } catch (const std::runtime_error &error) {
      rejected = std::string_view(error.what()).find("changed during transfer preflight") !=
                 std::string_view::npos;
    }
    bool rejectedAtFinish = false;
    try {
      selection.RecheckWitnesses({});
    } catch (const std::runtime_error &error) {
      rejectedAtFinish = std::string_view(error.what()).find("changed during transfer preflight") !=
                         std::string_view::npos;
    }
    check(rejected && rejectedAtFinish && prefixReads.size() == 1 && mutationRequests == 0 &&
              readBytes(localFile) == "edit" && files.at(remoteFile).data == "new",
          "read-only preflight cannot reuse the discarded revision of a cached witness");
    std::filesystem::remove_all(local);
  }
  for (const bool upload : {false, true}) {
    reset();
    const auto folder = std::string(dir) + "/indexed-names";
    const auto local = folder + "/memo";
    const std::string remote = "sftp://fixture/out";
    std::filesystem::create_directory(folder);
    std::ofstream(local) << "same";
    files[remote + "/memo"] = {"same"};
    LocalTransferSelection selection;
    if (upload)
      selection.Add(local, {}, {});
    else
      selection.AddOutput(local, {});
    std::ofstream(folder + "/later") << "appeared";
    FileEntry existing;
    StatFile(remote + "/memo", existing);
    bool refused = false;
    try {
      if (upload)
        selection.CheckParent({"memo", "later"}, remote + "/memo", &existing, {});
      else
        selection.CheckSourceFolder({"memo", "later"}, remote, "memo", {});
      selection.RecheckWitnesses({});
    } catch (const std::runtime_error &) {
      refused = true;
    }
    check(refused && mutationRequests == 0 && readBytes(local) == "same",
          "read-only preflight rejects a stale negative name index");
    std::filesystem::remove_all(folder);
  }
  for (const bool upload : {false, true}) {
    reset();
    peers.at("fixture").mayBeLocal = true;
    files.clear();
    const auto folder = std::string(dir) + "/skip-denied";
    const std::string remote = "sftp://fixture/out";
    std::filesystem::create_directory(folder);
    for (const std::string leaf : {"first", "second"}) {
      std::ofstream(folder + "/" + leaf) << (upload ? "new" : "old");
      files[remote + "/" + leaf] = {upload ? "old" : "new"};
    }
    if (upload)
      unreadableSource = remote + "/first";
    else if (chmod((folder + "/first").c_str(), 0000))
      return 1;
    const auto source = upload ? folder : remote;
    const Job batch{Action::Copy, {source + "/first", source + "/second"}, upload ? remote : folder};
    Hooks decisions = hooks;
    unsigned prompts = 0;
    decisions.conflict = [&](const auto &) { return ++prompts == 1 ? Decision::Skip : Decision::Replace; };
    localPublish = LocalPublish::Limited;
    r = RunVirtual(batch, decisions);
    localPublish = LocalPublish::Unexpected;
    chmod((folder + "/first").c_str(), 0600);
    check(r.completed == 1 && r.skipped == 1 && r.errors.empty() && prompts == 2 &&
              readBytes(folder + "/first") == (upload ? "new" : "old") &&
              files.at(remote + "/first").data == (upload ? "old" : "new") &&
              readBytes(folder + "/second") == "new" && files.at(remote + "/second").data == "new",
          "a stable sibling permits skipping an unreadable target and copying the next file");
    unreadableSource.clear();
    std::filesystem::remove_all(folder);
  }
  // The two routes have identical source/target path texts and the destination
  // contains an existing file. Authorization to replace is not namespace evidence.
  for (const std::string trial : {"same-key", "same-peer", "other-key-type", "unknown", "independent"}) {
    reset();
    peers["fixture"] = {"peer:22", "key-a", 1};
    if (trial != "unknown")
      peers["other"] = {trial == "same-peer" ? "peer:22" : "other:22",
                        trial == "same-key" ? "key-a" : "key-b",
                        trial == "other-key-type" ? 2 : 1};
    const auto sourceTree = std::string("sftp://other/out/album");
    const auto targetTree = std::string("sftp://fixture/out/album");
    files[sourceTree] = files[targetTree] = {"", 1, 0, true};
    files[sourceTree + "/memo"] = {"selected"};
    files[targetTree + "/memo"] = {"prior"};
    auto expected = fixtureTree();
    int prompts = 0;
    Hooks observe = hooks;
    observe.conflict = [&](const auto &) { ++prompts; return Decision::Replace; };
    r = RunVirtual({Action::Copy, {sourceTree}, "sftp://fixture/out"}, observe);
    if (trial == "independent") {
      expected[targetTree + "/memo"] = {false, "selected"};
      check(r.completed == 1 && !r.partialCopies && r.errors.empty() && prompts == 1 &&
                mutationRequests > 0 && fixtureTree() == expected,
            "independent authenticated peers support equal paths without changing source bytes");
    } else {
      check(!r.completed && !r.partialCopies && !r.errors.empty() && !prompts &&
                !mutationRequests && fixtureTree() == expected,
            "ambiguous or missing SFTP peer identity rejects before any mutation or conflict");
    }
  }
  for (auto action : {Action::Copy, Action::Move}) {
    for (bool reverse : {false, true}) {
      for (bool occupied : {false, true}) {
        reset();
        const std::string alias = "sftp://fixture/Archive";
        const auto target = alias + "/sub";
        files[alias] = files[target] = files[work] = files[work + "/sub"] = {"", 1, 0, true};
        files[pickedFile] = {"selected"};
        files[work + "/extra"] = {"preserve"};
        if (occupied)
          files[work + "/sub/input"] = {"keep"};
        canonicalAlias = alias;
        Job batch{action, reverse ? std::vector<std::string>{work, pickedFile}
                                  : std::vector<std::string>{pickedFile, work}, target};
        for (const auto &path : batch.sources)
          StatFile(path, batch.expectedRemoteSources[path]);
        const auto expected = fixtureTree();
        int prompts = 0;
        Hooks observe = hooks;
        observe.conflict = [&](const auto &) { ++prompts; return Decision::Replace; };
        r = RunVirtual(batch, observe);
        check(!r.completed && !r.partialCopies && !done && !prompts &&
                  errorHas(r, "Cannot establish canonical SFTP path") && fixtureTree() == expected,
              "a non-canonical ancestor in the SFTP destination blocks the entire transfer");
      }
    }
  }
  for (const auto &blockedPath : {in, in + "/second", out}) {
    reset();
    files[in] = files[out] = {"", 1, 0, true};
    files[in + "/second"] = {"not delivered"};
    unresolvedCanonical = blockedPath;
    const Job batch{Action::Copy, {source, in + "/second"},
                    blockedPath == out ? out : "sftp://fixture/out"};
    r = RunVirtual(batch, hooks);
    check(!r.completed && !r.partialCopies && !done && r.outcomes.size() == 2 &&
              r.outcomes[0].status == "unprocessed" && r.outcomes[1].status == "unprocessed" &&
              errorHas(r, "Canonical path unavailable") &&
              files.at(destination).data == "old contents" &&
              files.at(in + "/second").data == "not delivered" &&
              !files.count(out + "/input") && !files.count(out + "/second"),
          "unresolved SFTP identity blocks every selected output, including an earlier file");
  }
  reset();
  files[in] = files[in + "/fresh"] = {"", 1, 0, true};
  files[in + "/fresh/child"] = {"kept"};
  unresolvedCanonical = in + "/fresh/child";
  r = RunVirtual({Action::Copy, {source, in + "/fresh"}, "sftp://fixture/out"}, hooks);
  check(!r.completed && !r.partialCopies && !done && r.outcomes.size() == 2 &&
            errorHas(r, "Canonical path unavailable") &&
            files.at(destination).data == "old contents" &&
            files.at(in + "/fresh/child").data == "kept" &&
            !files.count("sftp://fixture/out/fresh"),
        "full SFTP preflight checks a later source child before creating a fresh output tree");
  for (bool fold : {false, true}) {
    reset();
    foldDirectoryNames = fold;
    files[in] = {"", 1, 0, true};
    files[in + "/Drafts"] = files[in + "/drafts"] = {"", 1, 0, true};
    files[in + "/Drafts/first"] = {"first"};
    files[in + "/drafts/second"] = {"second"};
    // The selected root merges into an existing folder in both modes.
    files[out] = {"", 1, 0, true};
    files[out + "/unrelated"] = {"existing"};
    r = RunVirtual({Action::Copy, {in}, "sftp://fixture/out"}, hooks);
    const bool kept = files.at(out + "/unrelated").data == "existing" &&
                      files.at(out + "/Drafts/first").data == "first";
    check(kept && (fold ?
              (!r.completed && r.partialCopies == 1 && !r.errors.empty() &&
               !files.count(out + "/drafts") && !files.count(out + "/Drafts/second") &&
               r.outcomes.size() == 2 && r.outcomes.front().destination == out + "/Drafts/first" &&
               r.outcomes.back().status == "partial") :
              (r.completed == 1 && r.errors.empty() && files.count(out + "/drafts") &&
               files.at(out + "/drafts/second").data == "second")),
          fold ? "case-insensitive target stops a folder collision and retains delivered receipt" :
                 "case-sensitive target preserves both folders and ordinary existing-root merge");
  }
  // Start a new runner for every retry, with no in-memory claims from the
  // earlier copy. A provider ID still cannot make a different spelling exact.
  for (bool reverse : {false, true}) {
    reset();
    foldDirectoryNames = true;
    const auto left = in + "/Drafts", right = in + "/drafts";
    files[in] = files[left] = files[right] = {"", 1, 0, true};
    files[left + "/one"] = {"first"};
    files[right + "/two"] = {"second"};
    const std::string parent = "sftp://fixture/out";
    const auto arrived = parent + "/Drafts";
    r = RunVirtual({Action::Copy, {left, right}, parent}, hooks);
    check(r.completed == 1 && r.errors.size() == 1 && files.at(arrived + "/one").data == "first" &&
              !files.count(arrived + "/two"), "first attempt refuses a directory alias");
    files[arrived].identity = "server-directory-id";
    Job retry{Action::Copy, reverse ? std::vector<std::string>{right, left} :
                                     std::vector<std::string>{right}, parent};
    r = RunVirtual(retry, hooks);
    check(!r.completed && r.errors.size() == 1 &&
              files.at(arrived + "/one").data == "first" && !files.count(arrived + "/two"),
          "reverse order or remaining-only retry refuses the differently spelled directory");
  }
  for (bool fold : {false, true}) {
    for (bool nestedNames : {false, true}) {
      reset();
      foldDirectoryNames = fold;
      const std::string container = "sftp://fixture/in/batch";
      const std::string target = "sftp://fixture/out";
      const auto output = nestedNames ? target + "/batch" : target;
      const auto old = container + "/first", upper = container + "/Report", lower = container + "/report";
      files[container] = {"", 1, 0, true};
      files[old] = {"replace old target"};
      files[upper] = {"first report"};
      files[lower] = {"second report"};
      files[output] = {"", 1, 0, true};
      files[output + "/first"] = {"old"};
      Hooks replace = hooks;
      int questions = 0;
      replace.conflict = [&](const auto &) { ++questions; return Decision::ReplaceAll; };
      Job batch{Action::Copy, nestedNames ? std::vector<std::string>{container} :
                                          std::vector<std::string>{old, upper, lower}, target};
      r = RunVirtual(batch, replace);
      const bool separate = !fold;
      check(r.completed == (nestedNames ? int(separate) : 2 + int(separate)) &&
                r.errors.size() == size_t(fold) && questions == 1 &&
                files.at(output + "/Report").data == "first report" &&
                files.at(upper).data == "first report" && files.at(lower).data == "second report" &&
                (separate ? files.at(output + "/report").data == "second report" :
                            !files.count(output + "/report")),
            "Replace All protects earlier file deliveries including children, without blocking distinct names");
      if (fold) {
        r = RunVirtual({Action::Copy, {lower}, output}, replace);
        check(!r.completed && r.errors.size() == 1 && questions == 1 &&
                  files.at(output + "/Report").data == "first report",
              "retry of just the remaining file cannot replace a differently spelled delivered file");
      }
    }
  }
  foldDirectoryNames = false;
  const auto folder = std::string(dir) + "/upload-folder";
  const auto blocked = folder + "/unavailable";
  const auto sibling = folder + "/sibling";
  const std::string remoteFolder = "sftp://fixture/out/upload-folder";
  const auto remoteSibling = remoteFolder + "/sibling";
  if (mkdir(folder.c_str(), 0700) || mkdir(blocked.c_str(), 0700))
    return 1;
  InterruptedVolumes().Block(blocked);
  reset();
  r = RunVirtual({Action::Copy, {blocked}, "sftp://fixture/out"}, hooks);
  check(!r.completed && !r.partialCopies && !done && files.size() == 1 &&
            errorHas(r, blocked.c_str()),
        "upload refuses a registered unavailable root without publishing anything");
  const Job folderJob{Action::Copy, {folder}, "sftp://fixture/out"};
  r = RunVirtual(folderJob, hooks);
  check(!r.completed && !r.partialCopies && !done && files.size() == 1 &&
            errorHas(r, blocked.c_str()),
        "folder with only unavailable children is not uploaded as an empty success");
  std::ofstream(sibling) << "healthy sibling";
  r = RunVirtual(folderJob, hooks);
  const auto hasReceipt = [&](const Result &result) {
    for (const auto &entry : result.outcomes)
      if (entry.source == sibling && entry.destination == remoteSibling &&
          entry.status == "completed" && entry.upload &&
          entry.upload->contentVerified &&
          entry.upload->publication == Publication::Published)
        return true;
    return false;
  };
  check(!r.completed && r.partialCopies == 1 && !r.cancelled && done == 1 &&
            errorHas(r, blocked.c_str()) && hasReceipt(r) &&
            files.count(remoteSibling) && files.at(remoteSibling).data == "healthy sibling" &&
            !files.count(remoteFolder + "/unavailable"),
        "partial folder upload retains verified sibling receipts but never completes the root");
  reset();
  Hooks reporting = hooks;
  reporting.report = std::make_shared<JobReportStore>();
  r = RunVirtual(folderJob, reporting);
  const auto uploadReport = reporting.report->Page(0, 10);
  check(!r.completed && r.partialCopies == 1 && done == 1 &&
            uploadReport.errors.size() == 1 &&
            uploadReport.errors.front().find(blocked) != std::string::npos &&
            uploadReport.outcomes.size() == 3 &&
            uploadReport.outcomes[0].source == blocked &&
            uploadReport.outcomes[0].status == "unprocessed" &&
            uploadReport.outcomes[1].source == sibling &&
            uploadReport.outcomes[1].status == "completed" &&
            uploadReport.outcomes[1].detail.find("Staged contents verified.") != std::string::npos &&
            uploadReport.outcomes[1].detail.find("Destination published.") != std::string::npos &&
            uploadReport.outcomes[2].source == folder &&
            uploadReport.outcomes[2].status == "partial",
        "queue-style report distinguishes the omitted child, delivered sibling and partial root");
  const auto nested = folder + "/nested";
  const auto nestedBlocked = nested + "/other-unavailable";
  if (rmdir(blocked.c_str()) || mkdir(nested.c_str(), 0700) ||
      mkdir(nestedBlocked.c_str(), 0700))
    return 1;
  InterruptedVolumes().Block(nestedBlocked);
  reset();
  r = RunVirtual(folderJob, hooks);
  check(!r.completed && r.partialCopies == 1 && done == 1 && hasReceipt(r) &&
            errorHas(r, nestedBlocked.c_str()) &&
            !files.count(remoteFolder + "/nested"),
        "nested omissions propagate to the selected folder without hiding healthy receipts");
  rmdir(nestedBlocked.c_str());
  rmdir(nested.c_str());
  reset();
  const auto arrived = folder + "/new.txt";
  Hooks changing = hooks;
  changing.fileDone = [&](const std::string &path, const std::string &) {
    ++done;
    if (path == sibling)
      std::ofstream(arrived) << "new arrival";
  };
  r = RunVirtual(folderJob, changing);
  check(!r.completed && r.partialCopies == 1 && done == 1 && hasReceipt(r) &&
            errorHas(r, "Source folder changed") && files.count(remoteSibling) &&
            !files.count(remoteFolder + "/new.txt") && access(arrived.c_str(), F_OK) == 0,
        "concurrent source addition retains delivered child but suppresses root completion");
  unlink(arrived.c_str());

  for (const bool nestedSwap : {false, true}) {
    for (const bool preexisting : {false, true}) {
      reset();
      giveCreatedFoldersIds = true;
      const auto localParent = nestedSwap ? nested : folder;
      if (nestedSwap && mkdir(nested.c_str(), 0700))
        return 1;
      const auto firstLocal = localParent + "/sibling";
      const auto secondLocal = localParent + "/another";
      std::ofstream(firstLocal) << "healthy sibling";
      std::ofstream(secondLocal) << "another";
      const std::string serverOut = "smb://fixture/out";
      const auto remoteRoot = serverOut + "/upload-folder";
      const auto remoteParent = nestedSwap ? remoteRoot + "/nested" : remoteRoot;
      const auto moved = serverOut + "/renamed-original";
      if (preexisting) {
        files[remoteRoot] = {"", 1, 0, true, "root"};
        files[remoteParent] = {"", 1, 0, true, "original"};
      }
      std::string delivered;
      auto swap = hooks;
      swap.fileDone = [&](const auto &, const auto &path) {
        if (!delivered.empty() || ParentLocation(path) != remoteParent)
          return;
        delivered = LocationLeaf(path);
        files[moved] = files.at(remoteParent);
        files[moved + "/" + delivered] = files.at(path);
        files.erase(path);
        files[remoteParent].identity = "replacement";
      };
      Job upload{Action::Copy, {folder}, serverOut};
      r = RunVirtual(upload, swap);
      check(!delivered.empty() && r.completed == 0 && r.partialCopies == 1 &&
                errorHas(r, remoteParent.c_str()) &&
                std::any_of(r.outcomes.begin(), r.outcomes.end(), [&](const auto &entry) {
                  return entry.destination == remoteParent + "/" + delivered && entry.upload &&
                         entry.upload->publication == Publication::Published;
                }) && files.at(moved + "/" + delivered).data ==
                         (delivered == "another" ? "another" : "healthy sibling") &&
                !files.count(remoteParent + "/sibling") && !files.count(remoteParent + "/another") &&
                access(firstLocal.c_str(), F_OK) == 0 && access(secondLocal.c_str(), F_OK) == 0,
            preexisting ? "existing SMB upload folder changed: next write refused, delivery receipt retained" :
                          "new SMB upload folder changed: next write refused, delivery receipt retained");
      r = RunVirtual(upload, hooks);
      check(r.completed == 1 && files.at(remoteParent + "/sibling").data == "healthy sibling" &&
                files.at(remoteParent + "/another").data == "another" && files.count(moved + "/" + delivered),
            "fresh SMB upload retry binds the current folder and preserves the first delivery");
      unlink(secondLocal.c_str());
      if (nestedSwap) {
        unlink(firstLocal.c_str());
        rmdir(nested.c_str());
      }
    }
  }
  // Select the folder, both children, or a single child on the same share.
  const std::string serverOut = "smb://fixture/out";
  const auto secondLocal = folder + "/another";
  const auto uploadFolder = serverOut + "/upload-folder";
  std::ofstream(secondLocal) << "another";
  for (bool stable : {false, true}) {
    for (bool swap : {false, true}) {
      for (unsigned selection : {0u, 1u, 2u}) {
        reset();
        giveCreatedFoldersIds = stable;
        const auto parent = selection == 0 ? uploadFolder : serverOut;
        files[serverOut] = {"", 1, 0, true, stable ? "share" : ""};
        const auto aside = serverOut + "/aside";
        Job upload{Action::Copy, {sibling}, serverOut};
        if (!selection)
          upload.sources = {folder};
        else if (selection == 2)
          upload.sources.push_back(secondLocal);
        auto observe = hooks;
        std::string firstName;
        observe.fileDone = [&](const auto &path, const auto &target) {
          if (path == folder || !firstName.empty())
            return;
          firstName = LocationLeaf(target);
          if (swap) {
            files[aside] = files.at(parent);
            files[aside + "/" + firstName] = files.at(target);
            files.erase(target);
            files[parent] = {"", 1, 0, true, stable ? "replacement" : ""};
          }
        };
        r = RunVirtual(upload, observe);
        const auto remaining = firstName == "sibling" ? "another" : "sibling";
        const bool stopped = stable && swap && selection != 1;
        size_t receipts = 0;
        bool qualified = true;
        for (const auto &entry : r.outcomes)
          if (entry.upload && entry.upload->publication == Publication::Published) {
            ++receipts;
            qualified &= entry.upload->contentVerified &&
                (entry.detail.find("no stable identity") != std::string::npos) == !stable;
          }
        check(!firstName.empty() && qualified && r.copyLimitations == !stable &&
                  receipts == (selection == 1 || stopped ? 1u : 2u) &&
                  r.completed == (stopped ? int(selection == 2) : int(upload.sources.size())) &&
                  r.errors.empty() == !stopped &&
                  files.at((swap ? aside : parent) + "/" + firstName).data ==
                      (firstName == "sibling" ? "healthy sibling" : "another") &&
                  (bool(files.count(parent + "/" + remaining)) == (!stopped && selection != 1)) &&
                  readBytes(sibling) == "healthy sibling" && readBytes(secondLocal) == "another",
              "upload receipt distinguishes verified bytes from weak folder binding for each selection shape");
      }
    }
  }
  unlink(secondLocal.c_str());

  const std::string firstMove = "smb://fixture/in/one", nextMove = "smb://fixture/in/two";
  auto moveSelection = [&] {
    files[firstMove] = {"one", 1, 0, false, "one-id"};
    files[nextMove] = {"two", 1, 0, false, "two-id"};
    files[serverOut] = {"", 1, 0, true, "original-folder"};
    Job move{Action::Move, {firstMove, nextMove}, serverOut};
    for (const auto &path : move.sources)
      StatFile(path, move.expectedRemoteSources[path]);
    return move;
  };
  for (auto action : {Action::Move, Action::Copy}) {
    reset();
    auto pending = moveSelection();
    pending.action = action;
    pending.sources = {action == Action::Move ? firstMove : source};
    pending.displayedRemoteDestination = "previous-view";
    const auto output = ChildLocation(serverOut, LocationLeaf(pending.sources.front()));
    r = RunVirtual(pending, hooks);
    check(r.completed == 0 && errorHas(r, "destination folder changed") &&
              files.at(firstMove).data == "one" && !files.count(output),
          "stale queued destination is refused before the first transfer");
    pending.displayedRemoteDestination = files.at(serverOut).identity;
    r = RunVirtual(pending, hooks);
    check(r.completed == 1 && r.errors.empty() &&
              files.at(output).data == (action == Action::Move ? "one" : "new contents"),
          "new selection of the refreshed target can be transferred normally");
  }
  for (bool swap : {false, true}) {
    reset();
    auto move = moveSelection();
    const auto movedAside = "smb://fixture/aside/one";
    auto moveHooks = hooks;
    moveHooks.fileDone = [&](const auto &path, const auto &dst) {
      if (swap && path == firstMove) {
        files[movedAside] = files.at(dst);
        files.erase(dst);
        files[serverOut].identity = "replacement";
      }
    };
    r = RunVirtual(move, moveHooks);
    check(r.completed == (swap ? 1 : 2) && r.completedRoots.size() == size_t(r.completed) &&
              !files.count(firstMove) && bool(files.count(nextMove)) == swap &&
              files.at(swap ? movedAside : serverOut + "/one").data == "one" &&
              (swap ? (!files.count(serverOut + "/one") && !files.count(serverOut + "/two") &&
                       errorHas(r, "Remote destination folder changed")) :
                      (files.at(serverOut + "/two").data == "two" && r.errors.empty())),
          "network move binds all selected entries to one target folder and reports only confirmed moves");
    if (swap) {
      move.sources = {nextMove};
      r = RunVirtual(move, hooks);
      check(r.completed == 1 && !files.count(nextMove) && !files.count(firstMove) &&
                files.at(movedAside).data == "one" && files.at(serverOut + "/two").data == "two" &&
                !files.count(serverOut + "/one"),
            "explicit retry of just the retained selection binds the refreshed target");
    }
  }
  reset();
  auto finalMove = moveSelection();
  finalMove.sources = {firstMove};
  swapDuringMove = firstMove;
  r = RunVirtual(finalMove, hooks);
  check(!r.completed && r.completedRoots.empty() && !r.errors.empty() && r.outcomes.size() == 1 &&
            r.outcomes[0].status == "move destination unconfirmed" &&
            !files.count(firstMove) && !files.count(serverOut + "/one") &&
            files.at("smb://fixture/aside/one").data == "one",
        "even the final acknowledged move cannot confirm publication in a replaced target folder");
  reset();
  auto weakMove = moveSelection();
  files[firstMove].identity.clear();
  StatFile(firstMove, weakMove.expectedRemoteSources[firstMove]);
  weakMove.sources = {firstMove, firstMove};
  lostMove = firstMove;
  r = RunVirtual(weakMove, hooks);
  check(!r.completed && r.errors.size() == 2 && r.outcomes.front().status == "move unconfirmed" &&
            lostMoveRequests == 1 && files.at(firstMove).data == "one" && !files.count(serverOut + "/one"),
        "lost reply without stable source identity never retries an ambiguous destructive request");
  for (auto action : {Action::Move, Action::Rename}) {
    for (bool applied : {false, true}) {
      for (bool canCheck : {false, true}) {
        reset();
        auto move = moveSelection();
        move.action = action;
        const auto dst = serverOut + (action == Action::Rename ? "/renamed" : "/one");
        if (action == Action::Rename) {
          move.sources = {firstMove};
          move.destination = dst;
        }
        lostMove = firstMove;
        moveBeforeLoss = applied;
        loseMoveLookup = !canCheck;
        r = RunVirtual(move, hooks);
        const auto &row = r.outcomes.front();
        check(lostMoveRequests == 1 && row.source == firstMove && row.destination == dst &&
                  row.status == (!canCheck ? "move unconfirmed" : applied ? "moved" : "not moved") &&
                  r.completed == int(action == Action::Move) + int(canCheck && applied) &&
                  r.completedRoots.size() == size_t(r.completed) &&
                  r.errors.empty() == (canCheck && applied) && bool(files.count(dst)) == applied &&
                  bool(files.count(firstMove)) != applied &&
                  (action != Action::Move || (!files.count(nextMove) && files.at(serverOut + "/two").data == "two")),
              "lost rename/move reply reconciles both paths or records an unknown outcome without replay");
        unreadableLookup.clear();
        lostMove.clear();
        move.sources = {firstMove};
        r = RunVirtual(move, hooks);
        check(r.completed == int(!applied) && !files.count(firstMove) && files.at(dst).data == "one",
              "explicit retry only moves a still-present selection to an unused name");
      }
    }
  }
  for (unsigned count : {8u, 64u}) {
    reset();
    files[out] = {"", 1, 0, true, "root-id"};
    files[in] = {"", 1, 0, true};
    for (unsigned i = 0; i < count; ++i) {
      const auto suffix = "/sub" + std::to_string(i);
      files[in + suffix] = {"", 1, 0, true};
      files[out + suffix] = {"", 1, 0, true, "id" + suffix};
    }
    r = RunVirtual({Action::Copy, {in}, "sftp://fixture/out"}, hooks);
    check(r.completed == 1 && !r.copyLimitations && listingCounts.at(out) == 1 && listingRows.at(out) == count,
          (std::to_string(count) + " stable destination siblings require one exact-name listing").c_str());
    files[out + "/sub0"].identity.clear();
    r = RunVirtual({Action::Copy, {in}, "sftp://fixture/out"}, hooks);
    check(r.completed == 1 && r.copyLimitations && !r.outcomes.empty() &&
              r.outcomes.back().detail.find(out + "/sub0") != std::string::npos,
          "nested folder without an identity explicitly qualifies the selected root receipt");
  }
  for (const bool explicitSource : {false, true}) {
    reset();
    const std::string implicit = "sftp://fixture", explicitPort = "sftp://fixture:22";
    const auto from = (explicitSource ? explicitPort : implicit) + "/in/item";
    const auto to = (explicitSource ? implicit : explicitPort) + "/out";
    files[from] = {"selected"};
    Job move{Action::Move, {from}, "sftp://fixture:23/out"};
    StatFile(from, move.expectedRemoteSources[from]);
    r = RunVirtual(move, hooks);
    check(!r.completed && !r.errors.empty() && files.count(from), "different SFTP ports cannot move");
    move.destination = to;
    r = RunVirtual(move, hooks);
    check(r.completed == 1 && !files.count(from) && files.at(to + "/item").data == "selected",
          "worker moves across implicit and explicit standard SFTP ports");
  }
  reset();
  files[remote] = {"private", 1, 0, false, "private-file", 0644};
  tightenOnRead = remote;
  localPublish = LocalPublish::Limited;
  mkdir(downloadFolder.c_str(), 0700);
  std::ofstream(localTarget) << "keep";
  r = RunVirtual(downloadJob, hooks);
  check(!r.completed && errorHas(r, "Source changed") && files.at(remote).permissions == 0600 &&
            std::ifstream(localTarget).peek() == 'k',
        "permission change during download leaves previous destination intact");
  unlink(localTarget.c_str());
  rmdir(downloadFolder.c_str());

  for (const std::string server : {"sftp://fixture", "smb://fixture"}) {
    const auto picked = server + "/in/selected";
    const auto other = server + "/in/other";
    const auto newName = server + "/out/renamed";
    reset();
    files[picked] = {"before", 1, 0, false, "object-A"};
    Job rename{Action::Rename, {picked}, newName};
    StatFile(picked, rename.expectedRemoteSources[picked]);
    files[picked].identity = "object-B";
    r = RunVirtual(rename, hooks);
    check(!r.completed && !done && errorHas(r, "changed or disappeared") &&
              files.count(picked) && !files.count(newName),
          "rename rejects replacement with matching size/time but different server identity");
    Job copySelected{Action::Copy, {picked}, server + "/out"};
    copySelected.expectedRemoteSources = rename.expectedRemoteSources;
    r = RunVirtual(copySelected, hooks);
    check(!r.completed && !done && errorHas(r, "changed before copying") &&
              files.count(picked) && !files.count(server + "/out/selected"),
          "queued panel copy refuses to publish a replacement for its selected source");
    rename.expectedRemoteSources[picked].providerIdentity.clear();
    files[picked].data = "longer replacement";
    r = RunVirtual(rename, hooks);
    check(!r.completed && !done && errorHas(r, "changed or disappeared") &&
              files.at(picked).data == "longer replacement" && !files.count(newName),
          "rename rejects changed size without a stable provider ID");
    files.erase(picked);
    r = RunVirtual(rename, hooks);
    check(!r.completed && !done && errorHas(r, "changed or disappeared") && !files.count(newName),
          "rename rejects a vanished selection");

    files[picked] = {"selected", 1, 0, false, "id-1"};
    files[other] = {"other", 1, 0, false, "id-2"};
    Job move{Action::Move, {picked, other}, server + "/out"};
    StatFile(picked, move.expectedRemoteSources[picked]);
    StatFile(other, move.expectedRemoteSources[other]);
    files[other].identity = "changed";
    r = RunVirtual(move, hooks);
    check(r.completed == 1 && done == 1 && errorHas(r, "changed or disappeared") &&
              files.at(server + "/out/selected").data == "selected" && !files.count(picked) &&
              files.count(other) && !files.count(server + "/out/other"),
          "queued multi-selection move checks each source against its own captured revision");
    Job retry{Action::Rename, {other}, newName};
    r = RunVirtual(retry, hooks);
    check(!r.completed && errorHas(r, "No confirmed network entry") && files.count(other),
          "remote rename cannot substitute a fresh lookup for a missing selection");
    StatFile(other, retry.expectedRemoteSources[other]);
    r = RunVirtual(retry, hooks);
    check(r.completed == 1 && r.errors.empty() && !files.count(other) &&
              files.at(newName).data == "other" && files.at(server + "/out/selected").data == "selected",
          "rename after fresh selection succeeds without changing the earlier delivered entry");
  }

  const std::string tree = "sftp://fixture/in/selection";
  const auto first = tree + "/a";
  const auto second = tree + "/b";
  reset();
  files[tree] = {"old file"};
  Job deletion{Action::Delete, {tree}, ""};
  FileEntry selected;
  if (!StatFile(tree, selected))
    return 1;
  deletion.expectedRemoteSources[tree] = selected;
  files[tree].nanoseconds = 500;
  r = RunVirtual(deletion, hooks);
  check(!r.completed && errorHas(r, "changed before deletion") && files.count(tree) &&
            r.outcomes.empty(),
        "queued deletion preserves a same-size file with a changed revision");
  files[tree] = Remote{"", 1, 0, true};
  files[first] = {"keep this"};
  files[second] = {"keep this too"};
  r = RunVirtual(deletion, hooks);
  check(errorHas(r, "changed before deletion") && !r.completed &&
            r.outcomes.empty() && files.count(tree) && files.count(first) && files.count(second),
        "queued file deletion cannot expand to a replacement tree");
  r = RunVirtual(Job{Action::Delete, {tree}, ""}, hooks);
  check(!r.completed && !r.errors.empty() && r.outcomes.empty() && files.count(tree) &&
            files.count(first) && files.count(second),
        "a network delete missing its confirmed selection fails closed");
  if (!StatFile(tree, deletion.expectedRemoteSources.at(tree)))
    return 1;
  for (bool cancel : {true, false}) {
    files[first] = {"remove first"};
    Hooks interrupt = hooks;
    interrupt.report = std::make_shared<JobReportStore>();
    interrupt.proceed = [&] { return !cancel || files.count(first); };
    refuseRemoval = cancel ? "" : second;
    r = RunVirtual(deletion, interrupt);
    const auto rows = interrupt.report->Page(0, 10);
    check(!r.completed && r.cancelled == cancel && r.partialDeletions == 1 &&
              rows.errors.size() == (cancel ? 0u : 1u) && !files.count(first) &&
              files.count(second) && files.count(tree) && rows.outcomes.size() == (cancel ? 2u : 3u) &&
              (cancel || (rows.outcomes[1].source == second && rows.outcomes[1].status == "deletion unconfirmed")) &&
              rows.outcomes[0].source == first && rows.outcomes[0].status == "deleted" &&
              rows.outcomes.back().source == tree && rows.outcomes.back().status == "partially deleted",
          cancel ? "cancelled deletion reports acknowledged child and partial root"
                 : "failed deletion reports acknowledged child and partial root");
  }
  refuseRemoval.clear();
  r = RunVirtual(deletion, hooks);
  check(r.completed == 1 && !r.partialDeletions && r.errors.empty() && !files.count(second) &&
            !files.count(tree) && r.outcomes.size() == 2 &&
            r.outcomes.back().source == tree && r.outcomes.back().status == "deleted",
        "completed network deletion reports each acknowledged removal");
  for (bool nested : {false, true}) {
    for (bool afterChild : {false, true}) {
      reset();
      const std::string selectedTree = "smb://fixture/in/tree";
      const auto folder = nested ? selectedTree + "/branch" : selectedTree;
      const auto child = folder + "/item";
      files[selectedTree] = {"", 1, 0, true, "tree"};
      files[folder] = {"", 1, 0, true, "folder"};
      files[child] = {"bytes"};
      Job remove{Action::Delete, {selectedTree}, ""};
      StatFile(selectedTree, remove.expectedRemoteSources[selectedTree]);
      (afterChild ? changedAfterRemove : changedAfterList) = folder;
      r = RunVirtual(remove, hooks);
      check(!r.completed && errorHas(r, "Folder changed during deletion") &&
                files.count(selectedTree) && files.at(folder).identity == "new-folder" &&
                (afterChild ? !files.count(child) && r.partialDeletions == 1 &&
                              r.outcomes.front().source == child && r.outcomes.front().status == "deleted"
                            : files.at(child).data == "bytes" && r.outcomes.empty()),
            "recursive delete refuses an identity mismatch at listing or final removal");
      changedAfterList.clear();
      changedAfterRemove.clear();
      StatFile(selectedTree, remove.expectedRemoteSources[selectedTree]);
      r = RunVirtual(remove, hooks);
      check(r.completed == 1 && r.errors.empty() && !files.count(selectedTree) && !files.count(child),
            "newly selected network folder can be removed after reloading");
    }
  }
  for (bool firstLost : {false, true}) {
    for (bool unknown : {false, true}) {
      reset();
      files[tree] = {"", 1, 0, true, "tree"};
      files[first] = {"one", 1, 0, false, "first"};
      files[second] = {"two", 1, 0, false, "second"};
      StatFile(tree, deletion.expectedRemoteSources[tree]);
      lostRemoval = firstLost ? first : second;
      loseLookupAfterRemoval = unknown;
      Job retry{Action::Delete, {lostRemoval}, ""};
      StatFile(lostRemoval, retry.expectedRemoteSources[lostRemoval]);
      r = RunVirtual(deletion, hooks);
      const auto hasRow = [&](const auto &path, const auto &status) {
        return std::any_of(r.outcomes.begin(), r.outcomes.end(), [&](const EntryOutcome &entry) {
          return entry.source == path && entry.status == status;
        });
      };
      check(!r.completed && !r.errors.empty() && !files.count(lostRemoval) &&
                r.unconfirmedDeletions == int(unknown) && r.partialDeletions == int(!unknown || !firstLost) &&
                hasRow(lostRemoval, unknown ? "deletion unconfirmed" : "deleted") &&
                (firstLost ? (hasRow(second, "unprocessed") && files.count(second)) :
                             (hasRow(first, "deleted") && !files.count(first))),
            "lost removal reply distinguishes confirmed absence, unknown outcome and untouched siblings");
      unreadableLookup.clear();
      files[lostRemoval] = {"new", 1, 0, false, "replacement"};
      r = RunVirtual(retry, hooks);
      check(!r.completed && files.at(lostRemoval).identity == "replacement",
            "replaying original selection never deletes replacement after failed removal");
    }
  }
  unlink(sibling.c_str());
  rmdir(folder.c_str());
  unlink(source.c_str()); rmdir(dir);
  reset();
  files.clear();
  const std::string leftCompare = "sftp://fixture/in";
  const std::string rightCompare = "sftp://fixture/out";
  files[leftCompare + "/equal"] = files[rightCompare + "/equal"] = {"same"};
  files[leftCompare + "/other"] = {"left"};
  files[rightCompare + "/other"] = {"else"};
  files[leftCompare + "/read-error"] = files[rightCompare + "/read-error"] = {"read me"};
  files[leftCompare + "/different-size"] = {"abc"};
  files[rightCompare + "/different-size"] = {"abcde"};
  for (bool checksum : {false, true}) {
    const CompareRequest comparison{leftCompare, rightCompare, checksum};
    auto compared = CompareFolders(comparison);
    check(compared.complete && compared.verified == (checksum ? 2u : 0u) &&
              compared.sizeOnly == (checksum ? 0u : 3u) &&
              compared.different == (checksum ? 2u : 1u) && !compared.unverified &&
              SelectCompareRows(compared, false, 10).total == compared.different,
          "stable comparison retains confirmed size and checksum results");
    // Keep a real per-row read diagnostic when final validation also fails.
    if (checksum)
      unreadableSource = leftCompare + "/read-error";
    for (bool cancelFinal : {false, true}) {
      const auto scansBefore = listingCounts[leftCompare];
      TransferControl control{[&] {
        if (listingCounts[leftCompare] <= scansBefore + 1)
          return true;
        if (!cancelFinal)
          throw std::runtime_error("Fixture final comparison read failed");
        return false;
      }};
      compared = CompareFolders(comparison, control);
      const auto visible = SelectCompareRows(compared, false, 10);
      const bool rowsInvalidated = std::all_of(
          compared.entries.begin(), compared.entries.end(), [&](const auto &entry) {
            if (entry.leftBytes != -1 || entry.rightBytes != -1 ||
                !entry.leftHash.empty() || !entry.rightHash.empty())
              return false;
            if (checksum && entry.relative == "read-error")
              return entry.state == CompareState::Error && entry.detail == "Source read denied";
            return entry.state == CompareState::Unverified;
          });
      check(!compared.complete && compared.cancelled == cancelFinal &&
                !compared.verified && !compared.sizeOnly && !compared.different &&
                compared.unverified == 4 && compared.entries.size() == 4 && rowsInvalidated &&
                visible.total == 4 && visible.indices.size() == 4 &&
                compared.error.find("provisional") != std::string::npos,
            "failed or cancelled final checks revoke sizes and hashes and expose every row");
    }
    unreadableSource.clear();
  }
  std::cout << checks << " fault checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
