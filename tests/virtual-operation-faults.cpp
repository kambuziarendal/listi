// SPDX-License-Identifier: MIT
// Compile on POSIX: c++ -std=c++17 -Isrc tests/virtual-operation-faults.cpp
// src/VirtualOperation.cpp src/VirtualFile.cpp src/Sha256.cpp -o /tmp/listi-faults
// Real staging/dispatch code, deterministic fake network commit failures.
#include "FileOperation.h"
#include "VirtualFile.h"
#include <fstream>
#include <iostream>
#include <map>
#include <unistd.h>
namespace {
struct Remote { std::string data; int64_t modified = 1; };
std::map<std::string, Remote> files;
bool corruptUpload = false, denyReadback = false, cancelReadback = false;
bool truncateUpload = false, omitStageSize = false, growSourceOnRead = false;
bool failCommit = false, failRollback = false, failPartCleanup = false,
     failBackupCleanup = false, cancelAfterBackup = false, cancelled = false,
     mutateDestination = false, loseBackupReply = false, loseCommitReply = false;
const std::string destination = "sftp://fixture/out/input";
bool suffix(const std::string &path, const char *part) {
  return path.find(part) != std::string::npos;
}
}
namespace Listi {
Result RunVirtual(const Job &, const Hooks &);
Result Run(const Job &, const Hooks &) { throw std::runtime_error("Unexpected local dispatch"); }
std::vector<FileEntry> ListArchive(const Location &, const TransferControl &) {
  throw std::runtime_error("Unexpected archive dispatch");
}
bool StatArchive(const Location &, FileEntry &, const TransferControl &) { throw std::runtime_error("Unexpected archive dispatch"); }
void ReadArchive(const Location &, const DataSink &, const TransferControl &) {
  throw std::runtime_error("Unexpected archive dispatch");
}
bool StatNetwork(const Location &l, FileEntry &entry, const TransferControl &c) {
  c.Check();
  auto it = files.find(l.URL());
  if (it == files.end()) return false;
  entry.path = l.URL(); entry.name = LocationLeaf(entry.path);
  entry.sizeKnown = !omitStageSize || !suffix(l.URL(), ".part");
  entry.size = it->second.data.size(); entry.modified = it->second.modified;
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
  c.Check();
  if (cancelReadback && suffix(l.URL(), ".part")) {
    cancelled = true;
    c.Check();
  }
  if (denyReadback && suffix(l.URL(), ".part")) throw std::runtime_error("readback denied");
  auto &data = files.at(l.URL()).data;
  if (growSourceOnRead && l.URL() == "sftp://fixture/in/input") {
    std::string grown = data + "more data";
    sink(grown.data(), grown.size());
  } else {
    sink(data.data(), data.size());
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
  if (std::string(action) == "remove") {
    if ((suffix(source, ".part") && failPartCleanup) ||
        (suffix(source, ".previous") && failBackupCleanup))
      throw std::runtime_error("injected cleanup failure");
    files.erase(source); return;
  }
  if (std::string(action) != "rename")
    throw std::runtime_error("Unexpected operation");
  if ((suffix(source, ".part") && failCommit) ||
      (suffix(source, ".previous") && failRollback))
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
  reset(); truncateUpload = true; r = RunVirtual(job, hooks);
  check(!r.completed && !done && !r.errors.empty() && files.size() == 1 &&
        files.at(destination).data == "old contents",
        "acknowledged short upload cannot replace the original");
  reset(); omitStageSize = true; r = RunVirtual(job, hooks);
  check(r.completed == 1 && done == 1 && r.errors.empty() && files.size() == 1 &&
        files.at(destination).data == "new contents",
        "valid upload with no server size is verified by reading back the stage");
  reset(); omitStageSize = truncateUpload = true; r = RunVirtual(job, hooks);
  check(!r.completed && !done && !r.errors.empty() && files.size() == 1 &&
        files.at(destination).data == "old contents",
        "short upload with no server size cannot replace the original");
  for (bool knownSize : {false, true}) {
    reset(); corruptUpload = true; omitStageSize = !knownSize; r = RunVirtual(job, hooks);
    check(!r.completed && !done && !r.errors.empty() && files.size() == 1 &&
          files.at(destination).data == "old contents",
          "same-length corruption cannot replace the original");
  }
  reset(); denyReadback = true; r = RunVirtual(job, hooks);
  check(!r.completed && !done && !r.errors.empty() && files.size() == 1 &&
        files.at(destination).data == "old contents", "unreadable stage cannot publish");
  denyReadback = false; r = RunVirtual(job, hooks);
  check(r.completed == 1 && done == 1 && files.at(destination).data == "new contents",
        "retry after failed verification succeeds");
  reset(); cancelReadback = true; r = RunVirtual(job, hooks);
  check(r.cancelled && !r.completed && !done && files.size() == 1 &&
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
  reset(); cancelAfterBackup = true; r = RunVirtual(job, hooks);
  check(r.cancelled && r.completed == 0 && done == 0 && files.size() == 1 &&
        files.at(destination).data == "old contents",
        "cancellation between backup and commit rolls back without cancellation blocking recovery");
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
  unlink(source.c_str()); rmdir(dir);
  std::cout << checks << " fault checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
