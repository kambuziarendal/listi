// SPDX-License-Identifier: MIT
// Deterministic native syscall faults, including when the test user is root.
#include "FileOperation.h"
#include "JobSnapshot.h"
#include "Trash.h"
#include <Node.h>
#include <fcntl.h>
#include <cstdarg>
#include <fs_attr.h>
#include <fs_info.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace Listi;
namespace fs = std::filesystem;
namespace {
std::string deniedStage, watchedStage, deniedRemoval;
int watchedFd = -1, writes = 0;
int writeFailure = 0;
bool denyTrashCleanup = false;
bool failStageClose = false, failDirectorySync = false;
int stageForClose = -1;
ino_t directoryForSync = -1;
bool deviceGone = false, reportFat = false;
int failures = 0;

void Check(bool ok, const char *label) {
  std::cout << (ok ? "PASS " : "FAIL ") << label << '\n';
  failures += !ok;
}
std::string Read(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}
bool Has(const Result &result, const fs::path &source, const fs::path &dest,
         const char *status, const char *detail = "") {
  return std::any_of(result.outcomes.begin(), result.outcomes.end(),
                     [&](const EntryOutcome &entry) {
    return entry.source == source.string() && entry.destination == dest.string() &&
           entry.status == status && entry.detail.find(detail) != std::string::npos;
  });
}
}

extern "C" int __real_openat(int, const char *, int, ...);
extern "C" int __wrap_openat(int directory, const char *path, int flags, ...) {
  if (!(flags & O_CREAT))
    return __real_openat(directory, path, flags);
  va_list args;
  va_start(args, flags);
  const auto mode = va_arg(args, int);
  va_end(args);
  const bool stage = std::string(path).rfind(".listi-copy-", 0) == 0;
  if (stage && !deniedStage.empty()) {
    errno = EACCES;
    return -1;
  }
  const bool watch = stage && !watchedStage.empty();
  const int fd = __real_openat(directory, path, flags, mode);
  if (watch) {
    watchedFd = fd;
    stageForClose = fd;
  }
  return fd;
}
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" ssize_t __wrap_write(int fd, const void *data, size_t size) {
  if (fd != watchedFd || !size)
    return __real_write(fd, data, size);
  ++writes;
  if (writes == 1) {
    errno = EINTR;
    return -1;
  }
  if (writes == 2)
    return __real_write(fd, data, std::min<size_t>(size, 7));
  watchedFd = -1;
  if (writeFailure) {
    errno = writeFailure;
    return -1;
  }
  return __real_write(fd, data, size);
}
extern "C" int __real_close(int);
extern "C" int __wrap_close(int fd) {
  const auto status = __real_close(fd);
  if (fd == stageForClose) {
    stageForClose = -1;
    if (failStageClose) {
      errno = EIO;
      return -1;
    }
  }
  return status;
}
extern "C" int __real_lstat(const char *, struct stat *);
extern "C" int __wrap_lstat(const char *path, struct stat *st) {
  const int status = __real_lstat(path, st);
  // FAT synthesizes execute bits for .tmp stages but not the final .png.
  if (status == 0 && reportFat && strstr(path, "/.listi-copy-") && S_ISREG(st->st_mode))
    st->st_mode |= S_IXUSR | S_IXGRP | S_IXOTH;
  return status;
}
extern "C" int __real_fs_stat_dev(dev_t, fs_info *);
extern "C" int __wrap_fs_stat_dev(dev_t device, fs_info *info) {
  const int status = __real_fs_stat_dev(device, info);
  if (!status && reportFat)
    strcpy(info->fsh_name, "fat");
  return status;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  struct stat st;
  if (failDirectorySync && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) &&
      st.st_ino == directoryForSync) {
    errno = EIO;
    return -1;
  }
  return __real_fsync(fd);
}
extern "C" int __real_stat(const char *, struct stat *);
extern "C" int __wrap_stat(const char *path, struct stat *st) {
  if (deviceGone && strncmp(path, "/dev/disk/", 10) == 0) {
    errno = ENOENT;
    return -1;
  }
  return __real_stat(path, st);
}

extern "C" int __real_unlinkat(int, const char *, int);
extern "C" int __wrap_unlinkat(int fd, const char *path, int flags) {
  if (!deniedRemoval.empty() && Leaf(deniedRemoval) == path) {
    errno = EACCES;
    return -1;
  }
  return __real_unlinkat(fd, path, flags);
}

extern "C" status_t __real__ZN5BNode10RemoveAttrEPKc(BNode *, const char *);
extern "C" status_t __wrap__ZN5BNode10RemoveAttrEPKc(BNode *node, const char *name) {
  if (denyTrashCleanup && strcmp(name, "_trk/original_path") == 0)
    return B_PERMISSION_DENIED;
  return __real__ZN5BNode10RemoveAttrEPKc(node, name);
}

int main() {
  char temp[] = "/boot/home/Listi-operation-faults-XXXXXX";
  if (!mkdtemp(temp))
    return 2;
  const auto root = fs::canonical(temp);
  const auto out = root / "out", source = root / "file", target = out / "file";
  fs::create_directory(out);
  const std::string payload(1000, 'x');
  std::ofstream(source) << payload;
  std::ofstream(target) << "original";
  for (const std::string name : {"report.pdf", ".settings", "notes"}) {
    const auto incoming = root / name, existing = out / name;
    const std::string stem = name == "report.pdf" ? "report" : name;
    const std::string extension = name == "report.pdf" ? ".pdf" : "";
    const auto occupied = out / (stem + " (2)" + extension);
    const auto candidate = out / (stem + " (3)" + extension);
    std::ofstream(incoming) << "incoming";
    std::ofstream(existing) << "existing";
    std::ofstream(occupied) << "prior";
    Job copy{Action::Copy, {incoming.string()}, out.string()};
    bool race = true;
    Hooks keep;
    keep.copyConflict = [&](const CopyConflict &request) {
      Check(request.source == incoming.string() && request.destination == existing.string() &&
                request.sourceSize == 8 && request.destinationSize == 8 &&
                request.proposed == candidate.string(),
            "keep both describes both files and skips occupied names");
      if (race)
        std::ofstream(candidate) << "competitor";
      return Decision::KeepBoth;
    };
    auto raced = Run(copy, keep);
    Check(!raced.errors.empty() && !raced.completed && Read(candidate) == "competitor" &&
              Read(existing) == "existing" && Read(incoming) == "incoming",
          "keep both never overwrites a concurrently claimed name");
    fs::remove(candidate);
    race = false;
    bool cancel = false;
    keep.progress = [&](const auto &, uint64_t bytes) { cancel = bytes != 0; };
    keep.proceed = [&] { return !cancel; };
    auto interrupted = Run(copy, keep);
    Check(interrupted.cancelled && !interrupted.completed &&
              Has(interrupted, incoming, candidate, "unprocessed", "Cancelled before commit") &&
              !fs::exists(candidate) && Read(existing) == "existing",
          "cancelling staged keep both reports the proposed path without creating it");
    keep.progress = {};
    keep.proceed = {};
    auto copied = Run(copy, keep);
    Check(copied.errors.empty() && copied.completed == 1 &&
              Has(copied, incoming, candidate, "copied") &&
              copied.completedRoots.size() == 1 &&
              copied.completedRoots.front().second == candidate.string() &&
              Read(candidate) == "incoming" && Read(existing) == "existing" &&
              Read(occupied) == "prior",
          "keep both preserves contents and reports the actual published path");
    for (const auto &path : {incoming, existing, occupied, candidate})
      fs::remove(path);
  }
  // Substitute only the filesystem identification; exercise production staging,
  // publication and typed attributes on an isolated disposable directory.
  {
    const auto image = root / "screen.png", output = out / "screen.png";
    std::ofstream(image) << payload;
    BNode node(image.c_str());
    int32 width = 640;
    Check(node.WriteAttr("Media:Width", B_INT32_TYPE, 0, &width, sizeof(width)) == sizeof(width),
          "fixture has screenshot width metadata");
    reportFat = true;
    Job copy{Action::Copy, {image.string()}, out.string()};
    Hooks consent;
    bool allow = false;
    int prompts = 0;
    consent.confirmFatCopy = [&](const std::string &src, const std::string &dst) {
      ++prompts;
      Check(src == image.string() && dst == out.string(), "metadata choice identifies both endpoints");
      return allow;
    };
    const auto refused = Run(copy, consent);
    Check(refused.cancelled && prompts == 1 && !fs::exists(output) && Read(image) == payload,
          "declining metadata loss leaves source intact and no destination file");
    allow = true;
    const auto copied = Run(copy, consent);
    BNode destination(output.c_str());
    attr_info attr{};
    Check(copied.completed == 1 && copied.errors.empty() && copied.copyLimitations &&
              prompts == 2 && Read(output) == payload && Read(image) == payload &&
              node.GetAttrInfo("Media:Width", &attr) == B_OK &&
              destination.GetAttrInfo("Media:Width", &attr) != B_OK,
          "explicit FAT copy publishes PNG bytes despite extension-derived execute bits and reports omitted attributes");
    std::ofstream(output) << "existing";
    consent.conflict = [](const auto &) { return Decision::Replace; };
    copy.action = Action::Move;
    const auto moved = Run(copy, consent);
    BNode movedNode(output.c_str());
    Check(moved.errors.empty() && moved.completed == 1 && prompts == 2 &&
              movedNode.GetAttrInfo("Media:Width", &attr) == B_OK,
          "copy waiver never applies to Move");
    reportFat = false;
    fs::remove(output);
  }
  Hooks replace;
  replace.conflict = [](const std::string &) { return Decision::Replace; };
  Job move{Action::Move, {source.string()}, out.string()};

  deniedStage = (out / ".listi-copy-XXXXXX").string();
  auto result = Run(move, replace);
  Check(result.errors.size() == 1 && result.completed == 0 &&
            Has(result, source, target, "failed", strerror(EACCES)) &&
            Read(source) == payload && Read(target) == "original",
        "destination permission failure names both paths and preserves both files");
  deniedStage.clear();

  watchedStage = (out / ".listi-copy-XXXXXX").string();
  for (const int error : {EIO, EDQUOT}) {
    writes = 0;
    writeFailure = error;
    result = Run(move, replace);
    Check(writes == 3 && !result.completed && result.errors.size() == 1 &&
              Has(result, source, target, "failed", strerror(error)) &&
              Read(source) == payload && Read(target) == "original" &&
              std::distance(fs::directory_iterator(out), fs::directory_iterator()) == 1,
          error == EDQUOT ? "quota error after partial write preserves both files and removes staging"
                          : "write failure after partial output discards staging, not the original");
  }

  writeFailure = 0;
  writes = 0;
  deniedRemoval = source.string();
  result = Run(move, replace);
  Check(writes == 3 && result.errors.size() == 1 && !result.completed &&
            Has(result, source, target, "source-retained", strerror(EACCES)) &&
            Read(source) == payload && Read(target) == payload,
        "retry EINTR and short write; report copied file and denied source cleanup");
  deniedRemoval.clear();
  watchedStage.clear();

  std::ofstream(target) << "original";
  watchedStage = (out / ".listi-copy-XXXXXX").string();
  failStageClose = true;
  result = Run(move, replace);
  failStageClose = false;
  watchedStage.clear();
  Check(!result.completed && !result.errors.empty() &&
            Has(result, source, target, "failed", strerror(EIO)) &&
            Read(source) == payload && Read(target) == "original",
        "staging close failure retains the previously published file and source");

  struct stat folderStat;
  if (stat(out.c_str(), &folderStat))
    return 2;
  directoryForSync = folderStat.st_ino;
  failDirectorySync = true;
  result = Run(move, replace);
  failDirectorySync = false;
  Check(!result.completed && !result.errors.empty() &&
            Has(result, source, target, "published-flush-failed", "published") &&
            Read(source) == payload && Read(target) == payload,
        "directory flush failure accurately reports a changed target and retained source");

  std::ofstream(target) << "original";
  Hooks disconnect = replace;
  disconnect.progress = [](const std::string &, uint64_t bytes) {
    if (bytes)
      deviceGone = true;
  };
  result = Run({Action::Copy, {source.string()}, out.string()}, disconnect);
  deviceGone = false;
  Check(!result.completed && result.errors.size() == 2 &&
            result.errors.front().find("Staging recovery record retained") != std::string::npos &&
            Has(result, source, target, "failed", "device disconnected") &&
            Read(source) == payload && Read(target) == "original" &&
            std::distance(fs::directory_iterator(out), fs::directory_iterator()) == 1,
        "disconnected device cannot publish a cached copy or replace the destination");

  // A conflict forces a copy-and-cleanup move even on a single volume.
  std::ofstream(target) << "old";
  bool edited = false;
  Hooks changeAfterCopy = replace;
  changeAfterCopy.proceed = [&] {
    if (!edited && Read(target) == payload) {
      edited = true;
      std::ofstream(target) << "changed after commit";
    }
    return true;
  };
  result = Run(move, changeAfterCopy);
  Check(edited && !result.errors.empty() && !result.completed &&
            Read(source) == payload && Read(target) == "changed after commit",
        "changed destination stops move cleanup and preserves the source");

  const auto alias = root / "destination-link", alternate = root / "alternate";
  fs::create_directory(alternate);
  fs::create_directory_symlink(out, alias);
  std::ofstream(target) << "old";
  Hooks redirectAfterCopy = replace;
  bool switched = false;
  redirectAfterCopy.proceed = [&] {
    if (!switched && Read(target) == payload) {
      fs::remove(alias);
      fs::create_directory_symlink(alternate, alias);
      switched = true;
    }
    return true;
  };
  result = Run({Action::Move, {source.string()}, alias.string()}, redirectAfterCopy);
  Check(switched && !result.errors.empty() && !result.completed &&
            Read(source) == payload && Read(target) == payload &&
            fs::is_empty(alternate),
        "destination alias changed after copy leaves the original in place");

  // Even an empty merged folder must stay bound to its original target.
  const auto dir = root / "empty", mapped = out / "empty", saved = out / "saved";
  fs::create_directory(dir);
  fs::create_directory(mapped);
  Hooks swapFolder;
  swapFolder.progress = [&](const std::string &path, uint64_t) {
    if (path == dir && !fs::exists(saved)) {
      fs::rename(mapped, saved);
      fs::create_directory(mapped);
    }
  };
  result = Run({Action::Move, {dir.string()}, out.string()}, swapFolder);
  Check(!result.errors.empty() && !result.completed && fs::exists(dir) &&
            fs::exists(saved) && fs::exists(mapped),
        "replacement of empty destination at final progress keeps the source directory");

  const auto deleted = root / "delete", original = root / "delete-original";
  std::ofstream(deleted) << "selected";
  Job deletion{Action::Delete, {deleted.string()}, {}};
  FreezeLocalJob(deletion);
  int polls = 0;
  Hooks swapBeforeSnapshot;
  swapBeforeSnapshot.proceed = [&] {
    if (++polls == 2) {
      fs::rename(deleted, original);
      std::ofstream(deleted) << "replacement";
    }
    return true;
  };
  result = Run(deletion, swapBeforeSnapshot);
  Check(!result.completed && !result.errors.empty() &&
            Read(deleted) == "replacement" && Read(original) == "selected",
        "entry replaced before the deletion snapshot is left untouched");

  const auto growing = root / "download", copied = out / "download";
  fs::create_directory(growing);
  std::ofstream(growing / "first") << "first";
  Hooks addAfterScan;
  addAfterScan.progress = [&](const std::string &path, uint64_t) {
    if (path == growing)
      std::ofstream(growing / "second") << "just arrived";
  };
  result = Run({Action::Copy, {growing.string()}, out.string()}, addAfterScan);
  Check(!result.errors.empty() && !result.completed &&
            Has(result, growing, copied, "partial") &&
            Read(growing / "second") == "just arrived" &&
            Read(copied / "first") == "first" && !fs::exists(copied / "second"),
        "folder changed after enumeration is not reported as completely copied");

  const auto longName = std::string(250, 'a');
  std::ofstream(root / longName) << "long name";
  result = Run({Action::Copy, {(root / longName).string()}, out.string()}, {});
  Check(result.errors.empty() && result.completed == 1 &&
            Read(out / longName) == "long name",
        "copying a valid long filename does not require room for a staging suffix");

  const auto other = root / "second", untouched = root / "third";
  std::ofstream(other) << "second";
  std::ofstream(untouched) << "third";
  std::ofstream(out / "second") << "existing";
  move.sources = {source.string(), other.string(), untouched.string()};
  Hooks conflicts;
  conflicts.conflict = [&](const std::string &path) {
    return path == target.string() ? Decision::Skip : Decision::Abort;
  };
  result = Run(move, conflicts);
  Check(result.cancelled && result.skipped == 1 && !result.completed &&
            Has(result, source, target, "skipped") &&
            Has(result, other, out / "second", "unprocessed") &&
            Has(result, untouched, out / "third", "unprocessed") &&
            Read(out / "second") == "existing" && fs::exists(untouched),
        "skipped, aborted and unstarted roots have individually identifiable outcomes");
  const auto oldParent = root / "old-parent";
  fs::create_directory(oldParent);
  const auto redirected = oldParent / ("redirected-" + root.filename().string());
  std::ofstream(redirected) << payload;
  const fs::path inTrash = MoveToTrash(redirected.string());
  fs::remove(oldParent);
  const auto redirectedParent = inTrash.parent_path() / ("parent-" + root.filename().string());
  fs::create_directory(redirectedParent);
  fs::create_directory_symlink(redirectedParent, oldParent);
  result = Run({Action::Restore, {inTrash.string()}, {}}, {});
  BNode retained(inTrash.c_str());
  attr_info info;
  Check(!result.errors.empty() && !result.completed && fs::exists(inTrash) &&
            retained.GetAttrInfo("_trk/original_path", &info) == B_OK,
        "redirected original parent cannot restore back into Trash or strip metadata");
  fs::remove(oldParent);
  fs::remove_all(redirectedParent);
  fs::create_directory(oldParent);
  std::ofstream(redirected) << "replacement";
  result = Run({Action::Restore, {inTrash.string()}, {}}, {});
  Check(!result.errors.empty() && Read(redirected) == "replacement" && Read(inTrash) == payload,
        "restore collision keeps both entries");
  fs::remove(redirected);
  result = Run({Action::Restore, {inTrash.string()}, {}}, {});
  Check(result.completed == 1 && Read(redirected) == payload && !fs::exists(inTrash),
        "restore retries successfully after original parent and name become available");
  if (fs::exists(inTrash))
    fs::remove(inTrash);

  const auto trashed = MoveToTrash(source.string());
  Job guardedRestore{Action::Restore, {trashed}, {}};
  FreezeLocalJob(guardedRestore);
  const auto shown = OriginalTrashPath(trashed, guardedRestore.expectedSources.at(trashed));
  Check(shown == source.string(), "restore preview identifies exact original path");
  guardedRestore.restoreTargets[trashed] = shown + "-different";
  result = Run(guardedRestore, {});
  Check(!result.completed && !result.errors.empty() && fs::exists(trashed) && !fs::exists(source),
        "restore refuses to repoint a user's destination after confirmation");
  denyTrashCleanup = true;
  result = Run({Action::Restore, {trashed}, {}}, {});
  denyTrashCleanup = false;
  Check(result.errors.size() == 1 && result.completed == 1 &&
            Has(result, trashed, source, "moved", "could not clear Trash metadata") &&
            !fs::exists(trashed) && Read(source) == payload,
        "post-restore metadata warning does not hide a committed move");
  if (fs::exists(trashed))
    fs::remove(trashed);
  fs::remove_all(root);
  return failures ? 1 : 0;
}
