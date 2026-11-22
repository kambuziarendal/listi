// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "ArchiveJobChecks.h"
#include "DataIsoVerification.h"
#include "ArchiveBackend.h"
#include "ArchiveVolumes.h"
#include "ArchiveWriting.h"
#include "JobSnapshot.h"
#include "LocalMoveTree.h"
#include "VirtualFile.h"
#include <Directory.h>
#include <Entry.h>
#include <Node.h>
#include <Path.h>
#include <dirent.h>
#include <fcntl.h>
#include <fs_attr.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cctype>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <set>

namespace Listi {
namespace {
using namespace ArchiveJobChecks;
std::string PathOf(BDirectory &dir) {
  BEntry e;
  BPath p;
  Require(dir.GetEntry(&e) == B_OK && e.GetPath(&p) == B_OK,
          "The working folder disappeared.");
  return p.Path();
}
bool RemoveContents(BDirectory &dir, dev_t device, unsigned depth = 0) {
  if (depth > 64 || dir.SetPermissions(0700) != B_OK)
    return false;
  bool clean = true;
  BEntry entry;
  status_t next;
  while ((next = dir.GetNextEntry(&entry, false)) == B_OK) {
    struct stat st;
    if (entry.GetStat(&st) != B_OK || st.st_dev != device) {
      clean = false;
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      BDirectory child(&entry);
      node_ref opened;
      if (child.GetNodeRef(&opened) != B_OK || opened.device != st.st_dev ||
          opened.node != st.st_ino || !RemoveContents(child, device, depth + 1)) {
        clean = false;
        continue;
      }
    }
    struct stat current;
    if (entry.GetStat(&current) != B_OK || current.st_dev != st.st_dev ||
        current.st_ino != st.st_ino || entry.Remove() != B_OK)
      clean = false;
  }
  return clean && next == B_ENTRY_NOT_FOUND;
}
struct Stage {
  BDirectory parent, dir;
  std::string path, leaf;
  node_ref identity;
  Result &result;
  Stage(const std::string &folder, Result &result)
      : parent(folder.c_str()), result(result) {
    Require(parent.InitCheck() == B_OK, "Cannot open destination folder.");
    std::string pattern = Join(folder, ".Listi-archive-stage-XXXXXX");
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back(0);
    char *made = mkdtemp(bytes.data());
    Require(made, "Cannot create archive staging folder.");
    path = made;
    leaf = Leaf(path);
    BEntry created(&parent, leaf.c_str(), false);
    Require(dir.SetTo(&created) == B_OK && dir.GetNodeRef(&identity) == B_OK,
            "Cannot open archive staging folder; inspect " + path);
    path = PathOf(dir);
    RegisterArchiveStage(path, identity.device, identity.node);
  }
  ~Stage() {
    try {
      ForgetArchiveStage(path);
      BEntry entry(&parent, leaf.c_str(), false);
      node_ref named;
      if (entry.GetNodeRef(&named) == B_OK &&
          named.device == identity.device && named.node == identity.node &&
          RemoveContents(dir, identity.device) &&
          entry.GetNodeRef(&named) == B_OK &&
          named.device == identity.device && named.node == identity.node &&
          entry.Remove() == B_OK)
        return;
      result.errors.push_back("Could not clean the archive staging folder; inspect " +
                              path + " (show hidden files).");
    } catch (...) {
      // Do not let diagnostic allocation failures mask the original error.
    }
  }
};
std::vector<std::string> Children(const std::string &folder) {
  DIR *raw = opendir(folder.c_str());
  Require(raw, "Cannot list archive staging folder.");
  std::unique_ptr<DIR, int (*)(DIR *)> dir(raw, closedir);
  std::vector<std::string> result;
  for (;;) {
    errno = 0;
    auto child = readdir(raw);
    if (!child) {
      Require(!errno, "Cannot finish listing archive staging folder.");
      break;
    }
    if (!strcmp(child->d_name, ".") || !strcmp(child->d_name, ".."))
      continue;
    result.push_back(Join(folder, child->d_name));
  }
  std::sort(result.begin(), result.end());
  return result;
}
void RemoveTree(const std::string &path, const Hooks &hooks,
                unsigned depth = 0) {
  if (hooks.proceed && !hooks.proceed())
    throw Cancelled();
  Require(depth <= 64, "Archive edit exceeds the folder depth limit.");
  struct stat st;
  Require(!lstat(path.c_str(), &st),
          "Archive entry selected for removal disappeared.");
  Require(!S_ISLNK(st.st_mode),
          "Archive edit staging unexpectedly contains a link.");
  if (S_ISDIR(st.st_mode))
    for (const auto &child : Children(path))
      RemoveTree(child, hooks, depth + 1);
  Require(!(S_ISDIR(st.st_mode) ? rmdir(path.c_str()) : unlink(path.c_str())),
          "Cannot remove an entry from archive staging.");
}
void CopyMetadata(const std::string &fromPath, const std::string &toPath,
                  const Hooks &hooks) {
  struct stat st;
  Require(!stat(fromPath.c_str(), &st), "Cannot inspect archive metadata.");
  Require(!chmod(toPath.c_str(), st.st_mode & 07777),
          "Cannot preserve archive permissions.");
  BNode from(fromPath.c_str()), to(toPath.c_str());
  Require(from.InitCheck() == B_OK && to.InitCheck() == B_OK,
          "Cannot open archive metadata.");
  char name[B_ATTR_NAME_LENGTH];
  std::vector<char> bytes(65536);
  status_t status;
  while ((status = from.GetNextAttrName(name)) == B_OK) {
    if (hooks.proceed && !hooks.proceed())
      throw Cancelled();
    attr_info info;
    Require(from.GetAttrInfo(name, &info) == B_OK,
            "Archive metadata changed during editing.");
    if (!info.size) {
      Require(to.WriteAttr(name, info.type, 0, nullptr, 0) == 0,
              "Cannot preserve empty archive metadata.");
      continue;
    }
    off_t at = 0;
    while (at < info.size) {
      auto amount = std::min<off_t>(bytes.size(), info.size - at);
      auto got = from.ReadAttr(name, info.type, at, bytes.data(), amount);
      Require(got > 0, "Cannot read archive metadata.");
      Require(to.WriteAttr(name, info.type, at, bytes.data(), got) == got,
              "Cannot preserve archive metadata.");
      at += got;
    }
  }
  Require(status == B_ENTRY_NOT_FOUND, "Cannot enumerate archive metadata.");
}
void VerifyMovedEntry(const std::string &, const std::string &,
                      const TransferControl &, bool exactTree = false, unsigned depth = 0);
Result RunArchiveEdit(const Job &job, const Hooks &hooks,
                      const std::function<void()> &beforePublish = {}) {
  Result result;
  try {
    Require(!job.destination.empty() && !job.destinationFolder.empty(),
            "Archive edit target is missing.");
    Require(!job.sources.empty() || !job.archiveRemove.empty() ||
                !job.archiveFolders.empty() || !job.archiveRenames.empty(),
            "Choose an archive change first.");
    Guards(job);
    VerifyArchiveSets(job);
    const auto format = DetectArchiveEditFormat(job.destination, hooks);
    Require(format == job.archiveFormat,
            "Archive format changed after selection.");
    const auto passwordGeneration = ArchivePasswordGeneration();
    const auto originalSet = FindArchiveVolumes(job.destination);
    ArchiveEditProtection protection(originalSet.files.front().identity,
                                     originalSet.Fingerprint());
    Stage stage(job.destinationFolder, result);
    const auto work = Join(PathOf(stage.dir), "contents");
    Require(!mkdir(work.c_str(), 0700),
            "Cannot create archive edit staging folder.");
    auto check = [&] {
      if (hooks.proceed && !hooks.proceed())
        throw Cancelled();
      CheckArchivePasswordGeneration(passwordGeneration);
      Guards(job);
      VerifyArchiveSets(job);
    };
    TransferControl control;
    control.proceed = [&] {
      check();
      return true;
    };
    control.progress = [&](uint64_t bytes) {
      if (hooks.progress)
        hooks.progress(job.destination, bytes);
    };
    Hooks internal = hooks;
    internal.fileDone = {};
    internal.fileFailed = {};
    internal.proceed = [&] {
      check();
      return true;
    };
    internal.progress = [&](const std::string &, uint64_t bytes) {
      if (hooks.progress)
        hooks.progress(job.destination, bytes);
    };
    check();
    const auto root = OpenArchive(job.destination, control);
    Require(!root.empty(), "The selected file is not a supported archive.");
    auto top = ListFiles(root, control);
    if (!top.empty()) {
      Job extract;
      extract.action = Action::Copy;
      extract.destination = work;
      for (const auto &entry : top)
        extract.sources.push_back(entry.path);
      auto extraction = internal;
      extraction.conflict = {};
      auto copied = Run(extract, extraction);
      if (copied.cancelled)
        throw Cancelled();
      Require(copied.errors.empty(), copied.errors.empty()
                                         ? "Archive extraction failed."
                                         : copied.errors.front());
      Require(!copied.skipped,
              "Archive contains conflicting entries and was not changed.");
    }
    for (const auto &member : job.archiveRemove) {
      check();
      const auto relative = MemberPath(member);
      Require(!relative.empty(), "The archive root cannot be removed.");
      const auto target = Join(work, relative);
      struct stat st;
      Require(!lstat(target.c_str(), &st),
              "An archive entry selected for removal no longer exists.");
      RemoveTree(target, internal);
    }
    for (const auto &path : job.archiveFolders) {
      check();
      const auto relative = MemberPath(path);
      Require(!relative.empty(), "The archive root already exists.");
      Require(!mkdir(Join(work, relative).c_str(), 0755),
              "Could not create archive folder. The name may already exist.");
    }
    for (const auto &change : job.archiveRenames) {
      check();
      const auto from = MemberPath(change.first),
                 to = MemberPath(change.second);
      Require(
          !from.empty() && !to.empty() && to != from &&
              to.compare(0, from.size() + 1, from + "/") != 0,
          "An archive entry cannot be moved onto itself or into its contents.");
      BEntry entry(Join(work, from).c_str(), false);
      BDirectory parent(ParentLocation(Join(work, to)).c_str());
      Require(
          entry.MoveTo(&parent, Leaf(to).c_str(), false) == B_OK,
          "Could not rename or move the archive entry. Existing entries were not replaced.");
    }
    if (!job.sources.empty()) {
      const auto relative = MemberPath(job.archivePath),
                 target = relative.empty() ? work : Join(work, relative);
      struct stat targetStat;
      Require(!stat(target.c_str(), &targetStat) && S_ISDIR(targetStat.st_mode),
              "The destination folder inside the archive no longer exists.");
      Job add;
      add.action = Action::Copy;
      add.destination = target;
      add.destinationFolder = target;
      add.expectedDestination = {int32_t(targetStat.st_dev),
                                 int64_t(targetStat.st_ino)};
      for (const auto &source : job.sources) {
        auto location = ParseLocation(source);
        if (location.Archive() && location.archive == job.destination) {
          const auto relative = MemberPath(location.path);
          Require(!relative.empty(), "An archive cannot be added to itself.");
          const auto staged = Join(work, relative);
          struct stat st;
          Require(!lstat(staged.c_str(), &st),
                  "The source archive entry disappeared.");
          add.sources.push_back(staged);
          add.expectedSources[staged] = {int32_t(st.st_dev),
                                         int64_t(st.st_ino)};
        } else {
          add.sources.push_back(source);
          if (!IsVirtual(source))
            add.expectedSources[source] = job.expectedSources.at(source);
        }
      }
      auto adding = internal;
      adding.conflict = [&](const std::string &destination) {
        if (!hooks.conflict)
          return Decision::Abort;
        auto relative = destination.size() > work.size() &&
                                destination.compare(0, work.size(), work) == 0
                            ? destination.substr(work.size())
                            : std::string("/") + Leaf(destination);
        if (relative.empty() || relative.front() != '/')
          relative = "/" + relative;
        return hooks.conflict(job.destination + " → " + relative);
      };
      auto added = Run(add, adding);
      if (added.cancelled)
        throw Cancelled();
      Require(added.errors.empty(), added.errors.empty()
                                        ? "Could not add files to archive."
                                        : added.errors.front());
      Require(!added.skipped, "Archive edit was cancelled at a name conflict.");
    }
    check();
    const auto outputPath = Join(PathOf(stage.dir), "updated");
    FD output(
        open(outputPath.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
    Require(output.n >= 0, "Cannot create updated archive output.");
    auto secret = protection.Password();
    Require(!secret || format == "zip" || format == "7z",
            "The writer cannot preserve this archive's password protection.");
    CreateArchiveOutput(output.n, format, Children(work), job.archiveLevel,
                        secret && format == "7z", secret, internal);
    Require(!fsync(output.n), "Cannot flush updated archive to disk.");
    CopyMetadata(job.destination, outputPath, internal);
    if (format == "iso")
      SealDataIso(output.n, internal);
    Require(!fsync(output.n), "Cannot flush updated archive metadata.");
    check();
    struct stat verifiedOutput;
    Require(!fstat(output.n, &verifiedOutput), "Cannot identify updated archive.");
    {
      ArchiveListingScope listingScope;
      if (secret) {
        struct stat st;
        Require(!lstat(outputPath.c_str(), &st),
                "Updated archive disappeared before verification.");
        RememberArchivePassword(ArchivePasswordIdentity(st), secret,
                                passwordGeneration);
        RememberArchivePassword(FindArchiveVolumes(outputPath).Fingerprint(),
                                secret, passwordGeneration);
      }
      TransferControl verify;
      verify.proceed = internal.proceed;
      // Report the verification phase without double-counting transfer bytes.
      verify.progress = [&](uint64_t) {
        if (hooks.progress)
          hooks.progress(job.destination, 0);
      };
      const auto root = OpenArchive(outputPath, verify);
      Require(
          !root.empty(),
          "Updated archive could not be reopened. Original archive was retained.");
      const auto children = Children(work);
      Require(
          ListFiles(root, verify).size() == children.size(),
          "Updated archive has a different member set. Original archive was retained.");
      for (const auto &child : children)
        VerifyMovedEntry(child, ChildLocation(root, Leaf(child)), verify, true);
      check();
    }
    // Full external tree guards belong at the destructive publication boundary,
    // not in every decoder progress callback. No callback or I/O that changes
    // the destination is allowed between this guard and the archive rename.
    if (beforePublish)
      beforePublish();
    struct stat original, named;
    FD originalFd(
        open(job.destination.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK));
    Require(
        originalFd.n >= 0 && !fstat(originalFd.n, &original) &&
            !lstat(job.destination.c_str(), &named) &&
            Unchanged(original, named),
        "The archive was replaced while it was being edited. Nothing was changed.");
    struct stat namedOutput;
    Require(!lstat(outputPath.c_str(), &namedOutput) &&
                Unchanged(verifiedOutput, namedOutput),
            "The verified output changed. Original archive was retained.");
    FD parent(open(job.destinationFolder.c_str(), O_RDONLY | O_DIRECTORY));
    Require(parent.n >= 0,
            "Cannot open the archive directory before publishing. Original archive was retained.");
    Require(
        !rename(outputPath.c_str(), job.destination.c_str()),
        "Could not publish the updated archive. The original was left in place.");
    Require(!fsync(parent.n),
            "Updated archive was published, but its directory could not be flushed. Durability after restart is unknown.");
    if (secret)
      try {
        struct stat published;
        if (!lstat(job.destination.c_str(), &published))
          RememberArchivePassword(ArchivePasswordIdentity(published), secret,
                                  passwordGeneration);
        RememberArchivePassword(
            FindArchiveVolumes(job.destination).Fingerprint(), secret,
            passwordGeneration);
      } catch (
          ...) { /* Password caching is optional after the archive is durable. */
      }
    result.completed = 1;
    result.completedRoots.push_back({job.destination, job.destination});
    if (hooks.fileDone)
      hooks.fileDone(job.destination, job.destination);
  } catch (const Cancelled &) {
    result.cancelled = true;
  } catch (const std::exception &e) {
    result.errors.push_back(e.what());
  }
  return result;
}
// Compare the durable destination's decoded bytes with the private source
// snapshot. A directory merge may retain additional destination entries.
void VerifyMovedEntry(const std::string &local, const std::string &destination,
                      const TransferControl &control, bool exactTree, unsigned depth) {
  control.Check();
  Require(depth <= 64, "Move verification exceeds the folder depth limit.");
  struct stat st;
  FileEntry target;
  Require(!lstat(local.c_str(), &st) && !S_ISLNK(st.st_mode) &&
              StatFile(destination, target, control) && !target.link &&
              bool(S_ISDIR(st.st_mode)) == target.directory,
          "Destination verification failed. Source entries were retained.");
  if (S_ISDIR(st.st_mode)) {
    const auto children = Children(local);
    if (exactTree)
      Require(ListFiles(destination, control).size() == children.size(),
              "Updated archive has a different member set. Original archive was retained.");
    for (const auto &child : children)
      VerifyMovedEntry(child, ChildLocation(destination, Leaf(child)), control,
                       exactTree, depth + 1);
    return;
  }
  Require(
      S_ISREG(st.st_mode) && target.sizeKnown && target.size == st.st_size,
      "Destination size verification failed. Source entries were retained.");
  FD input(open(local.c_str(), O_RDONLY | O_NOFOLLOW));
  Require(input.n >= 0, "Cannot read move verification snapshot.");
  std::vector<char> buffer(65536);
  int64_t total = 0;
  ReadFile(
      destination,
      [&](const char *bytes, size_t size) {
        Require(size <= uint64_t(st.st_size - total),
                "Destination verification exceeded the expected content size.");
        while (size) {
          control.Check();
          const auto amount = std::min(size, buffer.size());
          size_t have = 0;
          while (have < amount) {
            auto n = read(input.n, buffer.data() + have, amount - have);
            if (n < 0 && errno == EINTR)
              continue;
            Require(n > 0, "Move verification snapshot is incomplete.");
            have += n;
          }
          Require(!memcmp(bytes, buffer.data(), amount),
                  "Destination bytes differ. Source entries were retained.");
          total += amount;
          bytes += amount;
          size -= amount;
        }
      },
      control);
  Require(
      total == st.st_size,
      "Destination verification was incomplete. Source entries were retained.");
}
Result RunArchiveLocalMove(const Job &job, const Hooks &hooks) {
  Result result;
  bool copied = false, cleanup = false;
  try {
    const auto source = ParseLocation(job.sources.front());
    const auto target = ParseLocation(job.destination);
    Hooks internal = hooks;
    internal.fileDone = {};
    internal.fileFailed = {};
    internal.proceed = [&] {
      if (hooks.proceed && !hooks.proceed())
        throw Cancelled();
      Guards(job);
      VerifyArchiveSets(job);
      return true;
    };
    std::unique_ptr<LocalMoveTree> localSource;
    if (source.Local())
      localSource = std::make_unique<LocalMoveTree>(
          job.sources, internal, true, job.expectedDestination.device);
    Stage stage(job.destinationFolder, result);
    const auto snapshot = PathOf(stage.dir);
    Job extract{Action::Copy, job.sources, snapshot};
    FreezeLocalJob(extract, snapshot);
    auto extracted = Run(extract, internal);
    if (extracted.cancelled)
      throw Cancelled();
    Require(extracted.errors.empty() && !extracted.skipped &&
                extracted.completed == int(job.sources.size()),
            extracted.errors.empty()
                ? "Could not snapshot the complete selection. Sources retained."
                : extracted.errors.front());
    if (localSource)
      localSource->Verify();
    std::vector<std::string> additions, destinations;
    for (const auto &path : job.sources) {
      additions.push_back(Join(snapshot, LocationLeaf(path)));
      destinations.push_back(
          ChildLocation(job.destination, LocationLeaf(path)));
    }
    Job published;
    std::unique_ptr<LocalMoveTree> localDestination;
    if (target.Archive()) {
      auto add =
          PrepareArchiveEditJob(target.archive, additions, {}, target.path);
      auto written = RunArchiveEdit(add, internal);
      if (written.cancelled)
        throw Cancelled();
      Require(written.errors.empty() && written.completed == 1,
              written.errors.empty() ? "Destination archive update failed."
                                     : written.errors.front());
      copied = true;
      published = PrepareArchiveEditTarget(target.archive);
    } else {
      // Local directory merges may publish several files. A skipped or failed
      // file retains every source member; never treat a partial copy as a move.
      Job copy{Action::Copy, additions, job.destination};
      copy.copyForArchiveMove = true;
      FreezeLocalJob(copy, job.destination);
      copied = true;
      auto written = Run(copy, internal);
      if (written.cancelled)
        throw Cancelled();
      Require(written.errors.empty() && !written.skipped &&
                  written.completed == int(additions.size()),
              written.errors.empty()
                  ? "Incomplete destination copy. Source archive retained."
                  : written.errors.front());
      localDestination = std::make_unique<LocalMoveTree>(
          destinations, internal, false, job.expectedDestination.device);
      localDestination->Flush();
    }
    auto destinationGuard = [&] {
      if (hooks.proceed && !hooks.proceed())
        throw Cancelled();
      if (localDestination)
        localDestination->Verify();
      else {
        Guards(published);
        VerifyArchiveSets(published);
      }
      return true;
    };
    internal.proceed = destinationGuard;
    TransferControl verify;
    // The frozen local tree is checked before and after byte verification and
    // again immediately before removing the source archive. Rechecking the
    // whole tree for every read/header makes a move superlinear without making
    // publication atomic. Archive destinations have cheap fingerprint guards.
    verify.proceed = localDestination ? hooks.proceed : destinationGuard;
    destinationGuard();
    for (size_t i = 0; i < additions.size(); ++i)
      VerifyMovedEntry(additions[i], destinations[i], verify);
    destinationGuard();
    if (localSource) {
      localSource->Verify();
      destinationGuard();
      cleanup = true;
      for (const auto &path : job.sources) {
        localSource->Remove(path, internal);
        ++result.completed;
        const auto destination =
            ChildLocation(job.destination, LocationLeaf(path));
        result.completedRoots.emplace_back(path, destination);
        if (hooks.fileDone)
          hooks.fileDone(path, destination);
      }
    } else {
      Guards(job);
      VerifyArchiveSets(job);
      auto remove = PrepareArchiveEditJob(source.archive, {}, job.sources);
      // Bind source and verified destination guards through the final rewrite.
      internal.proceed = [&] {
        if (hooks.proceed && !hooks.proceed())
          throw Cancelled();
        Guards(job);
        VerifyArchiveSets(job);
        return true;
      };
      cleanup = true;
      const auto removed = RunArchiveEdit(remove, internal, [&] {
        destinationGuard();
        Guards(job);
        VerifyArchiveSets(job);
      });
      if (removed.cancelled)
        throw Cancelled();
      Require(removed.errors.empty() && removed.completed == 1,
              removed.errors.empty() ? "Source cleanup did not complete."
                                     : removed.errors.front());
      for (const auto &path : job.sources) {
        ++result.completed;
        const auto destination =
            ChildLocation(job.destination, LocationLeaf(path));
        result.completedRoots.emplace_back(path, destination);
        if (hooks.fileDone)
          hooks.fileDone(path, destination);
      }
    }
  } catch (const Cancelled &) {
    result.cancelled = true;
  } catch (const std::exception &e) {
    result.errors.push_back(e.what());
  }
  if (copied && (result.cancelled || !result.errors.empty()))
    result.errors.push_back(
        cleanup
            ? "Destination copies were verified, but source cleanup was interrupted. Check both locations; remaining sources were retained."
            : "Destination copies may remain. Source entries were retained. Check both locations before retrying.");
  return result;
}
Result RunArchiveMove(const Job &job, const Hooks &hooks) {
  ArchiveListingScope listingScope;
  Result result;
  bool copied = false;
  try {
    Guards(job);
    VerifyArchiveSets(job);
    Require(job.action == Action::MoveArchive, "Invalid archive move job.");
    // Revalidate the complete structure even for decoded/queued jobs.
    PrepareArchiveMove(job);
    const auto source = ParseLocation(job.sources.front());
    const auto target = ParseLocation(job.destination);
    if (source.Local() || target.Local())
      return RunArchiveLocalMove(job, hooks);
    Hooks internal = hooks;
    internal.fileDone = {};
    internal.fileFailed = {};
    auto sourceGuard = PrepareArchiveEditJob(source.archive, {}, job.sources);
    auto checkSource = [&] {
      if (hooks.proceed && !hooks.proceed())
        throw Cancelled();
      Guards(sourceGuard);
      VerifyArchiveSets(sourceGuard);
    };
    internal.proceed = [&] {
      checkSource();
      Guards(job);
      VerifyArchiveSets(job);
      return true;
    };
    if (source.archive == target.archive) {
      auto rename = PrepareArchiveMutation(
          Job{Action::Move, job.sources, job.destination});
      result = RunArchiveEdit(rename, internal);
    } else {
      Stage stage(job.destinationFolder, result);
      const auto snapshot = PathOf(stage.dir);
      Job extract{Action::Copy, job.sources, snapshot};
      FreezeLocalJob(extract, snapshot);
      auto extracted = Run(extract, internal);
      if (extracted.cancelled)
        throw Cancelled();
      Require(
          extracted.errors.empty() && !extracted.skipped &&
              extracted.completed == int(job.sources.size()),
          extracted.errors.empty()
              ? "Could not snapshot the complete selection. Archives are unchanged."
              : extracted.errors.front());
      std::vector<std::string> additions;
      for (const auto &path : job.sources)
        additions.push_back(Join(snapshot, LocationLeaf(path)));
      auto add =
          PrepareArchiveEditJob(target.archive, additions, {}, target.path);
      auto written = RunArchiveEdit(add, internal);
      if (written.cancelled)
        throw Cancelled();
      Require(written.errors.empty() && written.completed == 1,
              written.errors.empty() ? "Destination archive update failed."
                                     : written.errors.front());
      copied = true;
      // Destination is now committed and fsynced. Never roll it back to stale
      // bytes if source cleanup fails; retain both copies and report the failure.
      auto published = PrepareArchiveEditTarget(target.archive);
      sourceGuard.expectedSources.insert(published.expectedSources.begin(),
                                         published.expectedSources.end());
      sourceGuard.expectedArchiveSets.insert(
          published.expectedArchiveSets.begin(),
          published.expectedArchiveSets.end());
      internal.proceed = [&] {
        checkSource();
        Guards(published);
        return true;
      };
      TransferControl verify;
      verify.proceed = internal.proceed;
      for (const auto &path : job.sources)
        VerifyMovedEntry(Join(snapshot, LocationLeaf(path)),
                         ChildLocation(job.destination, LocationLeaf(path)),
                         verify);
      checkSource();
      // Rebuild the source only while the verified destination identity and
      // contents remain frozen, including the final pre-rename guard.
      result = RunArchiveEdit(sourceGuard, internal);
    }
    if (!result.cancelled && result.errors.empty() && result.completed == 1) {
      result.completedRoots.clear();
      result.completed = job.sources.size();
      for (const auto &path : job.sources) {
        const auto destination =
            ChildLocation(job.destination, LocationLeaf(path));
        result.completedRoots.emplace_back(path, destination);
        if (hooks.fileDone)
          hooks.fileDone(path, destination);
      }
    } else {
      result.completed = 0;
      result.completedRoots.clear();
      if (copied)
        result.errors.push_back(
            "Destination copy was saved, but source cleanup did not complete. Check both archives before retrying.");
    }
  } catch (const Cancelled &) {
    result.cancelled = true;
    if (copied)
      result.errors.push_back(
          "Move cancelled after copying. Source entries were retained; destination copies may remain.");
  } catch (const std::exception &e) {
    result.errors.push_back(e.what());
    if (copied)
      result.errors.push_back(
          "Destination copy was saved. Source cleanup did not complete; check both archives before retrying.");
  }
  return result;
}
}
Result RunArchiveJob(const Job &job, const Hooks &hooks) {
  // Editor workers and the operation queue must not publish competing edits.
  // Waiting remains cancellable; external processes are covered by fingerprints.
  static std::recursive_mutex edits;
  std::unique_lock<std::recursive_mutex> ownership(edits, std::defer_lock);
  if (job.action == Action::EditArchive || job.action == Action::MoveArchive) {
    while (!ownership.try_lock()) {
      if (hooks.proceed && !hooks.proceed()) {
        Result result;
        result.cancelled = true;
        return result;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  if (job.action == Action::MoveArchive)
    return RunArchiveMove(job, hooks);
  if (job.action == Action::EditArchive)
    return RunArchiveEdit(job, hooks);
  Result result;
  std::set<std::string> completedPaths;
  Hooks tracked = hooks;
  tracked.progress = [&](const std::string &path, uint64_t bytes) {
    completedPaths.insert(path);
    if (hooks.progress)
      hooks.progress(path, bytes);
  };
  try {
    Require(!job.sources.empty(), "Archive source selection is empty.");
    for (const auto &source : job.sources)
      Require(job.expectedSources.count(LocalSource(source)),
              "Archive source identity is missing.");
    Guards(job);
    VerifyArchiveSets(job);
    Require(!job.destinationFolder.empty() && !job.entryName.empty(),
            "Archive destination is missing.");
    struct stat target;
    Require(
        lstat(job.destination.c_str(), &target) != 0 && errno == ENOENT,
        "The destination already exists or is unavailable. Choose another name; nothing was replaced.");
    for (const auto &source : job.sources)
      if (!IsVirtual(source)) {
        char *src = realpath(source.c_str(), nullptr),
             *dst = realpath(job.destinationFolder.c_str(), nullptr);
        std::string a = src ? src : "", b = dst ? dst : "";
        free(src);
        free(dst);
        Require(!a.empty() && !b.empty(), "Cannot resolve archive paths.");
        Require(b != a && b.compare(0, a.size() + 1, a + "/") != 0,
                "Choose a destination outside the selected folder.");
      }
    Stage stage(job.destinationFolder, result);
    Guards(job);
    auto check = [&] {
      if (hooks.proceed && !hooks.proceed())
        throw Cancelled();
      Guards(job);
    };
    check();
    BEntry publish;
    if (job.action == Action::CreateArchive) {
      auto path = Join(PathOf(stage.dir), "archive");
      FD output(
          open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
      Require(output.n >= 0, "Cannot create archive output.");
      auto secret = TakeArchiveCreationPassword(job.archivePasswordToken);
      Require(
          !secret || job.archiveFormat == "zip" || job.archiveFormat == "7z",
          "The selected archive format does not support password protection.");
      Require(!job.archiveEncryptNames || (secret && job.archiveFormat == "7z"),
              "Only encrypted 7z archives can hide file names.");
      CreateArchiveOutput(output.n, job.archiveFormat, job.sources,
                          job.archiveLevel, job.archiveEncryptNames, secret,
                          tracked);
      if (job.archiveFormat == "iso")
        SealDataIso(output.n, tracked);
      Require(!fsync(output.n), "Cannot flush archive to disk.");
      Require(publish.SetTo(&stage.dir, "archive", false) == B_OK,
              "Archive output disappeared.");
    } else {
      std::vector<std::pair<std::string, struct stat>> original;
      for (const auto &item : job.expectedSources) {
        struct stat s;
        Require(!lstat(item.first.c_str(), &s), "Archive disappeared.");
        original.push_back({item.first, s});
      }
      TransferControl control;
      control.proceed = [&] {
        check();
        return true;
      };
      auto selected = job.sources;
      if (!IsVirtual(selected[0])) {
        const auto root = OpenArchive(selected[0], control);
        Require(!root.empty(), "The selected file is not a supported archive.");
        selected.clear();
        for (const auto &entry : ListFiles(root, control))
          selected.push_back(entry.path);
      }
      Job copy;
      copy.sources = selected;
      copy.destination = PathOf(stage.dir);
      Hooks guarded = tracked;
      // Defer all member completion notifications until atomic publication.
      // Otherwise the UI leaves successfully extracted members marked failed.
      guarded.fileDone = [&](const std::string &path, const std::string &) {
        completedPaths.insert(path);
      };
      guarded.fileFailed = {};
      guarded.proceed = [&] {
        check();
        return true;
      };
      auto copied = Run(copy, guarded);
      if (copied.cancelled)
        throw Cancelled();
      Require(copied.errors.empty(),
              copied.errors.empty() ? "" : copied.errors.front());
      Require(
          !copied.skipped,
          "Archive contains conflicting entries; extraction was not published.");
      for (const auto &item : original) {
        struct stat s;
        Require(!lstat(item.first.c_str(), &s) && Unchanged(item.second, s),
                "Archive changed during extraction.");
      }
      Require(stage.dir.GetEntry(&publish) == B_OK,
              "Extraction staging folder disappeared.");
    }
    check();
    VerifyArchiveSets(job);
    // Native no-clobber publication. A new target appearing while the job runs
    // must never be overwritten, including an empty directory or a symlink.
    Require(
        publish.MoveTo(&stage.parent, job.entryName.c_str(), false) == B_OK,
        "Could not publish the result. The destination may already exist; it was not replaced.");
    if (job.action == Action::ExtractArchive)
      stage.dir.Unset();
    result.completed = 1;
    completedPaths.insert(job.sources.begin(), job.sources.end());
    for (const auto &source : completedPaths)
      if (hooks.fileDone)
        hooks.fileDone(source, job.destination);
  } catch (const Cancelled &) {
    result.cancelled = true;
  } catch (const std::exception &e) {
    result.errors.push_back(e.what());
  }
  return result;
}
}
