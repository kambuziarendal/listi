// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "ArchiveWindow.h"
#include "JobSnapshot.h"
#include "Shelf.h"
#include "ShelfWindow.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
namespace {
struct Fixture {
  fs::path root;
  Fixture() {
    char path[] = "/tmp/Listi-shelf-XXXXXX";
    const char *created = mkdtemp(path);
    if (!created)
      throw std::runtime_error("Could not create test fixture");
    root = fs::canonical(created);
  }
  ~Fixture() {
    std::error_code error;
    fs::remove_all(root, error);
  }
};

void Check(bool success, const char *name) {
  if (!success)
    throw std::runtime_error(name);
  std::cout << "PASS " << name << '\n';
}

std::string Read(const fs::path &path) {
  std::ifstream stream(path);
  return std::string(std::istreambuf_iterator<char>(stream), {});
}

template <typename Action>
void Refused(Action action, const char *reason, const char *name) {
  try {
    action();
  } catch (const std::runtime_error &e) {
    Check(std::string(e.what()).find(reason) != std::string::npos, name);
    return;
  }
  Check(false, name);
}

void CheckProtectedSources(const fs::path &root) {
  for (auto action : {Listi::Action::Copy, Listi::Action::Move}) {
    const auto base = root / (action == Listi::Action::Move ? "move" : "copy");
    const auto incoming = base / "in/set", out = base / "out";
    const auto selected = out / "set/doc", valid = base / "valid";
    fs::create_directories(incoming / "doc");
    fs::create_directories(selected);
    fs::create_directory(valid);
    const auto first = base / "in/first";
    std::ofstream(first) << "first source";
    std::ofstream(out / "first") << "first target";
    std::ofstream(incoming / "doc/file") << "incoming";
    std::ofstream(selected / "file") << "selected";
    const auto alias = base / "alias";
    fs::create_directory_symlink(out / "set", alias);
    Listi::Shelf shelf;
    shelf.Add({first.string(), incoming.string(), (alias / "doc").string()});
    const auto prepared = Listi::PrepareShelfTransfer(shelf.Snapshot(), action, out.string());
    const auto guarded = Listi::Run(prepared, {});
    Check(!guarded.completed && !guarded.errors.empty() &&
              guarded.errors.front().find("contains a selected source") != std::string::npos,
          "Shelf worker rejects nested source/output overlap before first root");
    Check(Read(first) == "first source" && Read(out / "first") == "first target" &&
              Read(incoming / "doc/file") == "incoming" && Read(selected / "file") == "selected" &&
              !fs::exists(out / "doc"),
          "rejected Shelf transfer leaves both versions and the preceding conflict intact");
    Listi::Hooks overwrite;
    overwrite.conflict = [](const auto &) { return Listi::Decision::ReplaceAll; };
    const Listi::Job raw{action, {first.string(), incoming.string(), selected.string()}, out.string()};
    const auto refused = Listi::Run(raw, overwrite);
    Check(!refused.completed && !refused.errors.empty() && Read(out / "first") == "first target" &&
              Read(selected / "file") == "selected" && Read(incoming / "doc/file") == "incoming",
          "unprepared worker job detects the overlap before writing any root");
    const auto result = Listi::Run(Listi::PrepareShelfTransfer(shelf.Snapshot(), action, valid.string()), {});
    Check(result.completed == 3 && result.completedRoots.size() == 3 && result.errors.empty() &&
              Read(valid / "first") == "first source" && Read(valid / "set/doc/file") == "incoming" &&
              Read(valid / "doc/file") == "selected" && Read(out / "first") == "first target" &&
              fs::exists(first) == (action == Listi::Action::Copy) &&
              fs::exists(selected) == (action == Listi::Action::Copy) &&
              fs::exists(incoming) == (action == Listi::Action::Copy),
          "Shelf retry to disjoint output delivers every selected version");
  }
}

void CheckManyFolders(const fs::path &root) {
  const auto sources = root / "sources", copyTarget = root / "many-copies",
             archives = root / "archives";
  for (const auto &folder : {sources, copyTarget, archives})
    fs::create_directory(folder);
  std::vector<std::string> paths;
  constexpr int folders = 64;
  for (int i = 0; i < folders; ++i) {
    const auto name = "folder-" + std::to_string(i);
    const auto folder = sources / std::to_string(i) / name;
    fs::create_directories(folder / "nested" / "empty-dir");
    std::ofstream(folder / "nested" / "payload.txt") << name;
    std::ofstream(folder / "empty.txt");
    paths.push_back(folder.string());
  }
  Listi::Shelf shelf;
  Check(shelf.Add(paths) == folders, "collect 64 folders from separate parents");
  const auto snapshot = shelf.Snapshot();
  const auto first = fs::path(paths.front());
  const auto firstTarget = copyTarget / first.filename();
  fs::create_directories(firstTarget / "nested");
  const auto existing = firstTarget / "nested" / "payload.txt";
  std::ofstream(existing) << "keep target";
  std::ofstream(firstTarget / "only-at-destination") << "keep sibling";
  auto transfer = [&](const fs::path &target) {
    const auto job = Listi::PrepareShelfTransfer(
        shelf.Snapshot(), Listi::Action::Copy, target.string());
    const auto submitted = Listi::DecodeSubmittedJob(
        Listi::EncodeJob(job, Listi::kSubmitShelfTransfer),
        {Listi::Action::Copy});
    Check(submitted && submitted->sources == paths &&
              submitted->expectedSources.size() == paths.size(),
          "Shelf transfer payload keeps all selected source identities");
    return *submitted;
  };
  size_t conflicts = 0;
  const auto renamed = firstTarget / "nested" / "payload (2).txt";
  Listi::Hooks keepBoth;
  keepBoth.copyConflict = [&](const Listi::CopyConflict &request) {
    if (request.source == (first / "nested" / "payload.txt").string() &&
        request.destination == existing.string() &&
        request.proposed == renamed.string())
      ++conflicts;
    return Listi::Decision::KeepBoth;
  };
  const auto copied = Listi::Run(transfer(copyTarget), keepBoth);
  Check(conflicts == 1 && copied.completed == folders && copied.skipped == 0 &&
            copied.errors.empty() && copied.completedRoots.size() == folders &&
            Read(existing) == "keep target" &&
            Read(firstTarget / "only-at-destination") == "keep sibling" &&
            Read(renamed) == first.filename().string(),
        "merged folder keeps both conflicting files and reports all copied roots");
  auto verifyTree = [&](const fs::path &folder, const fs::path &payload) {
    return fs::is_directory(folder / "nested" / "empty-dir") &&
           fs::is_empty(folder / "nested" / "empty-dir") &&
           fs::is_regular_file(folder / "empty.txt") &&
           fs::file_size(folder / "empty.txt") == 0 &&
           Read(payload) == folder.filename().string();
  };
  bool exact = true;
  for (const auto &path : paths) {
    const fs::path original(path), target = copyTarget / original.filename();
    const auto payload = target == firstTarget ? renamed
                                               : target / "nested" / "payload.txt";
    exact = exact && verifyTree(original, original / "nested" / "payload.txt") &&
            verifyTree(target, payload) &&
            std::find(copied.completedRoots.begin(), copied.completedRoots.end(),
                      std::make_pair(path, target.string())) !=
                copied.completedRoots.end();
  }
  Check(exact, "all copied roots and originals contain the expected nested files and empty entries");

  auto archiveJob = Listi::PrepareShelfArchive(snapshot);
  std::ofstream(archives / "occupied.zip") << "existing archive";
  Refused([&] {
    Listi::SetArchiveDestination(archiveJob, archives.string(), "occupied.zip");
  },
          "already exists", "archive dialog refuses an existing output filename");
  Check(Read(archives / "occupied.zip") == "existing archive",
        "archive collision leaves existing bytes intact");
  // Selecting an alias of a child of the final root must also be refused.
  const auto insideSource = root / "inside-source";
  fs::create_directory_symlink(fs::path(paths.back()) / "nested", insideSource);
  Listi::SetArchiveDestination(archiveJob, insideSource.string(), "inside.zip");
  const auto inside = Listi::Run(archiveJob, {});
  Check(inside.completed == 0 && inside.errors.size() == 1 &&
            inside.errors.front().find("outside the selected folder") != std::string::npos &&
            !fs::exists(insideSource / "inside.zip"),
        "archive cannot publish under any selected source, even through an alias");
  auto rootArchive = Listi::PrepareArchiveJob(false, {"/"});
  Listi::SetArchiveDestination(rootArchive, archives.string(), "root.zip");
  const auto rootRefused = Listi::Run(rootArchive, {});
  Check(rootRefused.completed == 0 && rootRefused.errors.size() == 1 &&
            rootRefused.errors.front().find("outside the selected folder") != std::string::npos &&
            !fs::exists(rootArchive.destination),
        "every destination is inside the filesystem root");
  Listi::SetArchiveDestination(archiveJob, archives.string(), "full-shelf.zip");
  const auto submitted = Listi::DecodeSubmittedJob(
      Listi::EncodeJob(archiveJob, kArchiveAccepted), {Listi::Action::CreateArchive});
  Check(submitted && submitted->sources == paths &&
            submitted->expectedSources.size() == paths.size(),
        "archive accepts the entire Shelf, with each guarded source");
  std::set<std::string> done;
  Listi::Hooks packing;
  packing.fileDone = [&](const std::string &path, const std::string &destination) {
    if (destination == archiveJob.destination)
      done.insert(path);
  };
  const auto packed = Listi::Run(*submitted, packing);
  Check(packed.completed == 1 && packed.skipped == 0 && packed.errors.empty() &&
            std::all_of(paths.begin(), paths.end(), [&](const auto &path) { return done.count(path); }),
        "one published ZIP reports completion for every Shelf root");
  auto unpack = Listi::PrepareArchiveJob(true, {archiveJob.destination});
  Listi::SetArchiveDestination(unpack, archives.string(), "restored");
  const auto extracted = Listi::Run(unpack, {});
  const auto restored = fs::path(unpack.destination);
  Check(extracted.completed == 1 && extracted.skipped == 0 && extracted.errors.empty() &&
            std::distance(fs::directory_iterator(restored), fs::directory_iterator()) == folders &&
            std::all_of(paths.begin(), paths.end(), [&](const auto &path) {
              const auto folder = restored / fs::path(path).filename();
              return verifyTree(folder, folder / "nested" / "payload.txt");
            }) && shelf.Snapshot().revision == snapshot.revision,
        "ZIP round-trip preserves every folder and payload without changing the Shelf");

  const auto blocked = root / "blocked-copies";
  fs::create_directory(blocked);
  const auto blockedTarget = blocked / first.filename();
  std::ofstream(blockedTarget) << "not a folder";
  const auto partial = Listi::Run(transfer(blocked), {});
  Check(partial.completed == folders - 1 && partial.skipped == 0 &&
            partial.errors.size() == 1 && partial.completedRoots.size() == folders - 1 &&
            Read(blockedTarget) == "not a folder" &&
            Read(blocked / fs::path(paths.back()).filename() / "nested" / "payload.txt") ==
                fs::path(paths.back()).filename().string(),
        "a file blocking one folder is left untouched and other roots still copy");
}
} // namespace

int main() {
  try {
    Fixture fixture;
    CheckProtectedSources(fixture.root);
    const auto a = fixture.root / "a", b = fixture.root / "b",
               target = fixture.root / "target", alias = fixture.root / "alias";
    fs::create_directory(a);
    fs::create_directory(b);
    fs::create_directory(target);
    fs::create_directory_symlink(a, alias);
    auto first = (a / "first").string(), second = (b / "second").string(),
         collision = (b / "first").string(), link = (a / "link").string();
    std::ofstream(first) << "original";
    std::ofstream(second) << "second";
    std::ofstream(collision) << "collision";
    fs::create_symlink(first, link);

    Listi::Shelf shelf;
    Check(shelf.Add({first, second}) == 2, "collect from two source folders");
    auto ready = shelf.Snapshot();
    Check(shelf.Add({(alias / "first").string(), first}) == 0 &&
              shelf.Snapshot().revision == ready.revision,
          "duplicate refs and parent aliases do not refresh the snapshot");
    Check(shelf.Add({link}) == 1 &&
              shelf.Snapshot().entries.back().path == link,
          "collect a symlink without following the selected entry");
    shelf.Add({collision});
    const auto withLink = shelf.Snapshot();
    shelf.Remove({link, collision});
    const auto withoutLink = shelf.Snapshot();
    Check(fs::is_symlink(link) && withoutLink.entries.size() == 2 &&
              withoutLink.entries.front().path == first &&
              withoutLink.entries.back().path == second &&
              withoutLink.revision == withLink.revision + 1 &&
              Read(collision) == "collision",
          "selected removal keeps unselected references and original entries");

    shelf.Add({collision});
    Refused(
        [&] {
          Listi::PrepareShelfTransfer(shelf.Snapshot(), Listi::Action::Move,
                                      target.string());
        },
        "same name", "same-name entries cannot overwrite each other");
    Refused([&] { Listi::PrepareShelfArchive(shelf.Snapshot()); }, "same name",
            "ambiguous ZIP root names are refused");
    Refused([&] { Listi::PrepareShelfTransfer(shelf.Snapshot(), Listi::Action::Copy,
                                               target.string()); },
            "Remove one reference", "duplicate copy roots require removing one reference before submission");
    Check(fs::is_empty(target), "refused duplicate roots never write a destination");
    shelf.Remove({collision});
    Check(fs::exists(collision),
          "removing a shelf row leaves its original file");

    shelf.Add({a.string()});
    Refused(
        [&] {
          Listi::PrepareShelfTransfer(shelf.Snapshot(), Listi::Action::Move,
                                      target.string());
        },
        "its contents", "refuse a folder and one of its descendants");
    shelf.Remove({a.string()});
    Refused(
        [&] {
          Listi::PrepareShelfTransfer(shelf.Snapshot(), Listi::Action::Move,
                                      a.string());
        },
        "already in", "refuse a move back into an original folder");

    const auto copyFolder = fixture.root / "copies";
    fs::create_directory(copyFolder);
    const auto copySnapshot = shelf.Snapshot();
    auto copy = Listi::PrepareShelfTransfer(copySnapshot, Listi::Action::Copy,
                                            copyFolder.string());
    const auto submitted =
        Listi::DecodeSubmittedJob(Listi::EncodeJob(copy, 'test'),
                                  {Listi::Action::Copy, Listi::Action::Move});
    Check(submitted.has_value(),
          "Shelf copy uses the guarded transfer message");
    const auto copied = Listi::Run(*submitted, {});
    Check(
        copied.completed == 2 && copied.errors.empty() &&
            Read(copyFolder / "first") == "original" &&
            Read(copyFolder / "second") == "second" &&
            Read(first) == "original" && Read(second) == "second" &&
            shelf.Snapshot().revision == copySnapshot.revision,
        "copy entries from different folders without changing originals or Shelf");

    const auto zipSnapshot = shelf.Snapshot();
    auto zip = Listi::PrepareShelfArchive(zipSnapshot);
    Listi::SetArchiveDestination(zip, target.string(), "shelf.zip");
    zip = Listi::DecodeJob(Listi::EncodeJob(zip, 'test'));
    auto packed = Listi::Run(zip, {});
    Check(packed.completed == 1 && packed.errors.empty() &&
              zip.archiveFormat == "zip" && Read(first) == "original" &&
              Read(second) == "second" &&
              shelf.Snapshot().revision == zipSnapshot.revision,
          "create a ZIP without modifying originals or Shelf");
    auto unpack = Listi::PrepareArchiveJob(true, {zip.destination});
    Listi::SetArchiveDestination(unpack, target.string(), "unpacked");
    auto extracted = Listi::Run(unpack, {});
    Check(extracted.completed == 1 && extracted.errors.empty() &&
              Read(target / "unpacked" / "first") == "original" &&
              Read(target / "unpacked" / "second") == "second",
          "round trip ZIP members collected from different folders");
    auto tar = Listi::PrepareShelfArchive(shelf.Snapshot());
    tar.archiveFormat = "tar";
    Listi::SetArchiveDestination(tar, target.string(), "shelf.tar");
    tar = Listi::DecodeJob(Listi::EncodeJob(tar, 'test'));
    const auto packedTar = Listi::Run(tar, {});
    auto unpackTar = Listi::PrepareArchiveJob(true, {tar.destination});
    Listi::SetArchiveDestination(unpackTar, target.string(), "unpacked-tar");
    const auto extractedTar = Listi::Run(unpackTar, {});
    Check(
        packedTar.completed == 1 && packedTar.errors.empty() &&
            extractedTar.completed == 1 && extractedTar.errors.empty() &&
            Read(target / "unpacked-tar" / "first") == "original" &&
            Read(target / "unpacked-tar" / "second") == "second" &&
            shelf.Snapshot().revision == zipSnapshot.revision,
        "selected non-ZIP format survives submission and round-trips Shelf entries");

    auto occupied = Listi::PrepareShelfArchive(shelf.Snapshot());
    Listi::SetArchiveDestination(occupied, target.string(), "occupied.zip");
    Listi::Hooks occupyTarget;
    occupyTarget.progress = [&](const std::string &, uint64_t) {
      std::ofstream(occupied.destination) << "do not replace";
    };
    auto refused = Listi::Run(occupied, occupyTarget);
    Check(refused.completed == 0 && !refused.errors.empty() &&
              Read(occupied.destination) == "do not replace",
          "ZIP job cannot replace a target created while packing");

    auto lateCopyFolder = fixture.root / "late-copy";
    fs::create_directory(lateCopyFolder);
    const auto guardedCopy = Listi::PrepareShelfTransfer(
        shelf.Snapshot(), Listi::Action::Copy, lateCopyFolder.string());
    auto guardedZip = Listi::PrepareShelfArchive(shelf.Snapshot());
    Listi::SetArchiveDestination(guardedZip, target.string(), "stale.zip");
    auto job = Listi::PrepareShelfTransfer(
        shelf.Snapshot(), Listi::Action::Move, target.string());
    job = Listi::DecodeJob(Listi::EncodeJob(job, 'test'));
    const auto original = first + ".original";
    fs::rename(first, original);
    std::ofstream(first) << "replacement";
    Check(shelf.Snapshot().entries.front().State() ==
              Listi::ShelfState::Replaced,
          "same-path replacement is visibly stale");
    Check(shelf.Add({first}) == 0 && shelf.Snapshot().entries.front().State() ==
                                         Listi::ShelfState::Replaced,
          "adding the same path does not silently accept a replacement");
    Refused([&] { shelf.Add({first}, job.expectedSources); }, "changed",
            "a drag with the old inode cannot collect its replacement");
    Refused(
        [&] {
          Listi::PrepareShelfTransfer(shelf.Snapshot(), Listi::Action::Move,
                                      target.string());
        },
        "missing or replaced", "reject stale sources before queuing");
    Refused([&] { Listi::PrepareShelfArchive(shelf.Snapshot()); },
            "missing or replaced", "reject a stale Shelf reference for ZIP");
    auto stale = Listi::Run(guardedZip, {});
    Check(stale.completed == 0 && !stale.errors.empty() &&
              !fs::exists(guardedZip.destination),
          "ZIP job refuses a source replaced after picking");

    const auto staleCopy = Listi::Run(guardedCopy, {});
    Check(
        staleCopy.completed == 1 && !staleCopy.errors.empty() &&
            !fs::exists(lateCopyFolder / "first") &&
            Read(lateCopyFolder / "second") == "second" &&
            Read(first) == "replacement" && Read(second) == "second",
        "guarded copy rejects a late replacement and leaves originals untouched");

    const auto outcome = Listi::Run(job, {});
    Check(outcome.completed == 1 && !outcome.errors.empty() &&
              fs::exists(first) && !fs::exists(target / "first") &&
              fs::exists(target / "second"),
          "guarded job rejects a late replacement and moves the other source");
    Check(shelf.Snapshot().entries.size() == 2 &&
              shelf.Snapshot().entries.back().State() ==
                  Listi::ShelfState::Missing,
          "shelf retains references and shows a moved source as unavailable");
    shelf.Remove({first});
    Refused([&] { Listi::PrepareShelfArchive(shelf.Snapshot()); },
            "missing or replaced", "reject a missing Shelf reference for ZIP");
    Refused([&] { Listi::PrepareShelfTransfer(shelf.Snapshot(), Listi::Action::Copy,
                                               target.string()); },
            "Relink or remove", "missing copy root requires relinking or removing before submission");

    shelf.Remove({second});
    shelf.Add({first});
    auto destinationJob = Listi::PrepareShelfTransfer(
        shelf.Snapshot(), Listi::Action::Move, target.string());
    fs::rename(target, fixture.root / "old-target");
    fs::create_directory(target);
    const auto replacedDestination = Listi::Run(destinationJob, {});
    Check(!replacedDestination.errors.empty() && fs::exists(first) &&
              !fs::exists(target / "first"),
          "guard rejects a destination replaced after confirmation");

    shelf.Add({a.string(), collision, link, first});
    fs::rename(collision, collision + ".renamed");
    const auto beforeClear = shelf.Snapshot();
    shelf.Clear();
    const auto cleared = shelf.Snapshot();
    Check(cleared.entries.empty() &&
              cleared.revision == beforeClear.revision + 1 &&
              fs::is_directory(a) && fs::is_symlink(link) &&
              Read(first) == "replacement" &&
              Read(collision + ".renamed") == "collision",
          "clearing ready and missing references leaves originals unchanged");
    shelf.Clear();
    Check(shelf.Snapshot().revision == cleared.revision,
          "clearing an empty Shelf does not change its revision");
    Check(shelf.Add({first, link}) == 2,
          "cleared references can be collected again");
    CheckManyFolders(fixture.root);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
