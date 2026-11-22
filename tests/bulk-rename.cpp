// SPDX-License-Identifier: MIT
#include "BulkRename.h"
#include "JobSnapshot.h"
#include <Application.h>
#include <array>
#include <Node.h>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace Listi;

namespace {
int renameCalls = 0, failRename = 0, stopBefore = 0, stopAfter = 0;
std::string blockRestore;
bool simulateLarge = false;
}
extern "C" int __real_renameat(int, const char *, int, const char *);
extern "C" int __wrap_renameat(int from, const char *before, int to,
                                const char *after) {
  if (++renameCalls == failRename) {
    if (!blockRestore.empty()) {
      int fd = openat(to, blockRestore.c_str(), O_WRONLY | O_CREAT | O_EXCL,
                      0600);
      if (fd < 0 || write(fd, "other", 5) != 5)
        abort();
      close(fd);
    }
    errno = EIO;
    return -1;
  }
  if (renameCalls == stopBefore)
    _exit(21);
  const auto status = __real_renameat(from, before, to, after);
  if (!status && renameCalls == stopAfter)
    _exit(21);
  return status;
}
// BFS may allocate blocks for extending a file. Simulate an oversized
// entry in the worker's stat call rather than consuming 512 MiB on disk.
extern "C" int __real_fstatat(int, const char *, struct stat *, int);
extern "C" int __wrap_fstatat(int fd, const char *name, struct stat *st,
                               int flags) {
  const int status = __real_fstatat(fd, name, st, flags);
  if (!status && simulateLarge && std::string(name) == "oversized")
    st->st_size = 512ll * 1024 * 1024 + 1;
  return status;
}

namespace {
void Check(bool ok, const char *label) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", label);
  if (!ok)
    throw std::runtime_error(label);
}
void Write(const std::string &path, const std::string &text) {
  std::ofstream file(path);
  if (!(file << text))
    throw std::runtime_error("Cannot write fixture: " + path);
}
std::string Read(const std::string &path) {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), {}};
}
bool Exists(const std::string &path) {
  struct stat st;
  return lstat(path.c_str(), &st) == 0;
}
template <class F> bool Reject(F action) {
  try {
    action();
    return false;
  } catch (const std::runtime_error &) {
    return true;
  }
}
// Enumerate all assignments of three source names to five usable names
// and one unrelated occupied name. Verify the actual directory after every
// operation, including the unchanged rows of a mixed batch.
void CheckMappings(const std::string &folder) {
  if (mkdir(folder.c_str(), 0700))
    throw std::runtime_error("Cannot create mapping fixture");
  const std::array<std::string, 6> names{
      "été.txt", "blå", ".nom", "新", "two words.txt", "occupied"};
  const std::vector<std::string> sources{Join(folder, names[0]),
                                         Join(folder, names[1]),
                                         Join(folder, names[2])};
  const auto occupied = Join(folder, names[5]);
  for (size_t i = 0; i < sources.size(); ++i)
    Write(sources[i], std::to_string(i));
  Write(occupied, "keep");
  const auto snapshot = [&] {
    std::map<std::string, std::string> files;
    for (const auto &entry : std::filesystem::directory_iterator(folder))
      files.emplace(entry.path().filename().string(), Read(entry.path().string()));
    return files;
  };
  const auto original = snapshot();
  const auto batch = FreezeBulkRename(folder, sources);
  for (size_t a = 0; a < names.size(); ++a) {
    for (size_t b = 0; b < names.size(); ++b) {
      for (size_t c = 0; c < names.size(); ++c) {
        BulkRenameOptions options;
        options.overrides = {{sources[0], names[a]}, {sources[1], names[b]},
                             {sources[2], names[c]}};
        const auto preview = PreviewBulkRename(batch, options);
        const int changes = (a != 0) + (b != 1) + (c != 2);
        const bool valid = a < 5 && b < 5 && c < 5 &&
                           a != b && b != c && a != c && changes;
        const auto fail = [&] {
          throw std::runtime_error("Unexpected mapping result: " + names[a] +
                                   ", " + names[b] + ", " + names[c]);
        };
        if (preview.CanApply() != valid)
          fail();
        if (a == 1 && b == 2 && c == 5 && preview.blocked != 3)
          fail();
        auto result = Run(preview.job, {});
        if (valid) {
          const std::map<std::string, std::string> expected{
              {names[a], "0"}, {names[b], "1"}, {names[c], "2"}, {names[5], "keep"}};
          if (result.completed != changes || !result.errors.empty() ||
              !result.bulkRenameUndo || snapshot() != expected)
            fail();
          const auto inverse = Run(*result.bulkRenameUndo, {});
          if (inverse.completed != changes || !inverse.errors.empty())
            fail();
        } else if (result.completed || result.errors.empty()) {
          fail();
        }
        if (snapshot() != original)
          fail();
      }
    }
  }
  for (const auto &source : sources)
    unlink(source.c_str());
  unlink(occupied.c_str());
  rmdir(folder.c_str());
  Check(true, "216 rename mappings preserve exact contents, diagnose blocked chains and undo all valid changes");
}
void CheckCrash(const std::string &folder, int boundary, bool after) {
  std::filesystem::create_directory(folder);
  const auto a = Join(folder, "a"), b = Join(folder, "b");
  Write(a, "first");
  Write(b, "second");
  BulkRenameOptions rules;
  rules.overrides = {{a, "b"}, {b, "a"}};
  const auto swap = PreviewBulkRename(FreezeBulkRename(folder, {a, b}), rules);
  const pid_t child = fork();
  if (child == 0) {
    renameCalls = 0;
    (after ? stopAfter : stopBefore) = boundary;
    Run(swap.job, {});
    _exit(22);
  }
  int status;
  if (child < 0 || waitpid(child, &status, 0) != child ||
      !WIFEXITED(status) || WEXITSTATUS(status) != 21)
    throw std::runtime_error("Crash fixture missed the expected boundary");
  std::string record, stage;
  for (const auto &entry : std::filesystem::directory_iterator(folder)) {
    const auto path = entry.path().string();
    if (Leaf(path).rfind(".listi-recovery-rename-", 0) == 0)
      record = Read(path);
    else if (Leaf(path).rfind(".listi-rename-", 0) == 0)
      stage = Read(path);
  }
  const int moves = boundary - (after ? 0 : 1);
  Check(record.find("Review only") != std::string::npos &&
            record.find("intent ") != std::string::npos &&
            record.find("committed") == std::string::npos &&
            Read(a) == (moves == 0 ? "first" : moves == 1 ? "" : "second") &&
            Read(b) == (moves < 2 ? "second" : moves == 2 ? "" : "first") &&
            stage == ((moves == 1 || moves == 2) ? "first" : ""),
        "process death retains a flushed intent and actual names without replay");
  std::filesystem::remove_all(folder);
}
} // namespace

int main() {
  BApplication app("application/x-vnd.Kambuzi-BulkRenameTests");
  char temp[] = "/boot/home/listi-bulk-tests-XXXXXX";
  if (!mkdtemp(temp))
    return 2;
  const std::string root(temp), folder = root + "/entries";
  try {
    CheckMappings(root + "/mappings");
    for (int boundary = 1; boundary <= 3; ++boundary) {
      CheckCrash(root + "/before", boundary, false);
      CheckCrash(root + "/after", boundary, true);
    }
    if (mkdir(folder.c_str(), 0700))
      throw std::runtime_error("Cannot create fixture directory");
    const auto a = folder + "/find find.txt", b = folder + "/find folder.v1",
               c = folder + "/.link";
    Write(a, "asset");
    if (mkdir(b.c_str(), 0700) || symlink("missing", c.c_str()))
      throw std::runtime_error("Cannot create folder or symbolic link fixture");
    const auto batch = FreezeBulkRename(folder, {b, a, c});
    Check(Reject([&] { FreezeBulkRename(folder, {a}); }) &&
              Reject([&] { FreezeBulkRename(folder, {a, a}); }) &&
              Reject([&] { FreezeBulkRename(folder, {a, root + "/other"}); }) &&
              Reject([&] { FreezeBulkRename("sftp://example.invalid", {a, b}); }),
          "bulk selection requires distinct sibling entries in a local folder");
    const auto unchanged = PreviewBulkRename(batch, {});
    Check(!unchanged.CanApply() && unchanged.changed == 0 &&
              unchanged.rows.size() == 3,
          "a preview without changes cannot be applied");

    BulkRenameOptions options;
    options.search = "find";
    options.replacement = "æ";
    options.prefix = "Pre ";
    options.suffix = " end";
    options.number = true;
    options.start = 4;
    options.digits = 2;
    const auto preview = PreviewBulkRename(batch, options);
    Check(preview.CanApply() && preview.rows[0].after == "Pre æ folder.v1 end 04" &&
              preview.rows[1].after == "Pre æ æ end 05.txt" &&
              preview.rows[2].after == "Pre .link end 06",
          "all rules compose in frozen order without changing file extensions");
    Check(Exists(a) && Exists(b) && Exists(c),
          "preview does not modify files, folders or symlinks");

    BulkRenameOptions wideNumber;
    wideNumber.number = true;
    wideNumber.start = std::numeric_limits<unsigned>::max();
    const auto wide = PreviewBulkRename(batch, wideNumber);
    const auto next = uint64_t(wideNumber.start) + 1;
    Check(wide.CanApply() &&
              wide.rows[1].after == "find find " + std::to_string(next) + ".txt",
          "numbering continues past the largest starting number without wrapping");

    options.prefix = "/";
    Check(!PreviewBulkRename(batch, options).CanApply(),
          "invalid new name blocks the full batch");
    options.prefix = std::string(260, 'x');
    Check(!PreviewBulkRename(batch, options).CanApply(),
          "overlong name blocks the full batch");

    const auto x = folder + "/filex", y = folder + "/filexx";
    Write(x, "x");
    Write(y, "y");
    BulkRenameOptions collapse;
    collapse.search = "x";
    auto collision = PreviewBulkRename(FreezeBulkRename(folder, {x, y}),
                                       collapse);
    Check(!collision.CanApply() && collision.blocked == 2,
          "duplicate generated name blocks both rows");
    collapse.replacement = "xx";
    collision = PreviewBulkRename(FreezeBulkRename(folder, {x, y}), collapse);
    Check(collision.CanApply() && collision.changed == 2,
          "preview accepts a chain ending at an unused name");
    const auto z = Join(folder, "filexxxx");
    auto chain = Run(collision.job, {});
    Check(chain.completed == 2 && chain.bulkRenameUndo && !Exists(x) &&
              Read(y) == "x" && Read(z) == "y",
          "chain moves the blocking source first without clobbering either file");
    Write(x, "other");
    auto undo = Run(*chain.bulkRenameUndo, {});
    Check(!undo.completed && !undo.errors.empty() && Read(x) == "other" &&
              Read(y) == "x" && Read(z) == "y",
          "undo refuses an unrelated entry at an original name");
    unlink(x.c_str());
    Write(y, "changed");
    undo = Run(*chain.bulkRenameUndo, {});
    Check(!undo.completed && !undo.errors.empty() && !Exists(x) &&
              Read(y) == "changed" && Read(z) == "y",
          "undo refuses content edits even when the renamed inode still exists");
    rename(y.c_str(), x.c_str());
    rename(z.c_str(), y.c_str());
    Write(x, "x");

    BulkRenameOptions swapNames;
    swapNames.overrides = {{x, Leaf(y)}, {y, Leaf(x)}};
    const auto swapPreview = PreviewBulkRename(FreezeBulkRename(folder, {x, y}),
                                               swapNames);
    Check(swapPreview.CanApply() && swapPreview.changed == 2,
          "exact per-row overrides preview a two-file swap");
    const auto swap = swapPreview.job;
    renameCalls = 0;
    failRename = 3;
    auto failedSwap = Run(swap, {});
    Check(!failedSwap.completed && !failedSwap.errors.empty() &&
              !failedSwap.bulkRenameRecoveryNeeded && Read(x) == "x" &&
              Read(y) == "y" && failedSwap.outcomes[0].destination == x &&
              failedSwap.outcomes[1].destination == y,
          "failure after staging and moving rolls the entire cycle back");

    renameCalls = 0;
    failRename = 2;
    blockRestore = Leaf(x);
    failedSwap = Run(swap, {});
    blockRestore.clear();
    failRename = 0;
    const auto retained = failedSwap.outcomes[0].destination;
    Check(!failedSwap.completed && failedSwap.bulkRenameRecoveryNeeded &&
              failedSwap.outcomes[0].status == "retained" && retained != x &&
              Read(retained) == "x" && Read(x) == "other" && Read(y) == "y",
          "rollback collision preserves the new entry and reports the temporary location");
    unlink(x.c_str());
    rename(retained.c_str(), x.c_str());

    // Move the original anchor elsewhere after its first step. The failed
    // rollback must not claim it remains at the now empty staging name.
    const auto relocated = Join(folder, "relocated");
    renameCalls = 0;
    Hooks interference;
    interference.proceed = [&] {
      if (renameCalls == 1 && !Exists(relocated)) {
        for (const auto &entry : std::filesystem::directory_iterator(folder)) {
          const auto path = entry.path().string();
          if (Leaf(path).find(".listi-rename-") == 0) {
            if (rename(path.c_str(), relocated.c_str()))
              throw std::runtime_error("Cannot relocate staged fixture");
            break;
          }
        }
      }
      return true;
    };
    failedSwap = Run(swap, interference);
    Check(!failedSwap.completed && failedSwap.bulkRenameRecoveryNeeded &&
              failedSwap.outcomes[0].destination == relocated &&
              Read(relocated) == "x" && Read(y) == "y",
          "recovery discovers the expected identity at its relocated sibling name");
    rename(relocated.c_str(), x.c_str());

    auto swapped = Run(swap, {});
    Check(swapped.completed == 2 && swapped.bulkRenameUndo &&
              Read(x) == "y" && Read(y) == "x",
          "swap uses a temporary name and preserves both payloads");
    undo = Run(*swapped.bulkRenameUndo, {});
    Check(undo.completed == 2 && !undo.bulkRenameUndo && Read(x) == "x" &&
              Read(y) == "y",
          "guarded undo restores a cycle without creating a redo record");

    auto job = DecodeJob(EncodeJob(preview.job, 'brrt'));
    Check(job.action == Action::BulkRename && job.bulkNames == preview.job.bulkNames,
          "accepted message carries the confirmed names");
    const auto occupied = Join(folder, preview.rows[1].after);
    if (symlink("other-missing", occupied.c_str()))
      throw std::runtime_error("Cannot add collision fixture");
    bool prompted = false;
    Hooks hooks;
    hooks.conflict = [&](const auto &) {
      prompted = true;
      return Decision::Replace;
    };
    auto result = Run(job, hooks);
    Check(!prompted && result.completed == 0 && !result.errors.empty() &&
              Exists(a) && Exists(b) && Exists(c) && Exists(occupied),
          "broken symlink added after preview blocks all changes without replacement");
    if (unlink(occupied.c_str()))
      throw std::runtime_error("Cannot remove collision fixture");

    const auto oldA = folder + "/old-a";
    if (rename(a.c_str(), oldA.c_str()))
      throw std::runtime_error("Cannot move original source aside");
    Write(a, "different entry");
    Check(Reject([&] { FreezeBulkRename(folder, batch.sources, batch.expectedSources); }),
          "opening a new preview cannot adopt a replacement for a selected row");
    const auto stale = PreviewBulkRename(batch, {});
    result = Run(job, {});
    Check(stale.blocked == 1 && result.completed == 0 &&
              result.errors.size() == 1 && result.skipped == 2 &&
              result.outcomes[0].status == "unprocessed" &&
              result.outcomes[1].status == "failed" &&
              result.outcomes[2].status == "unprocessed" &&
              Read(a) == "different entry" && Exists(b),
          "one replaced source blocks the batch and is reported on its row");
    if (unlink(a.c_str()) || rename(oldA.c_str(), a.c_str()))
      throw std::runtime_error("Cannot restore original source");

    const auto moved = folder + "-moved";
    if (rename(folder.c_str(), moved.c_str()) || mkdir(folder.c_str(), 0700))
      throw std::runtime_error("Cannot replace original folder");
    result = Run(job, {});
    Check(result.completed == 0 && !result.errors.empty(),
          "replacement of the selected parent stops the entire job");
    if (rmdir(folder.c_str()) || rename(moved.c_str(), folder.c_str()))
      throw std::runtime_error("Cannot restore fixture folder");

    // Introduce a new target as soon as one rename syscall has returned.
    renameCalls = 0;
    bool injected = false;
    hooks.proceed = [&] {
      if (renameCalls == 1 && !injected) {
        Write(occupied, "other");
        injected = true;
      }
      return true;
    };
    result = Run(job, hooks);
    const auto renamedFolder = Join(folder, preview.rows[0].after);
    Check(!result.completed && result.skipped == 3 && !result.errors.empty() &&
              !result.bulkRenameRecoveryNeeded && !prompted && Exists(a) &&
              Exists(b) && Exists(c) && !Exists(renamedFolder) &&
              Read(occupied) == "other",
          "late collision rolls back earlier changes without replacing the new entry");
    unlink(occupied.c_str());

    renameCalls = 0;
    hooks.proceed = [&] { return renameCalls == 0; };
    result = Run(job, hooks);
    Check(result.cancelled && !result.completed && result.skipped == 3 &&
              result.errors.empty() && !Exists(renamedFolder) && Exists(b) &&
              Exists(a) && Exists(c),
          "cancellation between rename steps restores committed names");

    result = Run(job, {});
    struct stat st;
    const auto renamedLink = Join(folder, preview.rows[2].after);
    Check(result.completed == 3 && result.errors.empty() && !result.cancelled &&
              Read(occupied) == "asset" && Exists(renamedFolder) &&
              !lstat(renamedLink.c_str(), &st) && S_ISLNK(st.st_mode) &&
              !Exists(a) && !Exists(b) && !Exists(c),
          "ready batch moves entries in place, preserving payload and link type");
    Check(result.bulkRenameUndo.has_value(), "full rename records a guarded inverse");
    const auto child = Join(renamedFolder, "new child");
    Write(child, "new content");
    undo = Run(*result.bulkRenameUndo, {});
    Check(!undo.completed && !undo.errors.empty() && Read(child) == "new content" &&
              !Exists(b) && Exists(renamedLink),
          "undo refuses changes inside a renamed directory");
    const auto attrJob = Run(swap, {});
    Check(attrJob.bulkRenameUndo.has_value(), "swap has an inverse before attribute edit");
    BNode attrNode(x.c_str());
    if (attrNode.WriteAttr("listi:test", B_STRING_TYPE, 0, "edited", 6) != 6)
      throw std::runtime_error("Cannot create BFS attribute fixture");
    undo = Run(*attrJob.bulkRenameUndo, {});
    Check(!undo.completed && !undo.errors.empty() && Read(x) == "y" &&
              Read(y) == "x",
          "undo refuses a BFS attribute change on a renamed root");

    const auto oversized = Join(folder, "oversized");
    Write(oversized, "bounded fixture");
    const auto movedLarge = Join(folder, "new-large");
    auto largeJob = FreezeBulkRename(folder, {oversized, x});
    largeJob.bulkNames = {{oversized, Leaf(movedLarge)}, {x, Leaf(x)}};
    simulateLarge = true;
    const auto bounded = Run(largeJob, {});
    simulateLarge = false;
    Check(bounded.completed == 1 && bounded.errors.empty() &&
              !bounded.bulkRenameUndo && Exists(movedLarge) && !Exists(oversized),
          "unverifiable oversized batches can rename without promising undo");
    unlink(movedLarge.c_str());
    unlink(child.c_str());
    unlink(x.c_str());
    unlink(y.c_str());
    unlink(occupied.c_str());
    unlink(renamedLink.c_str());
    rmdir(renamedFolder.c_str());
    rmdir(folder.c_str());
    rmdir(root.c_str());
  } catch (const std::exception &error) {
    fprintf(stderr, "%s\nFixtures kept at %s\n", error.what(), temp);
    return 1;
  }
  return 0;
}
