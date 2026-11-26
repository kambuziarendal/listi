// SPDX-License-Identifier: MIT
// Compile on POSIX: c++ -std=c++17 -Isrc tests/virtual-operation-faults.cpp
// src/JobReportStore.cpp src/DirectoryFinalization.cpp src/VirtualOperation.cpp
// src/VirtualFile.cpp src/VirtualLocation.cpp src/Sha256.cpp -lsqlite3 -o /tmp/listi-faults
// Real staging/dispatch code, deterministic fake network commit failures.
#include "FileOperation.h"
#include "JobReportStore.h"
#include "VirtualFile.h"
#include "UnavailableVolume.h"
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
};
std::map<std::string, Remote> files;
bool corruptUpload = false, denyReadback = false, cancelReadback = false;
bool cancelAtReadyCheck = false;
unsigned postCancelReadCalls = 0;
bool commitFailed = false, expireDuringCleanup = false;
size_t cleanupReadBytes = 0;
bool editSmbSourceOnRead = false;
bool truncateUpload = false, omitStageSize = false, growSourceOnRead = false;
bool failCommit = false, failRollback = false, failPartCleanup = false,
     failBackupCleanup = false, cancelAfterBackup = false, cancelled = false,
     mutateDestination = false, loseBackupReply = false, loseCommitReply = false;
const std::string destination = "sftp://fixture/out/input";
enum class LocalPublish { Unexpected, Limited, FlushFailure, CancelAfterPublish };
LocalPublish localPublish = LocalPublish::Unexpected;
bool suffix(const std::string &path, const char *part) {
  return path.find(part) != std::string::npos;
}
}
namespace Listi {
Result RunVirtual(const Job &, const Hooks &);
Result Run(const Job &job, const Hooks &hooks) {
  if (localPublish == LocalPublish::Unexpected || !hooks.privateStaging ||
      hooks.report || job.sources.size() != 1)
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
  if (expireDuringCleanup && c.cleanup)
    c.cleanup->deadline = NetworkCleanupBudget::Clock::now();
  // The target is statted before staging, then again after Ready.
  if (cancelAtReadyCheck && l.URL() == destination && files.size() == 2)
    cancelled = true;
  c.Check();
  auto it = files.find(l.URL());
  if (it == files.end()) return false;
  entry.path = l.URL(); entry.name = LocationLeaf(entry.path);
  entry.directory = it->second.directory;
  entry.sizeKnown = !omitStageSize || !suffix(l.URL(), ".part");
  entry.size = it->second.data.size();
  entry.SetModification(it->second.modified, it->second.nanoseconds);
  return true;
}
std::vector<FileEntry> ListNetwork(const Location &l, const TransferControl &c) {
  std::vector<FileEntry> out;
  for (auto &item : files) {
    if (ParentLocation(item.first) == l.URL()) {
      FileEntry entry;
      StatNetwork(ParseLocation(item.first), entry, c);
      out.push_back(entry);
    }
  }
  return out;
}
void ReadNetwork(const Location &l, const DataSink &sink, const TransferControl &c) {
  if (cancelled)
    ++postCancelReadCalls;
  c.Check();
  if (cancelReadback && suffix(l.URL(), ".part")) {
    cancelled = true;
    c.Check();
  }
  if (denyReadback && suffix(l.URL(), ".part")) throw std::runtime_error("readback denied");
  auto &data = files.at(l.URL()).data;
  if (commitFailed)
    cleanupReadBytes += data.size();
  if (growSourceOnRead && l.URL() == "sftp://fixture/in/input") {
    std::string grown = data + "more data";
    sink(grown.data(), grown.size());
  } else {
    sink(data.data(), data.size());
  }
  if (editSmbSourceOnRead && l.scheme == "smb") {
    data = "changed";
    files.at(l.URL()).nanoseconds = 654000000;
  }
  c.Tick(data.size());
}
void UploadNetwork(const std::string &local, const Location &l, const TransferControl &c) {
  std::ifstream stream(local);
  std::string data(std::istreambuf_iterator<char>(stream), {});
  if (corruptUpload && !data.empty()) data.front() ^= 1;
  files[l.URL()] = {truncateUpload ? data.substr(0, data.size() / 2) : data, 1};
  if (mutateDestination) files[destination] = {"concurrent edit", 2};
  c.Tick(data.size());
}
void ChangeNetwork(const Location &l, const char *action,
                   const std::string &target, const TransferControl &c) {
  c.Check();
  std::string source = l.URL();
  if (std::string(action) == "mkdir") {
    files.emplace(source, Remote{"", 1, 0, true});
    return;
  }
  if (std::string(action) == "remove") {
    if ((suffix(source, ".part") && failPartCleanup) ||
        (suffix(source, ".previous") && failBackupCleanup))
      throw std::runtime_error("injected cleanup failure");
    files.erase(source); return;
  }
  if (std::string(action) != "rename")
    throw std::runtime_error("Unexpected operation");
  if (suffix(source, ".part") && failCommit) {
    commitFailed = true;
    throw std::runtime_error("injected commit failure");
  }
  if (suffix(source, ".previous") && failRollback)
    throw std::runtime_error("injected rename failure");
  if (!files.count(source) || files.count(target))
    throw std::runtime_error("rename precondition");
  files[target] = files.at(source); files.erase(source);
  if ((loseBackupReply && suffix(target, ".previous")) ||
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
    postCancelReadCalls = 0;
    commitFailed = expireDuringCleanup = false;
    cleanupReadBytes = 0;
    cancelAtReadyCheck = false;
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
  auto download = [&](LocalPublish publication, Decision decision, bool report) {
    reset();
    localPublish = publication;
    files[remote] = {"downloaded payload", 1};
    Hooks local = hooks;
    local.conflict = [=](const std::string &target) {
      if (target != localTarget)
        throw std::runtime_error("Wrong conflict target");
      return decision;
    };
    local.fileDone = [&](const std::string &from, const std::string &to) {
      if (from == remote && to == localTarget)
        ++done;
    };
    if (report)
      local.report = std::make_shared<JobReportStore>();
    auto result = RunVirtual(downloadJob, local);
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
  unlink(localTarget.c_str());
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
            uploadReport.outcomes[1].detail.find("Uploaded contents verified.") != std::string::npos &&
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
  unlink(sibling.c_str());
  rmdir(folder.c_str());
  unlink(source.c_str()); rmdir(dir);
  std::cout << checks << " fault checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
