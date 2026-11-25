// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "FileOperation.h"
#include "JobSnapshot.h"
#include "MessageStore.h"
#include "VirtualFile.h"
#include <FindDirectory.h>
#include <Node.h>
#include <Path.h>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <fs_info.h>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace Listi;
using namespace std::chrono_literals;
namespace {
fs::path destination, fault, archivedMember;
std::vector<fs::path> watched, flushed;
bool failStageMember = false, failPublishedParent = false,
     failStageCleanup = false;
bool simulateRemovable = false;
std::mutex mutex;
std::condition_variable changed;
bool entered = false, release = false, returned = false;
std::thread::id caller;
int parkedDescriptor = -1;

void Require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Pass(bool condition, const char *message) {
  Require(condition, message);
  std::cout << "PASS " << message << std::endl;
}
bool Matches(const fs::path &path, const struct stat &fd) {
  struct stat node;
  return !path.empty() && !lstat(path.c_str(), &node) &&
         fd.st_ino == node.st_ino && fd.st_dev == node.st_dev;
}
bool DurabilityError(const Result &result) {
  for (const auto &error : result.errors)
    if (error.find("Durability after restart is unknown") != std::string::npos)
      return true;
  return false;
}
struct Fixture {
  fs::path root;
  ~Fixture() { fs::remove_all(root); }
};
} // namespace

extern "C" int __real_fs_stat_dev(dev_t, fs_info *);
extern "C" int __wrap_fs_stat_dev(dev_t volume, fs_info *info) {
  const int status = __real_fs_stat_dev(volume, info);
  if (!status && simulateRemovable)
    info->flags |= B_FS_IS_REMOVABLE;
  return status;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  struct stat node;
  if (!fstat(fd, &node) && S_ISDIR(node.st_mode)) {
    for (const auto &path : watched)
      if (Matches(path, node))
        flushed.push_back(path);
    bool fail = Matches(fault, node) ||
                (failPublishedParent && fs::exists(archivedMember) &&
                 Matches(destination, node));
    if (failStageMember) {
      for (const auto &entry : fs::directory_iterator(destination))
        if (entry.path().filename().string().rfind(".Listi-archive-stage-",
                                                   0) == 0 &&
            Matches(entry.path() / "tree" / "branch" / "empty", node))
          fail = true;
    }
    if (failStageCleanup && Matches(destination, node)) {
      bool stageRemains = false;
      for (const auto &entry : fs::directory_iterator(destination))
        if (entry.path().filename().string().rfind(".listi-copy-", 0) == 0)
          stageRemains = true;
      fail = !stageRemains;
    }
    if (fail) {
      errno = EIO;
      return -1;
    }
    if (simulateRemovable && Matches(destination / "tree", node)) {
      std::unique_lock<std::mutex> lock(mutex);
      if (std::this_thread::get_id() == caller)
        std::abort();
      parkedDescriptor = fd;
      entered = true;
      changed.notify_all();
      changed.wait(lock, [] { return release; });
      returned = true;
      changed.notify_all();
    }
  }
  return __real_fsync(fd);
}

int main() try {
  char path[] = "/tmp/Listi-directory-flush-XXXXXX";
  Require(mkdtemp(path), "Create fixture");
  Fixture fixture{fs::canonical(path)};
  const auto root = fixture.root, tree = root / "tree";
  const auto empty = tree / "branch" / "empty";
  destination = root / "out";
  fs::create_directories(empty);
  fs::create_directory(destination);
  const auto target = destination / "tree";
  const auto copiedEmpty = target / "branch" / "empty";
  BNode metadata(empty.c_str());
  const char attribute[] = "audit BFS";
  Require(metadata.WriteAttr("Listi:audit", B_RAW_TYPE, 0, attribute,
                             sizeof(attribute)) == sizeof(attribute),
          "Write BFS attribute");
  const struct timespec times[] = {{0, UTIME_OMIT}, {1600000000, 0}};
  Require(!chmod(empty.c_str(), 0750) &&
              !utimensat(AT_FDCWD, empty.c_str(), times, 0),
          "Set folder metadata");
  unsigned done = 0;
  Hooks hooks;
  hooks.fileDone = [&](const auto &, const auto &) { ++done; };
  auto copy = Job{Action::Copy, {tree}, destination};
  FreezeLocalJob(copy, destination);
  watched = {copiedEmpty, target / "branch", target, destination};
  auto result = Run(copy, hooks);
  BNode copiedMetadata(copiedEmpty.c_str());
  char readback[sizeof(attribute)];
  const auto attributeSize = copiedMetadata.ReadAttr(
      "Listi:audit", B_RAW_TYPE, 0, readback, sizeof(readback));
  struct stat copiedStat;
  Pass(
      result.completed == 1 && result.errors.empty() && done == 3 &&
          !lstat(copiedEmpty.c_str(), &copiedStat) &&
          (copiedStat.st_mode & 0777) == 0750 &&
          copiedStat.st_mtime == times[1].tv_sec &&
          attributeSize == sizeof(attribute) &&
          !memcmp(attribute, readback, sizeof(attribute)) &&
          flushed == std::vector<fs::path>{copiedEmpty, target / "branch",
                                           target / "branch", target, target,
                                           destination},
      "directory-only BFS copy flushes leaf and parents in post-order and preserves attributes");
  watched.clear();
  fs::remove_all(target);

  const auto standalone = root / "standalone";
  fs::create_directory(standalone);
  fault = destination / "standalone";
  done = 0;
  result = Run({Action::Copy, {standalone}, destination}, hooks);
  Pass(
      !result.completed && !done && DurabilityError(result) &&
          fs::is_directory(standalone) && fs::is_directory(fault),
      "empty folder inode flush failure keeps source and suppresses completion");
  fs::remove(fault);

  const auto move = [&] {
    Job job{Action::Move, {tree}, destination};
    FreezeLocalJob(job, destination);
    return job;
  }();
  fs::create_directory(target);
  std::ofstream(target / "keep") << "pre-existing";
  fault = destination;
  done = 0;
  result = Run(move, hooks);
  Pass(
      !result.completed && !done && DurabilityError(result) &&
          fs::is_directory(empty) && fs::is_directory(copiedEmpty) &&
          fs::exists(target / "keep"),
      "merged Move cannot remove any source entry after namespace parent flush fails");
  fault.clear();
  result = Run(move, hooks);
  Pass(
      result.completed == 1 && done == 3 && !fs::exists(tree) &&
          fs::is_directory(copiedEmpty) && fs::exists(target / "keep"),
      "successful same-volume merged Move completes source cleanup after finalization");

  fs::remove(target / "keep");
  auto pack = PrepareArchiveJob(false, {target});
  pack.archiveFormat = "zip";
  SetArchiveDestination(pack, root, "fixture.zip");
  result = Run(pack, {});
  Require(result.completed == 1 && result.errors.empty(), "Create ZIP fixture");
  const auto archive = root / "fixture.zip";
  const auto virtualTree = OpenArchive(archive) + "tree";
  fs::remove_all(target);
  fault = copiedEmpty;
  done = 0;
  result = Run({Action::Copy, {virtualTree}, destination}, hooks);
  Pass(!result.completed && !done && DurabilityError(result) &&
           fs::exists(archive),
       "virtual archive member copy reports failed nested folder flush");
  fs::remove_all(target);
  fault.clear();
  result = Run({Action::Copy, {virtualTree}, destination}, hooks);
  Pass(result.completed == 1 && done > 0 && fs::is_directory(copiedEmpty),
       "virtual archive member copy completes after finalizing nested folders");

  fs::remove_all(target);
  auto archiveMove =
      PrepareArchiveMove({Action::Move, {virtualTree}, destination});
  fault = copiedEmpty;
  done = 0;
  result = Run(archiveMove, hooks);
  fault.clear();
  FileEntry originalTree;
  Pass(
      !result.completed && !done && DurabilityError(result) &&
          StatFile(virtualTree, originalTree) && originalTree.directory,
      "archive-to-local Move does not update source archive after destination folder flush fails");

  auto extract = PrepareArchiveJob(true, {archive});
  SetArchiveDestination(extract, destination, "extracted");
  archivedMember = destination / "extracted";
  done = 0;
  failStageMember = true;
  result = Run(extract, hooks);
  failStageMember = false;
  Pass(
      !result.completed && !done && DurabilityError(result) &&
          !fs::exists(archivedMember) && fs::exists(archive),
      "whole archive extraction never publishes a stage with a failed subdirectory flush");
  failPublishedParent = true;
  result = Run(extract, hooks);
  failPublishedParent = false;
  Pass(
      !result.completed && !done && DurabilityError(result) &&
          fs::is_directory(archivedMember / "tree" / "branch" / "empty") &&
          fs::exists(archive),
      "whole archive extraction reports uncertainty when publication parent flush fails");
  fs::remove_all(archivedMember);
  result = Run(extract, hooks);
  Pass(
      result.completed == 1 && done > 0 &&
          fs::is_directory(archivedMember / "tree" / "branch" / "empty"),
      "whole archive tree extraction succeeds and publishes member completion");

  const auto sourceFile = root / "cancelled-stage";
  std::ofstream(sourceFile) << "retain source";
  Hooks cancel;
  bool stop = false;
  cancel.progress = [&](const auto &, uint64_t bytes) { stop = bytes != 0; };
  cancel.proceed = [&] { return !stop; };
  BPath settings;
  Require(find_directory(B_USER_SETTINGS_DIRECTORY, &settings) == B_OK &&
              settings.Append("Listi-recovery") == B_OK,
          "Locate recovery journals");
  auto journalRetained = [&] {
    for (const auto &entry : fs::directory_iterator(settings.Path())) {
      if (entry.path().filename().string().rfind("record-", 0) != 0)
        continue;
      BMessage record;
      try {
        record = LoadMessageStore(entry.path());
      } catch (...) {
        continue;
      }
      if (record.GetString("source", "") == sourceFile.string()) {
        fs::remove(entry.path());
        return true;
      }
    }
    return false;
  };
  for (bool fail : {false, true}) {
    stop = false;
    failStageCleanup = fail;
    result = Run({Action::Copy, {sourceFile}, destination}, cancel);
    failStageCleanup = false;
    Pass(
        result.cancelled && !result.completed && fs::exists(sourceFile) &&
            !fs::exists(destination / sourceFile.filename()) &&
            journalRetained() == fail && result.errors.empty() == !fail,
        fail
            ? "cancelled staging removal cannot retire journal after failed parent flush"
            : "successful cancelled-stage parent flush retires recovery journal");
  }

  // Mark the actual mounted BFS volume removable without changing its identity.
  // Park the destination inode fsync, not a filesystem made to hang globally.
  fs::create_directories(tree / "isolated");
  fs::remove_all(target);
  caller = std::this_thread::get_id();
  simulateRemovable = true;
  Hooks usb;
  usb.proceed = [&] {
    std::lock_guard<std::mutex> lock(mutex);
    return !entered;
  };
  const auto start = std::chrono::steady_clock::now();
  result = Run({Action::Copy, {tree, standalone}, destination}, usb);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  Require(entered,
          "Fixture never reached isolated destination directory flush");
  Pass(
      !result.completed && result.interruptedCopyFolder == "/boot" &&
          DurabilityError(result) && elapsed < 3s && fs::is_directory(tree) &&
          !fs::exists(destination / standalone.filename()),
      "blocked removable directory flush detaches on cancellation and stops further roots");
  fs::rename(target, root / "old-target");
  fs::create_directory(target);
  {
    std::unique_lock<std::mutex> lock(mutex);
    release = true;
    changed.notify_all();
    Require(changed.wait_for(lock, 2s, [] { return returned; }),
            "Detached directory flush did not return");
  }
  for (int i = 0; i < 200 && fcntl(parkedDescriptor, F_GETFD) >= 0; ++i)
    std::this_thread::sleep_for(5ms);
  Pass(
      fcntl(parkedDescriptor, F_GETFD) < 0 && fs::is_empty(target) &&
          fs::is_directory(root / "old-target" / "isolated"),
      "late removable flush closes its descriptor and never mutates a replacement directory");
  return 0;
} catch (const std::exception &error) {
  std::cerr << "FAIL " << error.what() << std::endl;
  return 1;
}
