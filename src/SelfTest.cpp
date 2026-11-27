// SPDX-License-Identifier: MIT
#include "ArchiveJobChecks.h"
#include "ArchiveJobs.h"
#include "ArchiveVolumes.h"
#include "FileOperation.h"
#include "NameFilter.h"
#include "PaneModel.h"
#include "Trash.h"
#include "VirtualFile.h"
#include <algorithm>
#include <File.h>
#include <Node.h>
#include <String.h>
#include <TypeConstants.h>
#include <cstdlib>
#include <cstring>
#include <fs_attr.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
namespace Listi {
namespace {
// Optional installed-package check against a disposable, loopback FTP server.
// MLSD provides a stable unique fact but omits unix.mode in this fixture.
void TestLoopbackTransfer(const std::string &root) {
  auto require = [](bool ok, const std::string &why) {
    if (!ok)
      throw std::runtime_error(why);
  };
  auto bytes = [](const std::string &path) {
    std::string value;
    ReadFile(path, [&](const char *data, size_t n) { value.append(data, n); });
    return value;
  };
  const std::string endpoint = "ftp://127.0.0.1:2128/";
  Connection auth;
  auth.username = "listitest";
  auth.password = "fixture-only";
  SetConnection(endpoint, auth);
  const auto target = ChildLocation(endpoint, LocationLeaf(root));
  MakeFolder(target); // Exclusive, unique test namespace.
  const auto source = Join(root, "upload");
  MakeFolder(source);
  for (const auto name : {"first", "second"})
    std::ofstream(Join(source, name)) << name;
  const auto selected = ChildLocation(target, "upload");
  const auto moved = ChildLocation(target, "previous");
  std::string delivered;
  Hooks hooks;
  hooks.fileDone = [&](const std::string &, const std::string &path) {
    if (delivered.empty()) {
      delivered = LocationLeaf(path);
      RenameFile(selected, moved);
      MakeFolder(selected);
    }
  };
  const Job job{Action::Copy, {source}, target};
  const auto stopped = Run(job, hooks);
  require(!delivered.empty() && !stopped.completed && stopped.partialCopies == 1 &&
              !stopped.errors.empty() && stopped.errors.front().find(selected) != std::string::npos &&
              ListFiles(moved).size() == 1 && ListFiles(selected).empty() &&
              bytes(ChildLocation(moved, delivered)) == delivered,
          "changed destination did not stop at the first acknowledged child");
  require(std::any_of(stopped.outcomes.begin(), stopped.outcomes.end(), [&](const auto &entry) {
            return entry.destination == ChildLocation(selected, delivered) &&
                   entry.upload && entry.upload->publication == Publication::Published;
          }), "missing first delivery receipt");
  const auto retry = Run(job, {});
  require(retry.completed == 1 && retry.errors.empty(), "fresh upload retry failed");
  for (const auto name : {"first", "second"})
    require(bytes(Join(source, name)) == name && bytes(ChildLocation(selected, name)) == name,
            "fresh upload retry did not preserve both originals and both deliveries");

  FileEntry folder, file;
  require(StatFile(selected, folder) && StatFile(ChildLocation(selected, delivered), file) &&
              !folder.permissionsKnown && !file.permissionsKnown, "fixture unexpectedly supplied modes");
  const auto downloads = Join(root, "downloads");
  MakeFolder(downloads);
  const Job download{Action::Copy, {selected}, downloads};
  const auto local = Join(downloads, "upload");
  const auto partial = Run(download, {});
  struct stat info{};
  require(partial.partialCopies == 1 && !stat(local.c_str(), &info) && (info.st_mode & 0777) == 0700,
          "unknown FTP directory mode did not stay private");
  for (const auto name : {"first", "second"})
    require(!stat(Join(local, name).c_str(), &info) && (info.st_mode & 0777) == 0600 &&
                bytes(Join(local, name)) == name, "unknown FTP file mode did not stay private");
  require(!chmod(local.c_str(), 0755) && !unlink(Join(local, delivered).c_str()), "retry setup failed");
  const auto blocked = Run(download, {});
  require(!blocked.completed && !blocked.errors.empty() && access(Join(local, delivered).c_str(), F_OK),
          "FTP retry populated a more accessible existing folder");
}
bool TestLoopbackSftpMode(const std::string &root, const char *knownHosts) {
  const std::string endpoint = "sftp://127.0.0.1:2227/";
  Connection auth;
  auth.username = "listitest";
  auth.password = "fixture-only";
  auth.knownHosts = knownHosts;
  SetConnection(endpoint, auth);
  const auto path = ChildLocation(endpoint, "tightens-installed");
  const auto destination = Join(root, "tightens-installed");
  std::ofstream(destination) << "original";
  Hooks hooks;
  hooks.conflict = [](const std::string &) { return Decision::Replace; };
  FileEntry before, after;
  if (!StatFile(path, before) || before.permissions != 0644)
    return false;
  const Job copy{Action::Copy, {path}, root};
  const auto failed = Run(copy, hooks);
  std::string contents;
  std::ifstream(destination) >> contents;
  if (failed.completed || failed.errors.empty() || contents != "original" ||
      !StatFile(path, after) || after.permissions != 0600)
    return false;
  const auto retry = Run(copy, hooks);
  std::ifstream(destination) >> contents;
  struct stat st{};
  return retry.completed == 1 && retry.errors.empty() && contents == "private" &&
         !stat(destination.c_str(), &st) && (st.st_mode & 0777) == 0600;
}
} // namespace
int SelfTest() {
  char pattern[] = "/boot/home/Listi-test-XXXXXX";
  char *dir = mkdtemp(pattern);
  if (!dir)
    return 1;
  std::string root = dir;
  int checks = 0, failures = 0;
  auto check = [&](bool ok, const char *name) {
    checks++;
    if (!ok)
      failures++;
    std::cout << (ok ? "PASS " : "FAIL ") << name << std::endl;
  };
  if (const auto *hosts = getenv("LISTI_TEST_SFTP_AUDIT_HOSTS")) {
    try {
      check(TestLoopbackSftpMode(root, hosts), "loopback SFTP chmod during download and private retry");
    } catch (const std::exception &e) {
      std::cerr << e.what() << std::endl;
      check(false, "loopback SFTP chmod during download and private retry");
    }
  }
  if (getenv("LISTI_TEST_FTP_AUDIT")) {
    try {
      TestLoopbackTransfer(root);
      check(true, "loopback FTP upload folder identity, retry receipts and private download modes");
    } catch (const std::exception &e) {
      std::cerr << e.what() << std::endl;
      check(false, "loopback FTP upload folder identity, retry receipts and private download modes");
    }
  }
  check(MatchesNameFilter("notes.txt", ""), "empty filter shows all names");
  check(MatchesNameFilter("Holiday Notes.txt", "NOTES"),
        "name filter matches literal substring without ASCII case sensitivity");
  check(!MatchesNameFilter("notes.txt", "*.txt"),
        "filter does not interpret shell wildcards");
  check(!MatchesNameFilter("notes.txt", "picture"),
        "nonmatching name is hidden");
  check(MatchesNameFilter("..", "picture", true),
        "parent remains navigable with an active filter");
  check(MatchesNameFilter("\xc3\x85rsrapport.txt", "\xc3\x85r"),
        "UTF-8 literal filename is preserved and matched");
  check(MatchesNameFilter("report [final].txt", "[final]"),
        "filter treats punctuation literally");
  auto exists = [](const std::string &p) {
    struct stat s;
    return lstat(p.c_str(), &s) == 0;
  };
  auto text = [](const std::string &p) {
    std::ifstream f(p);
    return std::string(std::istreambuf_iterator<char>(f), {});
  };
  auto write = [](const std::string &p, const std::string &s) {
    std::ofstream f(p);
    f << s;
  };
  try {
    const auto seed = Join(root, "receipt-seed"), added = Join(root, "receipt-added");
    write(seed, "keep");
    write(added, "addition");
    auto create = PrepareArchiveJob(false, {seed});
    create.archiveFormat = "ar";
    SetArchiveDestination(create, root, "receipt-test");
    if (Run(create, {}).completed != 1)
      throw std::runtime_error("could not create receipt fixture");
    auto edit = PrepareArchiveEditJob(create.destination, {added}, {});
    const auto oldVersion = edit.expectedArchiveSets.at(create.destination);
    const auto saved = Run(edit, {});
    auto guard = ArchiveJobChecks::PublishedArchiveGuard(edit, saved.publishedArchiveVersion);
    ArchiveJobChecks::VerifyArchiveSets(guard);
    bool refused = false;
    try {
      ArchiveJobChecks::PublishedArchiveGuard(edit, oldVersion);
    } catch (const std::runtime_error &) {
      refused = true;
    }
    check(saved.completed == 1 && refused && text(added) == "addition",
          "move cleanup guard accepts writer receipt and rejects prior archive version");
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    check(false, "archive move publication guard");
  }
  try {
    for (char separator : {'\n', '\r'}) {
      const auto prefix = Join(root, std::string("split") + separator + "archive.7z.");
      write(prefix + "001", "first");
      write(prefix + "002", "second");
      const auto set = FindArchiveVolumes(prefix + "002");
      check(set.files.size() == 2 && set.files.front().path == prefix + "001",
            "split archive name containing a line break includes both volumes");
    }
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    check(false, "split archive name with line break");
  }
  PaneModel model;
  for (auto name : {"..", "a.txt", "b.txt", "c.png", "d.txt"}) {
    auto entry = std::make_unique<PaneEntry>();
    entry->name = name;
    entry->path = name;
    entry->parent = entry->name == "..";
    model.Add(std::move(entry));
  }
  model.Filter("");
  model.Pick(1, false, false);
  model.Pick(4, true, false);
  check(model.Selected().size() == 4, "grid Shift selects the complete range");
  model.Pick(2, true, false);
  check(model.Selected().size() == 2,
        "grid Shift range shrinks from its anchor");
  model.Pick(2, false, true);
  check(model.Selected() == std::vector<std::string>{"a.txt"},
        "grid toggle removes one selected entry");
  model.Select(1);
  model.Filter(".txt");
  check(model.Selected().size() == 3 && model.Visible().size() == 4,
        "filter excludes hidden selections and retains the parent");
  model.Filter("");
  check(model.Selected().size() == 3 && !model.IsSelected("c.png"),
        "clearing a filter does not resurrect a hidden selection");
  model.Select(2);
  check(model.Selected() == std::vector<std::string>{"c.png"},
        "invert selection never includes the parent entry");
  model.SetSelection({"d.txt"}, "d.txt");
  check(model.Focused() && model.Focused()->path == "d.txt" &&
            model.Selected() == std::vector<std::string>{"d.txt"},
        "details and grid share pane-owned selection and focus");
  model.Clear();
  check(model.Selected().empty() && !model.Focused(),
        "navigation clears both selection and focus ownership");
  mkdir(Join(root, "src").c_str(), 0755);
  mkdir(Join(root, "dst").c_str(), 0755);
  mkdir(Join(root, "src/tree").c_str(), 0755);
  mkdir(Join(root, "src/tree/nested").c_str(), 0755);
  write(Join(root, "src/tree/nested/file"), "test data\n");
  symlink("nested/file", Join(root, "src/tree/link").c_str());
  BNode node(Join(root, "src/tree/nested/file").c_str());
  const char attribute[] = "Haiku metadata";
  node.WriteAttr("Listi:test", B_STRING_TYPE, 0, attribute, sizeof(attribute));
  Hooks hooks;
  hooks.proceed = [] { return true; };
  hooks.conflict = [](auto &) { return Decision::Replace; };
  Job copy{Action::Copy, {Join(root, "src/tree")}, Join(root, "dst")};
  Result r;
  std::thread copyThread([&] { r = Run(copy, hooks); });
  copyThread.join();
  check(r.errors.empty() && r.completed == 1, "recursive copy completes");
  check(text(Join(root, "dst/tree/nested/file")) == "test data\n",
        "file contents preserved");
  char link[80] = {};
  readlink(Join(root, "dst/tree/link").c_str(), link, 79);
  check(!strcmp(link, "nested/file"), "symbolic link is copied, not traversed");
  BNode target(Join(root, "dst/tree/nested/file").c_str());
  char attr[80] = {};
  target.ReadAttr("Listi:test", B_STRING_TYPE, 0, attr, sizeof(attr));
  check(!strcmp(attr, attribute), "BFS attributes preserved");
  write(Join(root, "dst/tree/nested/file"), "keep me");
  hooks.conflict = [](auto &) { return Decision::SkipAll; };
  r = Run(copy, hooks);
  check(r.skipped >= 1 && text(Join(root, "dst/tree/nested/file")) == "keep me",
        "skip-all preserves destination");
  hooks.conflict = [](auto &) { return Decision::ReplaceAll; };
  r = Run(copy, hooks);
  check(r.errors.empty() &&
            text(Join(root, "dst/tree/nested/file")) == "test data\n",
        "replace-all replaces only confirmed conflicts");
  hooks.conflict = [](auto &) { return Decision::Abort; };
  r = Run(copy, hooks);
  check(r.cancelled, "conflict abort is reported");
  r = Run(
      {Action::Copy, {Join(root, "src/tree")}, Join(root, "src/tree/nested")},
      hooks);
  check(!r.errors.empty(), "copy into own descendant rejected");
  r = Run({Action::Copy, {Join(root, "src/tree")}, Join(root, "src")}, hooks);
  check(!r.errors.empty(), "copy onto itself rejected");
  write(Join(root, "src/large"), std::string(512 * 1024, 'x'));
  bool cancel = false;
  hooks.proceed = [&] { return !cancel; };
  hooks.progress = [&](auto &, uint64_t n) {
    if (n)
      cancel = true;
  };
  hooks.conflict = [](auto &) { return Decision::Replace; };
  r = Run({Action::Copy, {Join(root, "src/large")}, Join(root, "dst")}, hooks);
  check(r.cancelled && !exists(Join(root, "dst/large")),
        "cancel does not expose partial file");
  hooks.proceed = [] { return true; };
  hooks.progress = {};
  r = Run({Action::Move, {Join(root, "src/large")}, Join(root, "dst")}, hooks);
  check(r.errors.empty() && !exists(Join(root, "src/large")) &&
            exists(Join(root, "dst/large")),
        "same-volume move completes");
  // A cancellation after an atomic move cannot undo that move. Only
  // unstarted entries should remain in the source folder.
  for (bool hasNext : {false, true}) {
    std::string name = hasNext ? "batch-first" : "only-entry";
    std::string first = Join(root, "src/" + name);
    std::string moved = Join(root, "dst/" + name);
    write(first, "first");
    Job move{Action::Move, {first}, Join(root, "dst")};
    std::string next = Join(root, "src/batch-next");
    if (hasNext) {
      write(next, "next");
      move.sources.push_back(next);
    }
    Hooks afterMove;
    afterMove.proceed = [&] { return !exists(moved); };
    r = Run(move, afterMove);
    check(r.errors.empty() && r.cancelled == hasNext && r.completed == 1 &&
              r.completedRoots.size() == 1 &&
              r.completedRoots.front() == std::make_pair(first, moved) &&
              !exists(first) && text(moved) == "first" &&
              (!hasNext ||
               (text(next) == "next" && !exists(Join(root, "dst/batch-next")))),
          hasNext ? "cancel between moves counts first and leaves next in place"
                  : "cancel after last atomic move does not undo completion");
  }
  r = Run(
      {Action::Rename, {Join(root, "dst/large")}, Join(root, "dst/renamed")},
      hooks);
  check(r.errors.empty() && exists(Join(root, "dst/renamed")),
        "rename completes");
  r = Run({Action::MakeDirectory, {}, Join(root, "dst/new folder")}, hooks);
  check(r.errors.empty() && exists(Join(root, "dst/new folder")),
        "create folder completes");
  symlink(Join(root, "src/tree").c_str(),
          Join(root, "dst/linked-directory").c_str());
  r = Run({Action::Delete, {Join(root, "dst/linked-directory")}, ""}, hooks);
  check(r.errors.empty() && exists(Join(root, "src/tree/nested/file")),
        "delete symbolic link leaves target untouched");
  const auto removed = Join(root, "dst/delete-last");
  write(removed, "last");
  Hooks afterDelete;
  afterDelete.proceed = [&] { return exists(removed); };
  r = Run({Action::Delete, {removed}, ""}, afterDelete);
  check(!r.cancelled && r.completed == 1 && r.errors.empty() && !exists(removed),
        "cancel after the last removal does not undo deletion");
  mkdir(Join(root, "merge").c_str(), 0755);
  mkdir(Join(root, "merge/tree").c_str(), 0755);
  r = Run({Action::Move, {Join(root, "src/tree")}, Join(root, "merge")}, hooks);
  check(r.errors.empty() && !exists(Join(root, "src/tree")) &&
            text(Join(root, "merge/tree/nested/file")) == "test data\n",
        "merged move removes only successfully copied snapshot");
  mkdir(Join(root, "change").c_str(), 0755);
  mkdir(Join(root, "change/tree").c_str(), 0755);
  bool mutated = false;
  hooks.progress = [&](const std::string &, uint64_t n) {
    if (n && !mutated) {
      mutated = true;
      write(Join(root, "merge/tree/new-file"), "created during copy");
    }
  };
  r = Run({Action::Move, {Join(root, "merge/tree")}, Join(root, "change")},
          hooks);
  check(!r.errors.empty() && exists(Join(root, "merge/tree/new-file")) &&
            exists(Join(root, "merge/tree/nested/file")),
        "changed source tree is retained during merged move");
  hooks.progress = {};
  // Force the copy/cleanup move path by providing an existing destination.
  write(Join(root, "src/move-progress"), "new data");
  write(Join(root, "dst/move-progress"), "old data");
  int completedEvents = 0;
  bool reportedBeforeCleanup = false;
  hooks.fileDone = [&](const std::string &source, const std::string &) {
    ++completedEvents;
    reportedBeforeCleanup |= exists(source);
  };
  r = Run({Action::Move, {Join(root, "src/move-progress")}, Join(root, "dst")},
          hooks);
  check(r.completed == 1 && completedEvents == 1 && !reportedBeforeCleanup,
        "move completion is emitted only after source cleanup");
  write(Join(root, "src/move-cancel"), "new data");
  write(Join(root, "dst/move-cancel"), "old data");
  completedEvents = 0;
  hooks.proceed = [&] {
    return text(Join(root, "dst/move-cancel")) != "new data";
  };
  r = Run({Action::Move, {Join(root, "src/move-cancel")}, Join(root, "dst")},
          hooks);
  check(r.cancelled && r.completed == 0 && completedEvents == 0 &&
            exists(Join(root, "src/move-cancel")) &&
            text(Join(root, "dst/move-cancel")) == "new data",
        "cancel before move cleanup retains source without a success event");
  hooks.proceed = [] { return true; };
  hooks.fileDone = {};
  const std::string changing = Join(root, "src/changing");
  write(changing, std::string(262144, 'a'));
  struct stat original;
  stat(changing.c_str(), &original);
  bool changedSource = false;
  hooks.progress = [&](const std::string &, uint64_t n) {
    if (n && !changedSource) {
      changedSource = true;
      usleep(2000);
      write(changing, std::string(262144, 'b'));
      struct timespec times[2] = {original.st_atim, original.st_mtim};
      utimensat(AT_FDCWD, changing.c_str(), times, 0);
    }
  };
  r = Run({Action::Copy, {changing}, Join(root, "dst")}, hooks);
  check(!r.errors.empty() && !exists(Join(root, "dst/changing")),
        "same-size source edit with restored mtime is not committed");
  hooks.progress = {};
  write(Join(root, "src/disappearing"), "new data");
  write(Join(root, "dst/disappearing"), "old data");
  hooks.progress = [&](const std::string &, uint64_t n) {
    if (n)
      unlink(Join(root, "dst/disappearing").c_str());
  };
  r = Run({Action::Copy, {Join(root, "src/disappearing")}, Join(root, "dst")},
          hooks);
  check(!r.errors.empty() && !exists(Join(root, "dst/disappearing")),
        "deleted conflict target is detected before commit");
  hooks.progress = {};
  mkfifo(Join(root, "src/pipe").c_str(), 0600);
  r = Run({Action::Copy, {Join(root, "src/pipe")}, Join(root, "dst")}, hooks);
  check(!r.errors.empty() && !exists(Join(root, "dst/pipe")),
        "local special file is rejected without waiting for a writer");
  r = Run({Action::Delete, {"/boot"}, ""}, hooks);
  check(!r.errors.empty() && exists("/boot/home"),
        "mounted volume deletion refused");
  // Only this run's uniquely named entries are moved to the system Trash.
  std::string owned = Join(root, "trash-test-" + Leaf(root));
  mkdir(owned.c_str(), 0755);
  write(Join(owned, "inside"), "recover me");
  std::string trashed, collision, linked;
  try {
    // The opposite pane is irrelevant for source-only actions, even offline.
    for (const std::string destination :
         {"smb://offline.invalid/share", "archive:///missing.zip!/"}) {
      std::string single = Join(root, "source-only");
      write(single, "preserve local behavior");
      std::string actualTrash;
      hooks.fileDone = [&](const std::string &, const std::string &target) {
        actualTrash = target;
      };
      r = Run({Action::Trash, {single}, destination}, hooks);
      check(r.errors.empty() && r.completed == 1 && !exists(single) &&
                !actualTrash.empty() && exists(actualTrash),
            "local Trash ignores the virtual opposite pane");
      hooks.fileDone = {};
      if (!actualTrash.empty()) {
        r = Run({Action::Restore, {actualTrash}, destination}, hooks);
        check(r.errors.empty() && text(single) == "preserve local behavior",
              "local Restore ignores the virtual opposite pane");
        if (exists(actualTrash))
          Run({Action::Delete, {actualTrash}, ""}, hooks);
      }
      r = Run({Action::Delete, {single}, destination}, hooks);
      check(r.errors.empty() && !exists(single),
            "local Delete ignores the virtual opposite pane");
    }
    trashed = MoveToTrash(owned);
    BString original;
    BNode trashNode(trashed.c_str());
    check(trashNode.ReadAttrString("_trk/original_path", &original) == B_OK &&
              original == owned.c_str() && !exists(owned) &&
              text(Join(trashed, "inside")) == "recover me",
          "native Trash preserves tree and Tracker original-path metadata");
    mkdir(owned.c_str(), 0755);
    write(Join(owned, "inside"), "second tree");
    collision = MoveToTrash(owned);
    check(collision != trashed &&
              text(Join(trashed, "inside")) == "recover me" &&
              text(Join(collision, "inside")) == "second tree",
          "same-name trash entries never overwrite one another");
    write(owned, "existing replacement");
    r = Run({Action::Restore, {trashed}, ""}, hooks);
    check(!r.errors.empty() && exists(trashed) &&
              text(owned) == "existing replacement",
          "restore conflict retains both current file and trashed tree");
    unlink(owned.c_str());
    r = Run({Action::Restore, {trashed}, ""}, hooks);
    attr_info metadata;
    BNode restored(owned.c_str());
    check(
        r.errors.empty() && r.completed == 1 && !exists(trashed) &&
            text(Join(owned, "inside")) == "recover me" &&
            restored.GetAttrInfo("_trk/original_path", &metadata) != B_OK,
        "restore returns tree to original location and removes Trash metadata");
    r = Run({Action::Trash, {collision}, ""}, hooks);
    check(!r.errors.empty() && exists(collision),
          "already trashed entry is retained");
    r = Run({Action::Restore, {owned}, ""}, hooks);
    check(!r.errors.empty() && exists(owned),
          "restore outside Trash is rejected");
    std::string symlinkPath = Join(root, "link-" + Leaf(root));
    symlink(owned.c_str(), symlinkPath.c_str());
    linked = MoveToTrash(symlinkPath);
    char linkValue[PATH_MAX] = {};
    readlink(linked.c_str(), linkValue, sizeof(linkValue) - 1);
    check(std::string(linkValue) == owned && exists(Join(owned, "inside")),
          "trashing a symlink leaves its target untouched");
    r = Run({Action::Restore, {linked}, ""}, hooks);
    check(r.errors.empty() && exists(symlinkPath) && exists(owned),
          "trashed symlink restores without traversing target");
    hooks.proceed = [] { return false; };
    r = Run({Action::Trash, {owned}, ""}, hooks);
    check(r.cancelled && exists(owned),
          "cancelled trash leaves source untouched");
    hooks.proceed = [] { return true; };
    r = Run({Action::Trash, {"/boot"}, ""}, hooks);
    check(!r.errors.empty() && exists("/boot/home"),
          "trashing a volume root is refused");
    std::string missing = Join(root, "missing-parent");
    mkdir(missing.c_str(), 0755);
    write(Join(missing, "file"), "keep if parent missing");
    std::string missingTrashed = MoveToTrash(Join(missing, "file"));
    rmdir(missing.c_str());
    r = Run({Action::Restore, {missingTrashed}, ""}, hooks);
    check(!r.errors.empty() && exists(missingTrashed),
          "missing original parent leaves trashed file intact with error");
    Run({Action::Delete, {missingTrashed}, ""}, hooks);
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    check(false, "native Trash round trip");
  }
  for (auto path : {trashed, collision, linked})
    if (!path.empty() && exists(path))
      Run({Action::Delete, {path}, ""}, hooks);
  r = Run({Action::Delete, {root}, ""}, hooks);
  check(r.errors.empty() && !exists(root),
        "recursive delete of owned test tree completes");
  std::cout << checks << " checks, " << failures << " failures" << std::endl;
  return failures ? 1 : 0;
}
} // namespace Listi
