// SPDX-License-Identifier: MIT
// Opt-in native audit. The destination is an explicitly selected test volume.
#include "ArchiveJobs.h"
#include "FileClipboard.h"
#include "TransferCommand.h"
#include "Trash.h"
#include "VirtualFile.h"
#include <Application.h>
#include <Clipboard.h>
#include <Node.h>
#include <String.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace Listi;

namespace {
struct Fixture {
  fs::path path;
  explicit Fixture(const fs::path &parent) {
    std::string pattern = (parent / "Listi-move-XXXXXX").string();
    if (!mkdtemp(pattern.data()))
      throw std::runtime_error("Cannot create test folder in " +
                               parent.string());
    path = fs::canonical(pattern);
  }
  ~Fixture() {
    std::error_code error;
    fs::remove_all(path, error);
    if (error)
      std::cerr << "Could not clean up " << path << ": " << error.message()
                << '\n';
  }
};

std::string Read(const fs::path &file) {
  std::ifstream stream(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}

bool Has(const Result &result, const fs::path &source, const fs::path &target,
         const char *status) {
  return std::any_of(result.outcomes.begin(), result.outcomes.end(),
                     [&](const EntryOutcome &row) {
    return row.source == source.string() && row.destination == target.string() &&
           row.status == status;
  });
}

void Write(const fs::path &file, const std::string &contents) {
  std::ofstream stream;
  stream.exceptions(std::ios::failbit | std::ios::badbit);
  stream.open(file, std::ios::binary);
  stream << contents;
  stream.close();
}
}

int main() try {
  BApplication app("application/x-vnd.Listi-cross-volume-test");
  const char *volume = std::getenv("LISTI_TEST_VOLUME");
  struct stat from, to;
  if (!volume || stat("/tmp", &from) || stat(volume, &to) ||
      !S_ISDIR(to.st_mode) || from.st_dev == to.st_dev)
    throw std::runtime_error(
        "LISTI_TEST_VOLUME must be a directory on a different volume from /tmp");
  Fixture source("/tmp"), dest(volume);
  int failures = 0;
  auto check = [&](bool ok, const char *label) {
    std::cout << (ok ? "PASS " : "FAIL ") << label << '\n';
    failures += !ok;
  };

  const auto item = source.path / "item";
  const auto destItem = dest.path / "item";
  Write(item, "original");
  auto refuses = [](const Job &job) {
    try {
      PrepareTransfer(job);
    } catch (const std::exception &error) {
      return std::string(error.what()).find("Move between volumes") != std::string::npos;
    }
    return false;
  };
  auto recover = [&](const Result &result, const fs::path &original,
                     const std::string &contents) {
    for (const auto &row : result.outcomes) {
      const std::string prefix = "Original kept in Trash: ";
      if (row.source != original.string() || row.detail.rfind(prefix, 0) != 0)
        continue;
      const auto trash = row.detail.substr(prefix.size());
      struct stat trashed, parent;
      const bool sameVolume = !stat(trash.c_str(), &trashed) &&
                              !stat(original.parent_path().c_str(), &parent) &&
                              trashed.st_dev == parent.st_dev;
      return sameVolume && !fs::exists(original) &&
             RestoreFromTrash(trash) == original.string() &&
             Read(fs::is_directory(original) ? original / "child" : original) ==
                 contents;
    }
    for (const auto &error : result.errors)
      std::cerr << error << '\n';
    return false;
  };
  Job move{Action::Move, {item.string()}, dest.path.string()};
  auto result = Run(PrepareTransfer(move), {});
  check(result.errors.empty() && result.completed == 1 &&
            result.recoverableMoves == 1 && Read(destItem) == "original" &&
            recover(result, item, "original"),
        "panel transfer writes the destination and restores original from source Trash");

  Fixture localTarget("/tmp");
  const auto local = localTarget.path / "first";
  const auto remote = dest.path / "second";
  Write(local, "local");
  Write(remote, "other volume");
  Job mixed{Action::Move, {local.string(), remote.string()}, source.path.string()};
  result = Run(PrepareTransfer(mixed), {});
  check(result.completed == 2 && result.errors.empty() &&
            result.recoverableMoves == 1 &&
            Read(source.path / "first") == "local" && !fs::exists(local) &&
            Read(source.path / "second") == "other volume" &&
            recover(result, remote, "other volume"),
        "mixed move handles a native rename and USB source retirement independently");

  auto tree = source.path / "tree";
  auto targetTree = dest.path / "tree";
  fs::create_directory(tree);
  Write(tree / "child", "child");
  BString value("folder metadata"), copy;
  const bool tagged = BNode(tree.c_str()).WriteAttrString("Listi:test", &value) == B_OK;
  result = Run({Action::Move, {tree.string()}, dest.path.string()}, {});
  check(tagged && result.completed == 1 && result.errors.empty() &&
            Read(targetTree / "child") == "child" &&
            BNode(targetTree.c_str()).ReadAttrString("Listi:test", &copy) == B_OK &&
            copy == value && recover(result, tree, "child"),
        "folder move preserves nested bytes and folder metadata and can be restored");

  Hooks crossHooks;
  crossHooks.conflict = [](const std::string &) { return Decision::Skip; };
  Write(destItem, "previous target");
  result = Run(move, crossHooks);
  check(result.skipped == 1 && !result.completed && result.errors.empty() &&
            Read(item) == "original" && Read(destItem) == "previous target",
        "skipped cross-volume conflict retains both entries");
  crossHooks.conflict = [](const std::string &) { return Decision::Replace; };
  crossHooks.proceed = [&] { return Read(destItem) != "original"; };
  result = Run(move, crossHooks);
  check(result.cancelled && !result.completed && !result.recoverableMoves &&
            Read(item) == "original" && Read(destItem) == "original",
        "cancellation after publish preserves original at source");
  fs::remove(destItem);

  bool edited = false;
  crossHooks.proceed = [&] {
    if (!edited && fs::exists(destItem)) {
      Write(item, "edited while completing the move");
      edited = true;
    }
    return true;
  };
  result = Run(move, crossHooks);
  check(edited && !result.completed && !result.recoverableMoves &&
            !result.errors.empty() && Read(destItem) == "original" &&
            Read(item) == "edited while completing the move",
        "source modification after publication prevents source retirement");
  Write(item, "original");
  fs::remove(destItem);

  // Verify a skipped child does not put a partially copied folder in Trash.
  Write(tree / "new", "new");
  crossHooks.proceed = {};
  crossHooks.conflict = [](const std::string &) { return Decision::Skip; };
  result = Run({Action::Move, {tree.string()}, dest.path.string()}, crossHooks);
  check(!result.completed && !result.recoverableMoves && result.skipped == 1 &&
            Read(tree / "child") == "child" && Read(tree / "new") == "new" &&
            Read(targetTree / "new") == "new",
        "a partial folder merge keeps the complete original in place");
  fs::remove(tree / "new");

  auto archive = [&](const fs::path &directory) {
    auto j = PrepareArchiveJob(false, {item.string()});
    j.archiveFormat = "zip";
    SetArchiveDestination(j, directory.string(), "test.zip");
    auto create = Run(j, {});
    if (create.completed != 1 || !create.errors.empty())
      throw std::runtime_error("Cannot create archive fixture");
    return OpenArchive(j.destination);
  };
  const auto srcArchive = archive(source.path), dstArchive = archive(dest.path);
  const auto srcMember = ChildLocation(srcArchive, "item");
  const auto otherArchiveBefore = Read(dest.path / "test.zip");
  const auto srcArchiveBefore = Read(source.path / "test.zip");
  for (Job attempted : {Job{Action::Move, {item.string()}, dstArchive},
                        Job{Action::Move, {srcMember}, dest.path.string()},
                        Job{Action::Move, {srcMember}, dstArchive}}) {
    bool refused = refuses(attempted);
    attempted.action = Action::MoveArchive;
    auto worker = Run(attempted, {});
    check(refused && worker.completed == 0 && !worker.errors.empty() &&
              !fs::exists(destItem) && Read(item) == "original" &&
              Read(source.path / "test.zip") == srcArchiveBefore &&
              Read(dest.path / "test.zip") == otherArchiveBefore,
          "cross-volume archive move is refused by preparation and worker");
  }

  BClipboard clipboard("listi-volume-move-test", true);
  PutFileClipboard({item.string()}, true, &clipboard);
  auto cut = PrepareFilePaste(dest.path.string(), &clipboard);
  check(!cut.lease.empty() && cut.jobs.size() == 1 &&
            cut.jobs.front().action == Action::Move,
        "cross-volume Cut/Paste queues the same guarded Move");
  if (cut.jobs.size() == 1) {
    result = Run(cut.jobs.front(), {});
    FinishFilePaste(cut, {item.string()}, &clipboard);
    check(result.completed == 1 && result.errors.empty() &&
              Read(destItem) == "original" && recover(result, item, "original"),
          "cross-volume pasted cut is recoverable from source Trash");
    fs::remove(destItem);
  }

  result = Run(PrepareTransfer({Action::Copy, {item.string()}, dest.path.string()}), {});
  check(result.completed == 1 && result.errors.empty() && Read(item) == Read(destItem),
        "explicit cross-volume Copy still writes the selected bytes and keeps the source");
  result = Run({Action::Move, {item.string()}, localTarget.path.string()}, {});
  check(result.completed == 1 && result.errors.empty() && !fs::exists(item) &&
            Read(localTarget.path / "item") == "original",
        "same-volume Move still transfers the original");

  BNode(tree.c_str()).RemoveAttr("Listi:test");
  // Same-volume folder merges still copy and clean up. Cancellation must
  // distinguish two intact trees from a partially removed source.
  auto merged = localTarget.path / "tree";
  fs::create_directory(merged);
  Write(tree / "z", "last");
  Hooks hooks;
  hooks.proceed = [&] { return !fs::exists(merged / "z") || !fs::exists(merged / "child"); };
  result = Run({Action::Move, {tree.string()}, localTarget.path.string()}, hooks);
  check(result.cancelled && !result.completed && Read(tree / "child") == "child" &&
            Read(tree / "z") == "last", "cancelled merge retains source files");
  hooks.conflict = [](const std::string &) { return Decision::Replace; };
  hooks.proceed = [&] { return fs::exists(tree / "z"); };
  result = Run({Action::Move, {tree.string()}, localTarget.path.string()}, hooks);
  check(result.cancelled && !result.completed && !fs::exists(tree / "z") &&
            Has(result, tree, merged, "partial") &&
            Read(tree / "child") == "child" && Read(merged / "child") == "child" &&
            Read(merged / "z") == "last", "late cancellation reports partial same-volume merge");

  // Never fill a general-purpose volume. This case is explicitly enabled
  // for a small disposable filesystem, and writes only in the unique fixture.
  const char *diskFull = std::getenv("LISTI_TEST_DISK_FULL");
  if (diskFull && std::string(diskFull) == "1") {
    const auto space = fs::space(dest.path);
    if (space.capacity > 64 * 1024 * 1024)
      throw std::runtime_error(
          "Disk-full test requires a disposable volume of at most 64 MiB");
    Fixture full(volume);
    const auto oversized = source.path / "oversized";
    const auto original = full.path / "oversized";
    Write(original, "existing destination");
    const std::string contents(space.capacity + 1, 'z');
    Write(oversized, contents);
    hooks = {};
    hooks.conflict = [](const std::string &) { return Decision::Replace; };
    result =
        Run({Action::Copy, {oversized.string()}, full.path.string()}, hooks);
    check(
        result.errors.size() == 1 &&
            result.errors.front().find(std::strerror(ENOSPC)) !=
                std::string::npos &&
            !result.cancelled && !result.completed &&
            result.completedRoots.empty() && !result.retainedMoveCopy &&
            Has(result, oversized, original, "failed") &&
            Read(oversized) == contents &&
            Read(original) == "existing destination" &&
            std::distance(fs::directory_iterator(full.path),
                          fs::directory_iterator()) == 1,
        "full disk retains the source and existing destination without a staging file");
  }
  return failures ? 1 : 0;
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 2;
}
