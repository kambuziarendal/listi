// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "DownloadStageProbe.h"
#include "ArchiveVolumes.h"
#include "JobSnapshot.h"
#include "TransferCommand.h"
#include "VirtualFile.h"
#include <Application.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <sys/stat.h>

using namespace Listi;
namespace fs = std::filesystem;
namespace {
void Check(bool ok, const std::string &label) {
  if (!ok)
    throw std::runtime_error("FAIL " + label);
  std::cout << "PASS " << label << std::endl;
}
std::string Read(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot open " + path.string());
  return {std::istreambuf_iterator<char>(input), {}};
}
void Write(const fs::path &path, const std::string &bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), bytes.size());
  output.close();
  if (!output) throw std::runtime_error("Cannot write " + path.string());
}
void ListMembers(const std::string &folder, const std::string &prefix, std::set<std::string> &names) {
  for (const auto &entry : ListFiles(folder)) {
    names.insert(prefix + entry.name);
    if (entry.directory)
      ListMembers(entry.path, prefix + entry.name + "/", names);
  }
}
std::set<std::string> Members(const fs::path &file) {
  std::set<std::string> names;
  const auto url = OpenArchive(file.string());
  if (url.empty()) throw std::runtime_error("Archive unreadable: " + file.string());
  ListMembers(url, "", names);
  return names;
}
Job Copy(const std::vector<std::string> &sources, const fs::path &folder) {
  auto job = PrepareTransfer({Action::Copy, sources, folder.string()});
  return DecodeJob(EncodeJob(job, 0));
}
Hooks Replace() {
  Hooks hooks;
  hooks.conflict = [](const std::string &) {return Decision::ReplaceAll;};
  return hooks;
}

void ZipCases(const fs::path &root, fs::path &outer, std::string &outerBytes) {
  const auto inputs = root / "inputs", target = root / "target";
  fs::create_directories(inputs / "nested");
  fs::create_directories(target / "nested");
  const auto inner = Read("tests/fixtures/zip/cp437.zip");
  Write(inputs / "nested/backup.zip", inner);
  Write(inputs / "backup.zip", inner);
  Write(inputs / "other.zip", inner);
  Write(inputs / "unique.txt", "only in the outer archive");
  Write(inputs / "first", "incoming");
  std::vector<std::string> files;
  for (const char *name : {"nested", "backup.zip", "other.zip", "unique.txt", "first"})
    files.push_back((inputs / name).string());
  auto create = PrepareArchiveJob(false, files);
  SetArchiveDestination(create, (target / "nested").string(), "backup.zip");
  const auto built = Run(create, {});
  Check(built.errors.empty() && built.completed == 1, "created valid outer ZIP");
  outer = create.destination;
  outerBytes = Read(outer);
  const std::set<std::string> names{"backup.zip", "first", "nested", "nested/backup.zip", "other.zip", "unique.txt"};
  const auto url = OpenArchive(outer.string());
  const auto first = ChildLocation(url, "first");
  for (bool nested : {false, true}) {
    const auto output = nested ? target : outer.parent_path();
    Write(output / "first", "earlier target");
    const auto member = ChildLocation(url, nested ? "nested" : "backup.zip");
    for (int attempt = 0; attempt < 2; ++attempt) {
      const auto result = Run(Copy({first, member}, output), Replace());
      Check(result.completed == 0 && !result.errors.empty() &&
                result.errors.front().find("source archive volume") != std::string::npos &&
                Read(output / "first") == "earlier target" &&
                Read(outer) == outerBytes && Members(outer) == names,
            "known collision refuses whole copy before the first replacement; original ZIP intact");
    }
  }
  // A projected virtual folder must also leave a later *local* source intact.
  const auto mixed = root / "mixed";
  fs::create_directories(mixed / "nested");
  const auto waiting = mixed / "nested/backup.zip";
  Write(waiting, "existing local source");
  Write(mixed / "first", "do not replace me");
  const auto localAndVirtual = Copy({first, ChildLocation(url, "nested"), waiting.string()}, mixed);
  auto blocked = Run(localAndVirtual, Replace());
  Check(blocked.completed == 0 && !blocked.errors.empty() && Read(waiting) == "existing local source" &&
            Read(mixed / "first") == "do not replace me" && !fs::exists(mixed / "backup.zip"),
        "mixed virtual and local selection is checked together before first publication");
  const auto safe = root / "safe";
  fs::create_directory(safe);
  Write(safe / "backup.zip", "replaceable");
  auto good = Run(Copy({ChildLocation(url, "backup.zip")}, safe), Replace());
  Check(good.completed == 1 && good.errors.empty() && Read(safe / "backup.zip") == inner &&
            Read(outer) == outerBytes && Members(outer) == names,
        "ordinary Replace delivers the member and preserves every original member");

  auto disjoint = Run(Copy({first, waiting.string()}, safe), Replace());
  Check(disjoint.completed == 2 && disjoint.errors.empty() &&
            Read(safe / "first") == "incoming" && Read(safe / "backup.zip") == "existing local source",
        "independent archive and local selections still copy together");

  fs::permissions(mixed / "nested", fs::perms::owner_all);
  const auto sibling = mixed / "nested/unrelated.txt";
  Write(sibling, "other selected bytes");
  const auto nestedSafe = Run(Copy({ChildLocation(url, "nested"), sibling.string()}, mixed), Replace());
  Check(nestedSafe.completed == 2 && nestedSafe.errors.empty() &&
            Read(waiting) == inner && Read(sibling) == "other selected bytes" &&
            Read(mixed / "unrelated.txt") == "other selected bytes" && Read(outer) == outerBytes,
        "virtual folder may merge beside a selected local source without overwriting it");

  auto extract = PrepareArchiveJob(true, {outer.string()});
  bool refused = false;
  try { SetArchiveDestination(extract, outer.parent_path().string(), outer.filename().string()); }
  catch (const std::exception &) { refused = true; }
  Check(refused, "Extract separately refuses source archive as its output name");
  SetArchiveDestination(extract, safe.string(), "extracted");
  Write(safe / "extracted", "arrived while queued");
  auto late = Run(extract, Replace());
  Check(!late.errors.empty() && late.completed == 0 && Read(safe / "extracted") == "arrived while queued",
        "queued Extract does not replace an intervening target");
  fs::remove(safe / "extracted");
  good = Run(extract, {});
  Check(good.errors.empty() && good.completed == 1 && Read(safe / "extracted/nested/backup.zip") == inner &&
            Read(safe / "extracted/unique.txt") == "only in the outer archive" &&
            Read(outer) == outerBytes && Members(outer) == names,
        "Extract retry publishes a new folder and keeps the outer archive intact");
}
void SkipCases(const fs::path &root) {
  namespace Probe = DownloadStageProbe;
  const auto src = root / "skip-input", dst = root / "skip-output";
  fs::create_directory(src);
  fs::create_directory(dst);
  Write(src / Probe::smallName, std::string(32768, 'a'));
  Write(src / Probe::largeName, std::string(Probe::largeSize, 'b'));
  auto create = PrepareArchiveJob(false, {(src / Probe::smallName).string(), (src / Probe::largeName).string()});
  SetArchiveDestination(create, root.string(), "skip.zip");
  Check(Run(create, {}).completed == 1, "created Skip All ZIP fixture");
  const auto original = Read(create.destination);
  const auto url = OpenArchive(create.destination);
  // Haiku /tmp is a link. The local publisher presents the canonical target in its conflict question.
  Probe::Exercise(Copy({ChildLocation(url, Probe::smallName), ChildLocation(url, Probe::largeName)}, fs::canonical(dst)),
                  [](const Job &job, const Hooks &hooks) { return Run(job, hooks); }, "ZIP");
  Check(Read(create.destination) == original, "all three Skip All attempts preserve ZIP bytes");
}

void SplitCase(const fs::path &target, bool alias) {
  const fs::path fixture("tests/fixtures/multipart");
  const auto original = FindArchiveVolumes((fixture / "plain.7z.001").string());
  for (const auto &part : original.files)
    Write(target / fs::path(part.path).filename(), Read(part.path));
  const auto firstPart = target / "plain.7z.001";
  const auto names = Members(firstPart);
  const auto url = OpenArchive(firstPart.string());
  // Ordinary local input plus a member: a local output may collide with the
  // archive supplying a *different* selection, even without hard-link support.
  const fs::path local = target / "local-inputs";
  fs::create_directory(local);
  Write(local / "plain.7z.002", "would replace the middle archive part");
  const auto second = alias ? ChildLocation(url, "payload.bin") : (local / "plain.7z.002").string();
  Write(target / "note.txt", "keep");
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto result = Run(Copy({ChildLocation(url, "note.txt"), second}, target), Replace());
    Check(result.completed == 0 && !result.errors.empty() &&
              result.errors.front().find("source archive volume") != std::string::npos &&
              Read(target / "note.txt") == "keep", "all multipart readers protected before the first write");
    for (const auto &part : original.files)
      Check(Read(target / fs::path(part.path).filename()) == Read(part.path), "physical part unchanged");
    Check(names == std::set<std::string>{"note.txt", "payload.bin"} && Members(firstPart) == names,
          "all original split-archive members remain readable");
  }
}
void AliasCases(const fs::path &folder, const fs::path &outer, const std::string &bytes) {
  const auto source = folder / "original.zip", alias = folder / "other.zip";
  const auto hardlinkPair = [](const fs::path &left, const fs::path &right) {
    struct stat a{}, b{};
    return !lstat(left.c_str(), &a) && !lstat(right.c_str(), &b) &&
           S_ISREG(a.st_mode) && S_ISREG(b.st_mode) && a.st_nlink == 2 && b.st_nlink == 2 &&
           a.st_dev == b.st_dev && a.st_ino == b.st_ino;
  };
  Check(hardlinkPair(source, alias) && hardlinkPair(folder / "plain.7z.002", folder / "payload.bin"),
        "mounted fixture contains both real hard-link pairs");
  Write(source, bytes);
  const auto expected = Members(outer);
  const auto url = OpenArchive(source.string());
  const auto member = ChildLocation(url, "other.zip");
  auto rejected = Run(Copy({member}, folder), Replace());
  Check(rejected.completed == 0 && !rejected.errors.empty() &&
            Read(source) == bytes && Read(alias) == bytes && Members(source) == expected,
        "an alternate hard-link name cannot replace the physical archive");
  const auto late = folder / "late";
  fs::create_directory(late);
  Write(late / "other.zip", "ordinary destination");
  // Move this directory entry onto the actual target only after Replace was offered.
  const auto choice = folder / "at-prompt.zip";
  fs::rename(alias, choice);
  const auto beforePrompt = Copy({member}, late);
  bool prompted = false;
  Hooks hooks;
  hooks.conflict = [&](const std::string &) {
    fs::rename(choice, late / "other.zip");
    prompted = true;
    return Decision::ReplaceAll;
  };
  rejected = Run(beforePrompt, hooks);
  Check(prompted && rejected.completed == 0 && !rejected.errors.empty() && Read(source) == bytes &&
            Read(late / "other.zip") == bytes && Members(source) == expected,
        "target replaced by an archive alias during the prompt is left untouched");
  // Queue a nested projection with an ordinary target. Swap its *parent*
  // beneath the captured destination, so the alias inode and outer archive
  // fingerprint do not change between job preparation and worker preflight.
  const auto holding = folder / "holding";
  const auto nested = late / "nested";
  fs::create_directory(holding);
  fs::rename(late / "other.zip", holding / "backup.zip");
  fs::create_directory(nested);
  Write(nested / "backup.zip", "independent");
  Write(late / "first", "unchanged");
  const auto queued = Copy({ChildLocation(url, "first"), ChildLocation(url, "nested")}, late);
  fs::rename(nested, late / "earlier-nested");
  fs::rename(holding, nested);
  Check(queued.expectedArchiveSets.at(source.string()) == FindArchiveVolumes(source.string()).Fingerprint(),
        "queued alias fixture retains the frozen source fingerprint");
  rejected = Run(queued, Replace());
  Check(rejected.completed == 0 && !rejected.errors.empty() &&
            rejected.errors.front().find("source archive volume") != std::string::npos &&
            Read(late / "first") == "unchanged" && Read(nested / "backup.zip") == bytes &&
            Members(source) == expected, "changed nested target is found before queued copy starts");
  fs::rename(nested / "backup.zip", alias);
  fs::remove(nested);
  fs::remove_all(late / "earlier-nested");
  SplitCase(folder, true);
  std::cout << "PASS protected both mounted archive aliases through replacement and retry" << std::endl;
}
}
int main() {
  BApplication app("application/x-vnd.Listi-ArchiveCopyInputsTest");
  try {
    char temp[] = "/tmp/listi-archive-inputs-XXXXXX";
    if (!mkdtemp(temp)) throw std::runtime_error("Cannot create fixture");
    const fs::path root(temp);
    fs::path outer;
    std::string bytes;
    ZipCases(root, outer, bytes);
    SkipCases(root);
    fs::create_directory(root / "split");
    SplitCase(root / "split", false);
    if (const char *alias = getenv("LISTI_ARCHIVE_ALIAS_DIR"))
      AliasCases(alias, outer, bytes);
    else
      std::cout << "SKIP mounted hard-link fixture not supplied\n";
    fs::remove_all(root);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
