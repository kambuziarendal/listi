// SPDX-License-Identifier: MIT
// Deterministic native syscall faults, including when the test user is root.
#include "FileOperation.h"
#include "JobSnapshot.h"
#include "TransferCommand.h"
#include "JobReportStore.h"
#include "Trash.h"
#include <Node.h>
#include <String.h>
#include <fcntl.h>
#include <cstdarg>
#include <fs_attr.h>
#include <fs_info.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace Listi;
namespace fs = std::filesystem;
namespace {
std::string deniedStage, watchedStage, deniedRemoval, claimNewLeaf;
bool newLeafClaimed = false;
int watchedFd = -1, writes = 0;
int writeFailure = 0;
bool denyTrashCleanup = false;
bool failStageClose = false, failDirectorySync = false;
int stageForClose = -1;
ino_t directoryForSync = -1;
bool deviceGone = false, reportFat = false, synthesizeStageMode = false;
std::string simulatedHardlink;
int failures = 0;
int exactNameScans = 0;

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
  if ((flags & O_DIRECTORY) && !strcmp(path, "."))
    ++exactNameScans;
  if (!(flags & O_CREAT))
    return __real_openat(directory, path, flags);
  va_list args;
  va_start(args, flags);
  const auto mode = va_arg(args, int);
  va_end(args);
  if (claimNewLeaf == path) {
    const int competing = __real_openat(directory, path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (competing >= 0) {
      const char contents[] = "competitor";
      newLeafClaimed = write(competing, contents, sizeof(contents) - 1) ==
                       sizeof(contents) - 1;
      close(competing);
    }
  }
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
  if (!status && simulatedHardlink == path)
    st->st_nlink = 2;
  // FAT synthesizes execute bits for .tmp stages but not the final .png.
  if (status == 0 && synthesizeStageMode && strstr(path, "/.listi-copy-") && S_ISREG(st->st_mode))
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

namespace {
// Real prelinked image on Haiku: the names exist even on drivers which
// cannot create links or provide a native Tracker Trash directory.
void TestSharedTrashPath(const fs::path &root) {
  const auto one = root / "trash-one/note", two = root / "trash-two/note";
  fs::create_directory(one.parent_path());
  fs::create_directory(two.parent_path());
  std::ofstream(one) << "first";
  std::ofstream(two) << "second";
  const auto trashedOne = MoveToTrash(one.string());
  const auto trashedTwo = MoveToTrash(two.string());
  BString pathOne, pathTwo;
  Check(BNode(trashedOne.c_str()).ReadAttrString("_trk/original_path", &pathOne) == B_OK &&
            BNode(trashedTwo.c_str()).ReadAttrString("_trk/original_path", &pathTwo) == B_OK &&
            one.string() == pathOne.String() && two.string() == pathTwo.String(),
        "Tracker original-path strings are present and distinct on the native Trash volume");
  const auto restoredOne = RestoreFromTrash(trashedOne);
  const auto restoredTwo = RestoreFromTrash(trashedTwo);
  Check(restoredOne == one && restoredTwo == two && Read(one) == "first" && Read(two) == "second",
        "two independent files of the same name restore to their own folders");
  const char *fixture = getenv("LISTI_DELETE_HARDLINK_FIXTURE");
  if (!fixture)
    return;
  const auto base = fs::path(fixture) / "complete";
  const auto first = base / "selected/z", second = base / "selected/a", third = base / "outside";
  struct stat before{}, after{}, a{}, b{};
  const bool realLinks = !lstat(third.c_str(), &before) && S_ISREG(before.st_mode) && before.st_nlink == 3 &&
      !lstat(first.c_str(), &a) && !lstat(second.c_str(), &b) &&
      a.st_dev == before.st_dev && b.st_dev == before.st_dev &&
      a.st_ino == before.st_ino && b.st_ino == before.st_ino;
  Check(realLinks, "three fixture names refer to one inode before Trash");
  if (!realLinks)
    return;
  const auto contents = Read(third);
  BNode node(third.c_str());
  const char *attribute = "_trk/original_path";
  BString sentinel("/listi-test/previous-location"), originalPath, observedPath;
  attr_info info{};
  const auto initial = node.GetAttrInfo(attribute, &info);
  // Retain an existing caller-supplied sentinel. On a fresh disposable fixture
  // try to seed one, and observe whether this driver really stores the string.
  const auto written = initial == B_ENTRY_NOT_FOUND ? node.WriteAttrString(attribute, &sentinel) : initial;
  const auto found = node.GetAttrInfo(attribute, &info);
  const auto previous = node.ReadAttrString(attribute, &originalPath);
  if (found == B_OK) {
    Check(written == B_OK && info.type == B_STRING_TYPE && previous == B_OK &&
              (initial == B_OK || originalPath == sentinel),
          "three-link volume: Tracker original-path attribute verified present");
  } else {
    Check(initial == B_ENTRY_NOT_FOUND && written != B_OK && found == B_ENTRY_NOT_FOUND &&
              previous == B_ENTRY_NOT_FOUND,
          "three-link volume: Tracker original-path attribute verified absent");
  }
  // Attribute setup can change ctime. Capture the baseline for the two refusals now.
  if (lstat(third.c_str(), &before))
    throw std::runtime_error("Cannot recapture three-link fixture after attribute probe");
  for (const auto &path : {first, second}) {
    bool refused = false;
    try {
      CheckTrashSource(path.string());
    } catch (const std::runtime_error &error) {
      refused = std::string(error.what()).find("hard links") != std::string::npos;
    }
    const auto result = Run({Action::Trash, {path.string()}, {}}, {});
    Check(refused && !result.completed && !result.errors.empty() &&
              result.errors.front().find("hard links") != std::string::npos,
          "Trash preview and worker refuse a shared original-path attribute");
  }
  Check(!lstat(third.c_str(), &after) && before.st_ino == after.st_ino &&
            before.st_nlink == after.st_nlink && before.st_mode == after.st_mode &&
            before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec &&
            fs::equivalent(first, third) && fs::equivalent(second, third) &&
            Read(third) == contents && node.GetAttrInfo(attribute, &info) == found &&
            node.ReadAttrString(attribute, &observedPath) == previous &&
            (previous != B_OK || originalPath == observedPath),
        "both selected names and the outside link retain bytes and metadata");
}

// The prepared image is disposable; validate the exact three links and do
// not delete a caller's directory unless it contains only these fixture names.
void TestDeleteHardlinks(const fs::path &root) {
  const char *prepared = getenv("LISTI_DELETE_HARDLINK_FIXTURE");
  for (const std::string scenario : {"complete", "cancel", "replace", "edit"}) {
    const auto base = (prepared ? fs::path(prepared) : root) / scenario;
    const auto selected = base / "selected", first = selected / "z", second = selected / "a";
    const auto outside = base / "outside";
    if (!prepared) {
      fs::create_directories(selected);
      std::ofstream(outside) << "shared bytes";
      if (link(outside.c_str(), first.c_str())) {
        fs::remove_all(base);
        std::cout << "NOTE hard-link deletion needs the prepared volume on this filesystem\n";
        break;
      }
      if (link(outside.c_str(), second.c_str()))
        throw std::runtime_error("Cannot make second fixture link");
    }
    struct stat a, z, external;
    if (!fs::is_directory(fs::symlink_status(selected)) ||
        std::distance(fs::directory_iterator(selected), fs::directory_iterator()) != 2 ||
        lstat(first.c_str(), &z) || lstat(second.c_str(), &a) || lstat(outside.c_str(), &external) ||
        !S_ISREG(z.st_mode) || z.st_nlink != 3 || z.st_ino != a.st_ino ||
        z.st_dev != a.st_dev || z.st_ino != external.st_ino || z.st_dev != external.st_dev ||
        Read(outside) != "shared bytes")
      throw std::runtime_error("Invalid disposable hard-link fixture: " + selected.string());
    bool interrupted = false;
    Hooks hooks;
    hooks.progress = [&](const std::string &path, uint64_t) {
      if (path != first.string())
        return;
      if (scenario == "cancel")
        interrupted = true;
      if (scenario == "edit")
        std::ofstream(outside, std::ios::app) << " changed";
      if (scenario == "replace") {
        std::ofstream(base / "replacement") << "new bytes";
        fs::rename(base / "replacement", second);
      }
    };
    hooks.proceed = [&] { return !interrupted; };
    const auto r = Run({Action::Delete, {selected.string()}, ""}, hooks);
    const bool complete = scenario == "complete";
    Check(r.completed == int(complete) && r.cancelled == (scenario == "cancel") &&
          r.errors.empty() == (complete || interrupted) &&
          r.partialDeletions == int(!complete) && !fs::exists(first) &&
          fs::exists(selected) != complete && fs::exists(second) != complete &&
          Has(r, first, "", "completed") &&
          (complete ? Has(r, second, "", "completed") && Has(r, selected, "", "completed")
                    : Has(r, selected, "", "partial") &&
                      Has(r, second, "", interrupted ? "unprocessed" : "failed")) &&
          Read(outside) == (scenario == "edit" ? "shared bytes changed" : "shared bytes") &&
          (complete || Read(second) == (scenario == "replace" ? "new bytes" : Read(outside))),
          ("delete hard links: " + scenario + ", outside link and accurate receipt").c_str());
    if (!prepared)
      fs::remove_all(base);
  }
  const auto independent = root / "independent";
  fs::create_directory(independent);
  std::ofstream(independent / "a") << "one";
  std::ofstream(independent / "z") << "two";
  const auto normal = Run({Action::Delete, {independent.string()}, ""}, {});
  Check(normal.completed == 1 && normal.errors.empty() && !fs::exists(independent),
        "deleting independent files still completes");
}

void TestFileClaims(const fs::path &parent, bool insensitive) {
  for (const auto action : {Action::Copy, Action::Move}) {
    const auto base = parent / "claim-matrix", dest = base / "dest";
    fs::create_directories(base / "one");
    fs::create_directories(base / "two");
    fs::create_directory(dest);
    const auto old = base / "one" / "FirstExistingItem.tmp", first = base / "one" / "Report.tmp";
    const auto second = base / "two" / "report.tmp";
    std::ofstream(old) << "replacement";
    std::ofstream(dest / old.filename()) << "old";
    std::ofstream(first) << "first report";
    std::ofstream(second) << "second report";
    int prompts = 0, metadataChoices = 0;
    Hooks repeat;
    // Haiku exposes synthetic attributes even for these freshly created FAT
    // sources. Consent to bytes-only Copy just as the normal UI requires.
    repeat.confirmFatCopy = [&](const auto &, const auto &) {
      ++metadataChoices;
      return insensitive && action == Action::Copy;
    };
    repeat.conflict = [&](const auto &) { ++prompts; return Decision::ReplaceAll; };
    const auto job = PrepareTransfer({action, {old.string(), first.string(), second.string()}, dest.string()});
    const auto r = Run(job, repeat);
    const bool move = action == Action::Move;
    const bool fatCopy = insensitive && !move;
    Check(prompts == 1 && metadataChoices == int(fatCopy) && r.copyLimitations == fatCopy &&
              r.completed == (insensitive ? 2 : 3) &&
              r.errors.empty() == !insensitive && Read(dest / old.filename()) == "replacement" &&
              Read(dest / first.filename()) == "first report" &&
              fs::exists(first) != move && (insensitive ? Read(second) == "second report" &&
                    Has(r, second, dest / second.filename(), "failed", "already delivered")
                  : Read(dest / second.filename()) == "second report" && fs::exists(second) != move),
          "Replace All cannot discard the earlier file; distinct destination entries still succeed");
    if (insensitive) {
      const auto again = Run(PrepareTransfer({action, {second.string()}, dest.string()}), repeat);
      Check(!again.completed && again.errors.size() == 1 && Read(second) == "second report" &&
                Read(dest / first.filename()) == "first report",
            "remaining-file retry cannot replace a differently spelled stored name");
      Hooks keep;
      keep.confirmFatCopy = repeat.confirmFatCopy;
      keep.copyConflict = [](const CopyConflict &choice) {
        return choice.separateOnly ? Decision::KeepBoth : Decision::Abort;
      };
      const auto recovered = Run(PrepareTransfer({action, {second.string()}, dest.string()}), keep);
      Check(recovered.completed == 1 && recovered.errors.empty() &&
                metadataChoices == (fatCopy ? 2 : 0) && recovered.copyLimitations == fatCopy &&
                Read(dest / "report (2).tmp") == "second report" &&
                Read(dest / first.filename()) == "first report",
            "retry under a separate name delivers the remaining file without losing the first");
    }
    fs::remove_all(base);
  }
}
void TestClaimedKeepBoth(const fs::path &root) {
  const auto base = root / "claim-keep", named = base / "dest", prior = base / "one" / "item";
  const auto second = base / "two" / "item", occupied = named / "item";
  fs::create_directories(prior.parent_path());
  fs::create_directories(second.parent_path());
  fs::create_directory(named);
  std::ofstream(prior) << "first";
  std::ofstream(second) << "second";
  std::ofstream(occupied) << "old";
  bool offered = false;
  Hooks hooks;
  hooks.conflict = [](const auto &) { return Decision::ReplaceAll; };
  hooks.copyConflict = [&](const CopyConflict &choice) {
    offered = choice.separateOnly && choice.proposed == (named / "item (2)").string();
    return Decision::KeepBoth;
  };
  // The UI preflight rejects equal top-level spellings. Exercise the worker's
  // exact same claim transition as Report/report on the optional real volume.
  const auto moved = Run({Action::Move, {prior.string(), second.string()}, named.string()}, hooks);
  Check(offered && moved.completed == 2 && moved.errors.empty() &&
            !fs::exists(prior) && !fs::exists(second) && Read(occupied) == "first" &&
            Read(named / "item (2)") == "second" &&
            Has(moved, second, named / "item (2)", "moved"),
        "even after Replace All, a second source can only use its own unoccupied target");
  fs::remove_all(base);
}
void TestDirectoryClaims(const fs::path &root, const fs::path &out) {
  for (const auto action : {Action::Copy, Action::Move}) {
    for (const bool existing : {false, true}) {
      const auto first = root / "first" / "tree", second = root / "second" / "tree";
      const auto target = out / "tree";
      fs::create_directories(first);
      fs::create_directories(second);
      std::ofstream(first / "one") << "one";
      std::ofstream(second / "two") << "two";
      if (existing) {
        fs::create_directory(target);
        std::ofstream(target / "keep") << "untouched";
      }
      const auto r = Run({action, {first.string(), second.string()}, out.string()}, {});
      Check(r.completed == 1 && r.errors.size() == 1 &&
                Has(r, second, target, "failed", "already used by source folder") &&
                Read(target / "one") == "one" && !fs::exists(target / "two") &&
                Read(second / "two") == "two" &&
                fs::exists(first) == (action == Action::Copy) &&
                (!existing || Read(target / "keep") == "untouched"),
            "two selected roots cannot silently share a new or existing destination folder");
      fs::remove_all(first.parent_path());
      fs::remove_all(second.parent_path());
      fs::remove_all(target);
    }
  }
  const auto source = root / "set";
  fs::create_directories(source / "Drafts" / "one");
  fs::create_directories(source / "drafts" / "two");
  auto run = [&](const fs::path &parent, bool insensitive) {
    const auto result = Run({Action::Copy, {source.string()}, parent.string()}, {});
    const auto tree = parent / "set";
    if (insensitive) {
      const bool one = fs::is_directory(tree / "Drafts" / "one");
      const bool two = fs::is_directory(tree / "drafts" / "two");
      Check(!result.completed && result.partialCopies == 1 && result.errors.size() == 1 &&
                Has(result, source, tree, "partial", "already used by source folder") &&
                one != two && fs::is_directory(source / "Drafts" / "one") &&
                fs::is_directory(source / "drafts" / "two"),
            "case-insensitive target preserves the first sibling and reports partial copy");
    } else {
      Check(result.completed == 1 && result.errors.empty() &&
                fs::is_directory(tree / "Drafts" / "one") &&
                fs::is_directory(tree / "drafts" / "two") &&
                !fs::exists(tree / "Drafts" / "two") && !fs::exists(tree / "drafts" / "one"),
            "case-sensitive target preserves both distinct sibling folders");
    }
  };
  run(out, false);
  fs::remove_all(out / "set");
  if (const char *volume = getenv("LISTI_CASE_INSENSITIVE_TEST_ROOT")) {
    std::string scratch = (fs::path(volume) / "Listi-alias-XXXXXX").string();
    if (!mkdtemp(scratch.data()))
      throw std::runtime_error("Cannot create case-insensitive filesystem fixture");
    const fs::path test(scratch);
    fs::create_directory(test / "MiXeD");
    Check(fs::equivalent(test / "MiXeD", test / "mixed"),
          "case-insensitive fixture resolves both spellings to one real inode");
    run(test, true);
    run(test, true); // The first delivery must survive a retry.
    // Each Run creates a fresh worker. A restarted app has no claims from
    // the old job; the persisted spelling itself must guard these retries.
    fs::remove_all(test / "set");
    const auto first = source / "Drafts", other = source / "drafts";
    auto attempt = [&](std::vector<std::string> paths, const Hooks &h = {}) {
      return Run(PrepareTransfer({Action::Copy, std::move(paths), test.string()}), h);
    };
    auto firstJob = attempt({first.string(), other.string()});
    auto reversed = attempt({other.string(), first.string()});
    auto remaining = attempt({other.string()});
    Check(firstJob.completed == 1 && firstJob.errors.size() == 1 &&
              reversed.completed == 1 && reversed.errors.size() == 1 &&
              !remaining.completed && remaining.errors.size() == 1 &&
              fs::is_directory(test / "Drafts" / "one") && !fs::exists(test / "Drafts" / "two"),
          "opposite-order and remaining-only fresh jobs never merge a differently spelled folder");
    Hooks separate;
    separate.copyConflict = [&](const CopyConflict &request) {
      Check(request.separateOnly && request.source == other.string(),
            "directory alias offers a separate name without Replace");
      return Decision::KeepBoth;
    };
    const auto chosen = attempt({other.string()}, separate);
    Check(chosen.completed == 1 && chosen.errors.empty() &&
              fs::is_directory(test / "drafts (2)" / "two") &&
              !fs::exists(test / "Drafts" / "two") && !fs::exists(test / "drafts (2)" / "one"),
          "explicit separate name preserves both directory trees on the real target");
    const auto nested = root / "siblings";
    fs::create_directory(nested);
    std::ofstream(nested / "Report.tmp") << "upper";
    std::ofstream(nested / "report.tmp") << "lower";
    Hooks nestedConflict;
    nestedConflict.conflict = [](const auto &) { return Decision::ReplaceAll; };
    const auto files = Run(PrepareTransfer({Action::Copy, {nested.string()}, test.string()}), nestedConflict);
    const auto upper = nested / "Report.tmp", lower = nested / "report.tmp";
    const auto upperDst = test / "siblings" / "Report.tmp", lowerDst = test / "siblings" / "report.tmp";
    const bool deliveredUpper = Read(upperDst) == "upper";
    Check(!files.completed && files.partialCopies == 1 && files.errors.size() == 1 &&
              Has(files, nested, test / "siblings", "partial") &&
              Has(files, deliveredUpper ? upper : lower, deliveredUpper ? upperDst : lowerDst, "copied") &&
              Has(files, deliveredUpper ? lower : upper, deliveredUpper ? lowerDst : upperDst,
                  "failed", "already delivered") &&
              Read(lowerDst) == (deliveredUpper ? "upper" : "lower") &&
              Read(upper) == "upper" && Read(lower) == "lower",
          "sibling files in one local copy retain first publication and a partial folder receipt");
    fs::remove_all(nested);
    TestFileClaims(test, true);
    fs::remove_all(test);
  } else {
    std::cout << "NOTE separate case-insensitive volume not configured\n";
  }
  fs::remove_all(source);
}
void TestMovePermissions(const fs::path &root, const fs::path &out) {
  const auto source = root / "permissions", destination = out / "permissions";
  for (bool nested : {false, true}) {
    for (const auto modes : {std::pair<mode_t, mode_t>{0700, 0755}, {0755, 0700},
                             {0740, 0750}, {0750, 0750}}) {
      const auto from = nested ? source / "sub" : source;
      const auto to = nested ? destination / "sub" : destination;
      fs::create_directories(from);
      fs::create_directories(to);
      if (chmod(from.c_str(), modes.first) || chmod(to.c_str(), modes.second))
        throw std::runtime_error("Cannot set move permission fixture");
      if (nested)
        std::ofstream(source / "outside") << "sibling";
      std::ofstream(from / "memo") << "private";
      std::ofstream(to / "keep") << "unrelated";
      const bool matched = modes.first == modes.second;
      if (modes.first == 0700 && modes.second == 0755) {
        const Job copy{Action::Copy, {source.string()}, out.string()};
        for (int attempt = 0; attempt < 2; ++attempt) {
          // A previous attempt may have copied the public sibling first.
          // Accept that conflict so retry actually reaches the private folder.
          Hooks retry;
          retry.conflict = [](const auto &) { return Decision::Replace; };
          const auto blocked = Run(copy, retry);
          Check(!blocked.completed && !blocked.errors.empty() && !fs::exists(to / "memo") &&
                    Read(from / "memo") == "private" && Read(to / "keep") == "unrelated",
                "ordinary directory copy and retry do not expose private children in a public merge");
        }
        // The independent Move case below starts with the original destination.
        if (nested)
          fs::remove(destination / "outside");
      }
      const Job move{Action::Move, {source.string()}, out.string()};
      auto verify = [&](const Result &r) {
        struct stat node;
        return r.completed == int(matched) && r.errors.empty() == matched &&
               (matched ? !fs::exists(source) && Read(to / "memo") == "private"
                        : Read(from / "memo") == "private" && !fs::exists(to / "memo") &&
                          Has(r, from, to, "failed", "move permissions differ")) &&
               (!nested || fs::exists(destination / "outside") == matched) &&
               Read(to / "keep") == "unrelated" && !stat(to.c_str(), &node) &&
               (node.st_mode & 07777) == modes.second;
      };
      Check(verify(Run(move, {})), "directory merge requires matching access without changing prior files");
      if (!matched)
        Check(verify(Run(move, {})), "retry of refused directory merge leaves both trees intact");
      fs::remove_all(source);
      fs::remove_all(destination);
    }
  }
}
void TestNameFlush(const fs::path &root, const fs::path &out) {
  for (const auto action : {Action::MakeFile, Action::MakeDirectory, Action::Link,
                            Action::Rename, Action::Move}) {
    const auto src = root / "flush-name", dst = out / "flush-name";
    const bool create = action == Action::MakeFile || action == Action::MakeDirectory;
    const bool move = action == Action::Rename || action == Action::Move;
    if (!create)
      std::ofstream(src) << "payload";
    Job job{action, {}, out.string()};
    if (!create)
      job.sources.push_back(src.string());
    FreezeLocalJob(job, out.string());
    if (action == Action::MakeFile || action == Action::MakeDirectory || action == Action::Rename)
      job.destination = dst.string();
    if (action == Action::Link)
      job.entryName = "flush-name";
    struct stat parent;
    if (stat(out.c_str(), &parent))
      throw std::runtime_error("Cannot identify flush target");
    directoryForSync = parent.st_ino;
    failDirectorySync = true;
    Hooks hooks;
    hooks.report = std::make_shared<JobReportStore>();
    const auto failed = Run(job, hooks);
    failDirectorySync = false;
    hooks.report->Seal();
    const auto page = hooks.report->Page(0, 5);
    const std::string selected = create ? "" : src.string();
    Check(!failed.completed && page.errors.size() == 1 && fs::exists(dst) &&
              page.outcomes.size() == 1 && page.outcomes.front().source == selected &&
              page.outcomes.front().destination == dst.string() &&
              OutcomeLabel(page.outcomes.front().status) == "Published; directory flush failed" &&
              page.outcomes.front().detail.find("name change already applied") != std::string::npos &&
              (!move || !fs::exists(src)),
          "failed parent flush reports the applied name, not a successful job or an intact move source");
    const auto retried = Run(job, {});
    Check(!retried.completed && !retried.errors.empty() && fs::exists(dst) &&
              (create || Read(dst) == "payload"),
          "retry does not overwrite a previously applied name");
    fs::remove(dst);
    if (!create)
      fs::remove(src);
  }
}
}
int main() {
  char temp[] = "/boot/home/Listi-operation-faults-XXXXXX";
  if (!mkdtemp(temp))
    return 2;
  const auto root = fs::canonical(temp);
  const auto out = root / "out", source = root / "file", target = out / "file";
  fs::create_directory(out);
  TestSharedTrashPath(root);
  TestDeleteHardlinks(root);
  TestFileClaims(root, false);
  TestClaimedKeepBoth(root);
  TestDirectoryClaims(root, out);
  TestMovePermissions(root, out);
  TestNameFlush(root, out);
  {
    const auto inputs = root / "skip-inputs", outputs = root / "skip-outputs";
    fs::create_directory(inputs);
    fs::create_directory(outputs);
    Job copy{Action::Copy, {}, outputs.string()};
    for (int index = 0; index < 128; ++index) {
      const auto name = std::to_string(index);
      copy.sources.push_back((inputs / name).string());
      std::ofstream(inputs / name) << "source";
      std::ofstream(outputs / name) << "target";
    }
    int prompts = 0, scansAtChoice = 0;
    Hooks hooks;
    hooks.conflict = [&](const std::string &) {
      ++prompts;
      scansAtChoice = exactNameScans;
      return Decision::SkipAll;
    };
    const auto skipped = Run(copy, hooks);
    bool intact = true;
    for (const auto &source : copy.sources)
      intact &= Read(source) == "source" && Read(outputs / fs::path(source).filename()) == "target";
    Check(skipped.skipped == 128 && !skipped.completed && skipped.errors.empty() &&
              prompts == 1 && exactNameScans == scansAtChoice && intact,
          "Skip All leaves all bytes intact without enumerating later conflict names");
  }
  const std::string payload(1000, 'x');
  std::ofstream(source) << payload;
  std::ofstream(target) << "original";
  {
    const auto incoming = root / "new.txt", destination = out / "new.txt";
    std::ofstream(incoming) << "incoming";
    const Job copy{Action::Copy, {incoming.string()}, out.string()};
    claimNewLeaf = destination.filename().string();
    const auto raced = Run(copy, {});
    claimNewLeaf.clear();
    Check(newLeafClaimed && !raced.completed && !raced.errors.empty() &&
              Read(destination) == "competitor" && Read(incoming) == "incoming",
          "ordinary copy reserves an unused leaf without overwriting a concurrent arrival");
    fs::remove(destination);
    const auto clean = Run(copy, {});
    Check(clean.completed == 1 && clean.errors.empty() &&
              Read(destination) == "incoming" && Read(incoming) == "incoming",
          "ordinary copy to a new leaf publishes the complete bytes");
    fs::remove(destination);
    fs::remove(incoming);
  }
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
    reportFat = synthesizeStageMode = true;
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
    // This second scenario isolates the metadata policy: Move must copy the
    // attribute rather than borrowing the ordinary Copy waiver.
    synthesizeStageMode = false;
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

  {
    const char *hardlinkVolume = getenv("LISTI_HARDLINK_TEST_ROOT");
    const char *preparedPair = getenv("LISTI_HARDLINK_TEST_PAIR");
    const fs::path parent = preparedPair ? fs::path(preparedPair)
                            : hardlinkVolume ? fs::path(hardlinkVolume) : root;
    if (preparedPair) {
      struct stat a{}, b{};
      const auto entry = parent / "entry", sibling = parent / "sibling";
      if (lstat(entry.c_str(), &a) || lstat(sibling.c_str(), &b) ||
          !S_ISREG(a.st_mode) || !S_ISREG(b.st_mode) || a.st_nlink < 2 ||
          a.st_dev != b.st_dev || a.st_ino != b.st_ino ||
          Read(entry) != "source bytes" || Read(sibling) != "source bytes") {
        std::cerr << "Invalid prepared hard-link pair: " << parent << '\n';
        return 2;
      }
    }
    std::string scratch = (parent / "Listi-hardlink-XXXXXX").string();
    if (!mkdtemp(scratch.data()))
      return 2;
    const fs::path base(scratch), destination = base / "out";
    const auto fixture = preparedPair ? parent : base;
    const auto selected = fixture / "entry", sibling = fixture / "sibling";
    const auto existing = destination / "entry";
    fs::create_directory(destination);
    if (!preparedPair)
      std::ofstream(selected) << "source bytes";
    std::ofstream(existing) << "destination bytes";
    const bool realLinks = preparedPair || link(selected.c_str(), sibling.c_str()) == 0;
    if (!realLinks) {
      const int error = errno;
      // Haiku libroot linkat translates a driver's B_UNSUPPORTED into EPERM.
      Check(!hardlinkVolume && (error == B_UNSUPPORTED || error == ENOTSUP ||
                                error == EOPNOTSUPP || error == ENOSYS || error == EPERM),
            "hard link creation only skipped for an unsupported default volume");
      std::cout << "NOTE no real hard-link coverage on " << base << ": "
                << strerror(error) << "; exercising simulated link count instead\n";
      simulatedHardlink = selected.string();
    }

    struct stat original{}, other{}, current{}, targetAfter{}, siblingAfter{};
    const bool recorded = stat(selected.c_str(), &original) == 0 &&
                          stat(existing.c_str(), &other) == 0;
    const auto refused = Run({Action::Move, {selected.string()}, destination.string()}, replace);
    simulatedHardlink.clear();
    const bool retained = recorded && !refused.completed && !refused.errors.empty() &&
              refused.errors.front().find("hard-linked") != std::string::npos &&
              !stat(selected.c_str(), &current) && current.st_dev == original.st_dev &&
              current.st_ino == original.st_ino && Read(selected) == "source bytes" &&
              !stat(existing.c_str(), &targetAfter) && targetAfter.st_dev == other.st_dev &&
              targetAfter.st_ino == other.st_ino && Read(existing) == "destination bytes";
    Check(retained, "replacement move refuses a multiply linked source before altering either file");
    if (preparedPair && !retained)
      return 1; // Leave the test paths intact for inspection.
    if (realLinks) {
      Check(!stat(sibling.c_str(), &siblingAfter) && siblingAfter.st_dev == original.st_dev &&
                siblingAfter.st_ino == original.st_ino && Read(sibling) == "source bytes",
            "outside hard link still shares the original source inode and bytes");
      fs::remove(existing);
      const auto moved = Run({Action::Move, {selected.string()}, destination.string()}, {});
      Check(moved.completed == 1 && moved.errors.empty() && !fs::exists(selected) &&
                !stat(existing.c_str(), &targetAfter) && targetAfter.st_dev == original.st_dev &&
                targetAfter.st_ino == original.st_ino && Read(existing) == "source bytes" &&
                !stat(sibling.c_str(), &siblingAfter) && siblingAfter.st_dev == original.st_dev &&
                siblingAfter.st_ino == original.st_ino && Read(sibling) == "source bytes",
            "moving to a free name on the same volume preserves the linked inode");
      if (preparedPair) {
        // Return this exact link to its original, now vacant name. Never
        // recursively clean a directory containing a caller-supplied link.
        if (moved.completed != 1 || !moved.errors.empty() ||
            lstat(existing.c_str(), &current) || current.st_dev != original.st_dev ||
            current.st_ino != original.st_ino || !lstat(selected.c_str(), &current) ||
            errno != ENOENT)
          return 1;
        const auto restored = Run({Action::Move, {existing.string()}, parent.string()}, {});
        if (restored.completed != 1 || !restored.errors.empty() ||
            lstat(selected.c_str(), &current) || current.st_dev != original.st_dev ||
            current.st_ino != original.st_ino || Read(selected) != "source bytes" ||
            lstat(sibling.c_str(), &siblingAfter) || siblingAfter.st_dev != original.st_dev ||
            siblingAfter.st_ino != original.st_ino || Read(sibling) != "source bytes")
          return 1;
      }
    }
    if (preparedPair) {
      fs::remove(destination);
      fs::remove(base);
    } else {
      fs::remove_all(base);
    }
  }

  {
    // An existing root forces a copy-and-cleanup merge. Cover an existing
    // file, an exclusively published new child, a new directory and a link.
    const auto incoming = root / "move-owners", merged = out / "move-owners";
    fs::create_directories(incoming / "sub");
    fs::create_directory(merged);
    std::ofstream(incoming / "replacement") << "new";
    std::ofstream(merged / "replacement") << "old";
    std::ofstream(incoming / "sub" / "new") << "nested";
    fs::create_symlink("replacement", incoming / "link");
    const uid_t owner = geteuid() == 0 ? 1003 : geteuid();
    const gid_t group = geteuid() == 0 ? 2003 : getegid();
    const auto metadata = [&](const fs::path &path, mode_t mode) {
      struct stat st;
      return !lstat(path.c_str(), &st) && st.st_uid == owner &&
             st.st_gid == group && (st.st_mode & 07777) == mode;
    };
    for (const auto &path : {incoming / "replacement", incoming / "sub" / "new",
                             incoming / "sub", incoming / "link"})
      if (geteuid() == 0 && lchown(path.c_str(), owner, group))
        return 2;
    if (chmod((incoming / "replacement").c_str(), 04754) ||
        chmod((incoming / "sub").c_str(), 03755) ||
        chmod((incoming / "sub" / "new").c_str(), 02755))
      return 2;
    const auto copied = Run({Action::Move, {incoming.string()}, out.string()}, replace);
    struct stat linkInfo;
    Check(copied.completed == 1 && copied.errors.empty() && !fs::exists(incoming) &&
              Read(merged / "replacement") == "new" && Read(merged / "sub" / "new") == "nested" &&
              metadata(merged / "replacement", 04754) && metadata(merged / "sub", 03755) &&
              metadata(merged / "sub" / "new", 02755) &&
              !lstat((merged / "link").c_str(), &linkInfo) && S_ISLNK(linkInfo.st_mode) &&
              linkInfo.st_uid == owner && linkInfo.st_gid == group &&
              fs::read_symlink(merged / "link") == "replacement",
          "same-volume copy-and-cleanup preserves moved owners and special bits");

    fs::create_directory(incoming);
    std::ofstream(incoming / "unmoved") << "source";
    if (chmod(incoming.c_str(), 01755) || chmod(merged.c_str(), 0755))
      return 2;
    const auto refused = Run({Action::Move, {incoming.string()}, out.string()}, replace);
    Check(!refused.completed && !refused.errors.empty() &&
              Read(incoming / "unmoved") == "source" && !fs::exists(merged / "unmoved"),
          "move refuses conflicting merged directory metadata before copying children");
    fs::remove_all(incoming);
    fs::remove_all(merged);
  }

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

  // Swap a directory after preview, before execution of the confirmed snapshot.
  {
    const auto project = root / "project", original = root / "old-project", note = project / "note";
    fs::create_directory(project);
    std::ofstream(note) << payload;
    const auto trashPath = MoveToTrash(note.string());
    Job restore{Action::Restore, {trashPath}, {}};
    FreezeLocalJob(restore);
    const auto identity = restore.expectedSources.at(trashPath);
    restore.restoreTargets[trashPath] = CaptureRestoreTarget(trashPath, identity);
    auto encoded = EncodeJob(restore, 'test');
    auto submitted = DecodeSubmittedJob(encoded, {Action::Restore});
    BMessage incomplete;
    encoded.FindMessage("restoreTarget", &incomplete);
    incomplete.RemoveName("parentNode");
    encoded.ReplaceMessage("restoreTarget", &incomplete);
    Check(!DecodeSubmittedJob(encoded, {Action::Restore}),
          "a restore confirmation without its parent identity is refused");
    fs::rename(project, original);
    fs::create_directory(project);
    Check(submitted.has_value(), "restore confirmation includes the original directory identity");
    const auto rejected = submitted ? Run(*submitted, {}) : Result{};
    Check(!rejected.completed && !rejected.errors.empty() && Read(trashPath) == payload &&
              !fs::exists(note) && !fs::exists(original / "note") &&
              OriginalTrashPath(trashPath, identity) == note.string(),
          "replaced project folder cannot receive a confirmed restore");
    restore.restoreTargets[trashPath] = CaptureRestoreTarget(trashPath, identity);
    const auto retry = Run(DecodeJob(EncodeJob(restore, 'test')), {});
    Check(retry.completed == 1 && retry.errors.empty() && !fs::exists(trashPath) &&
              Read(note) == payload && !fs::exists(original / "note"),
          "a fresh restore confirmation can target the replacement folder");
  }

  const auto trashed = MoveToTrash(source.string());
  Job guardedRestore{Action::Restore, {trashed}, {}};
  FreezeLocalJob(guardedRestore);
  const auto shown = OriginalTrashPath(trashed, guardedRestore.expectedSources.at(trashed));
  Check(shown == source.string(), "restore preview identifies exact original path");
  guardedRestore.restoreTargets[trashed] = CaptureRestoreTarget(trashed, guardedRestore.expectedSources.at(trashed));
  guardedRestore.restoreTargets.at(trashed).path += "-different";
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
