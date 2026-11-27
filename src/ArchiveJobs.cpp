// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "ArchiveJobChecks.h"
#include "DataIsoVerification.h"
#include "DirectoryFinalization.h"
#include "EmptyAttribute.h"
#include "ArchiveMetadata.h"
#include "RemovableCopy.h"
#include "ArchiveBackend.h"
#include "ArchiveVolumes.h"
#include "ArchiveWriting.h"
#include "JobSnapshot.h"
#include "LocalMoveTree.h"
#include "RecoveryInventory.h"
#include "PrivateScratch.h"
#include "StagingSpace.h"
#include <utility>
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
#include <cstdlib>
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
struct Stage {
  BDirectory parent, dir;
  std::string path, leaf;
  node_ref identity;
  Result &result;
  std::optional<RecoveryRecord> recovery;
  std::optional<PinnedDirectory> namespaceParent;
  bool published = false;
  bool retainRecovery = false;
  Stage(const Job &job, Result &result, const Hooks &hooks)
      : parent(job.destinationFolder.c_str()), result(result) {
    Require(parent.InitCheck() == B_OK, "Cannot open destination folder.");
    const auto parentPath = PathOf(parent);
    node_ref parentIdentity;
    Require(parent.GetNodeRef(&parentIdentity) == B_OK,
            "Cannot identify the publication directory.");
    namespaceParent.emplace(parentPath, parentIdentity.device, parentIdentity.node);
    recovery.emplace("archive", parentPath, ".Listi-archive-stage-",
                     job.sources.empty() ? job.destination : job.sources.front(), job.destination);
    path = recovery->Stage();
    Require(mkdir(path.c_str(), 0700) == 0, "Cannot create archive staging folder.");
    leaf = Leaf(path);
    BEntry created(&parent, leaf.c_str(), false);
    Require(dir.SetTo(&created) == B_OK && dir.GetNodeRef(&identity) == B_OK,
            "Cannot open archive staging folder; inspect " + path);
    path = PathOf(dir);
    try {
      struct stat identity;
      Require(dir.GetStat(&identity) == B_OK, "Cannot identify archive staging directory.");
      namespaceParent->Flush([&] {
        if (hooks.proceed && !hooks.proceed())
          throw Cancelled();
      });
      recovery->Advance(RecoveryPhase::Writing, &identity);
      RegisterArchiveStage(path, identity.st_dev, identity.st_ino);
    } catch (const RemovableCopyInterrupted &) {
      throw; // Do not reenter the suspect mount while a flush is still pending.
    } catch (...) {
      node_ref named;
      if (created.GetNodeRef(&named) == B_OK &&
          named.device == identity.device && named.node == identity.node &&
          created.Remove() == B_OK) {
        try {
          namespaceParent->FlushAfterCleanup();
          recovery->Retire();
        } catch (const std::exception &) {
          // Keep the durable recovery record if the removal is uncertain.
        }
      }
      throw;
    }
  }
  void CheckInterrupted(const Result &copy) {
    if (copy.interruptedCopyFolder.empty())
      return;
    retainRecovery = true;
    throw RemovableCopyInterrupted(copy.errors.empty()
        ? "Archive transfer interrupted on a removable disk." : copy.errors.front(),
        copy.interruptedCopyFolder);
  }
  ~Stage() {
    try {
      ForgetArchiveStage(path);
      if (retainRecovery) return;
      if (published) {
        recovery->Retire();
        return;
      }
      BEntry entry(&parent, leaf.c_str(), false);
      node_ref named;
      if (entry.GetNodeRef(&named) == B_OK &&
          named.device == identity.device && named.node == identity.node &&
          RemoveStageContents(dir, identity.device) &&
          entry.GetNodeRef(&named) == B_OK &&
          named.device == identity.device && named.node == identity.node &&
          entry.Remove() == B_OK) {
        namespaceParent->FlushAfterCleanup();
        recovery->Retire();
        return;
      }
      result.errors.push_back("Could not clean the archive staging folder; inspect " +
                              path + " (show hidden files).");
    } catch (const std::exception &error) {
      try {
        if (auto interrupted = dynamic_cast<const RemovableCopyInterrupted *>(&error))
          result.interruptedCopyFolder = interrupted->folder.empty()
              ? ParentLocation(path) : interrupted->folder;
        result.errors.push_back(error.what());
      } catch (...) {
        // Do not let diagnostic allocation failures mask the original error.
      }
    }
  }
};
// The outer job owns the user-facing report. Inner copies must return failures
// directly to the caller that decides whether a snapshot is safe to publish.
Hooks InnerCopyHooks(const Hooks &hooks, bool privateDestination) {
  auto inner = hooks;
  inner.privateStaging = privateDestination;
  inner.report.reset();
  inner.fileDone = {};
  inner.fileFailed = {};
  return inner;
}

void PublishNewArchive(Stage &stage, const Job &job, BEntry &published,
                       node_ref &identity, const struct stat &written,
                       const std::function<void()> &check) {
  PinnedDirectory stagedFolder(stage.path, stage.identity.device, stage.identity.node);
  try {
    stagedFolder.Flush(check);
  } catch (const RemovableCopyInterrupted &) {
    stage.retainRecovery = true;
    throw;
  }
  const auto parent = stage.namespaceParent->Descriptor();
  FD source(openat(stagedFolder.Descriptor(), "archive", O_RDONLY | O_NOFOLLOW));
  struct stat staged;
  Require(source.n >= 0 && !fstat(source.n, &staged) && Unchanged(written, staged),
          "Archive output changed before publication.");
  Require(StagingSpaceAvailable(parent, staged.st_size),
          "Not enough confirmed space to publish the new archive.");
  check();
  stage.namespaceParent->Verify();
  FD out(openat(parent, job.entryName.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                O_NOFOLLOW | O_CLOEXEC, 0600));
  Require(out.n >= 0, "Cannot reserve the new archive name. No file was replaced.");
  // The name now exists. Never unlink an uncertain destination or discard the
  // completed stage if a write, device check or directory flush fails.
  stage.retainRecovery = true;
  struct stat reserved;
  Require(!fstat(out.n, &reserved), "Cannot identify the new file. Check the destination and recovery stage.");
  try {
    if (job.archiveFormat == "iso")
      CopyDataIsoSeal(source.n, out.n);
    WriteRemovableStagingFile(std::exchange(source.n, -1), std::exchange(out.n, -1),
                              staged, [&](size_t) { check(); }, true,
                              StagingPermissions::KeepCreationMode);
  } catch (const RemovableCopyInterrupted &) {
    throw RemovableCopyInterrupted("Archive publication interrupted; the new file may be incomplete. "
                                   "The archive stage was retained.", job.destinationFolder);
  } catch (const Cancelled &) {
    stage.result.errors.push_back("Archive publication cancelled; check the incomplete destination and retained stage.");
    throw;
  } catch (const std::exception &error) {
    throw std::runtime_error(std::string("Archive publication incomplete; check the new file and retained stage: ") + error.what());
  }
  struct stat final;
  Require(!fstatat(parent, job.entryName.c_str(), &final, AT_SYMLINK_NOFOLLOW) &&
              final.st_dev == reserved.st_dev && final.st_ino == reserved.st_ino &&
              final.st_size == staged.st_size,
          "The new archive changed during publication. Check the destination and retained stage.");
  identity.device = reserved.st_dev;
  identity.node = reserved.st_ino;
  Require(published.SetTo(&stage.parent, job.entryName.c_str(), false) == B_OK,
          "Cannot locate the new archive. The recovery stage was retained.");
}
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
void CopyMetadata(const std::string &fromPath, int output,
                  const Hooks &hooks) {
  struct stat st;
  BNode from(fromPath.c_str());
  Require(from.GetStat(&st) == B_OK, "Cannot inspect archive metadata.");
  Require(!fchmod(output, st.st_mode & 07777),
          "Cannot preserve archive permissions.");
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
      RestoreEmptyAttribute(output, name, info.type);
      continue;
    }
    off_t at = 0;
    while (at < info.size) {
      auto amount = std::min<off_t>(bytes.size(), info.size - at);
      auto got = from.ReadAttr(name, info.type, at, bytes.data(), amount);
      Require(got > 0, "Cannot read archive metadata.");
      Require(fs_write_attr(output, name, info.type, at, bytes.data(), got) == got,
              "Cannot preserve archive metadata.");
      at += got;
    }
  }
  Require(status == B_ENTRY_NOT_FOUND, "Cannot enumerate archive metadata.");
}
// Apply the same subtree removal or rename to the original member headers.
// Extraction identities belong to our scratch directory, not the TAR users.
void ChangeTarMetadataPaths(TarMemberMap &metadata, const std::string &from,
                         const std::string &to = {}) {
  auto change = [&](TarMemberMap::iterator it) {
    auto member = metadata.extract(it);
    if (!to.empty()) {
      member.key().replace(0, from.size(), to);
      Require(metadata.insert(std::move(member)).inserted,
              "Conflicting TAR ownership after rename. Original archive retained.");
    }
  };
  auto exact = metadata.find(from);
  if (exact != metadata.end())
    change(exact);
  const auto prefix = from + "/";
  auto it = metadata.lower_bound(prefix);
  while (it != metadata.end() && it->first.compare(0, prefix.size(), prefix) == 0)
    change(it++);
}
// A TAR move crosses a private filesystem snapshot, whose ownership is not
// the source metadata. Map each selected header into the final member namespace.
TarMemberMap MovedTarMetadata(const Job &remove, const std::string &targetFolder, const Hooks &hooks) {
  TarMemberMap moved;
  if (!IsTarFormat(remove.archiveFormat))
    return moved;
  TarMemberMap source;
  Require(DetectArchiveEditFormat(remove.destination, hooks, &source) == remove.archiveFormat,
          "The source TAR format changed. Source entries were retained.");
  ArchiveMetadataBudget budget;
  for (const auto &from : remove.archiveRemove) {
    const auto leaf = Leaf(from);
    const auto to = targetFolder.empty() ? leaf : Join(targetFolder, leaf);
    auto transfer = [&](TarMemberMap::iterator it) {
      auto entry = source.extract(it);
      budget.Charge(sizeof(TarMemberMap::value_type) + ArchiveMetadataBudget::NodeOverhead +
                    to.size() + entry.key().size() - from.size() +
                    entry.mapped().user.size() + entry.mapped().group.size());
      entry.key().replace(0, from.size(), to);
      Require(moved.insert(std::move(entry)).inserted, "Conflicting TAR move destinations.");
    };
    auto exact = source.find(from);
    const auto prefix = from + "/";
    auto child = source.lower_bound(prefix);
    Require(exact != source.end() ||
                (child != source.end() && child->first.compare(0, prefix.size(), prefix) == 0),
            "No TAR ownership information for " + from + ". Source entries were retained.");
    if (exact != source.end())
      transfer(exact);
    while (child != source.end() && child->first.compare(0, prefix.size(), prefix) == 0)
      transfer(child++);
  }
  return moved;
}

// A local extraction deliberately does not apply privileged permissions. A non-TAR
// writer has no verified ownership contract. Reject unrepresentable selections
// before publishing anything, and check the actual destination before cleanup.
void RequirePortableTarMove(const TarMemberMap &members, bool toLocal) {
  for (const auto &[name, member] : members) {
    Require(!member.accessed && !member.changed && !member.created,
            "Cannot preserve additional TAR timestamps for " + name + ". Use Copy; source retained.");
    Require(!(member.mode & 07000) && member.user.empty() && member.group.empty() &&
                (toLocal || (member.uid == 0 && member.gid == 0)),
            "Cannot preserve selected permissions or ownership for " + name + ". Use Copy; source retained.");
  }
}
// Only local sources use the documented non-TAR timestamp precision.
void VerifyPortableMove(const TarMemberMap &members, const std::string &target,
                        const TransferControl &control, const std::string &localToFormat = {},
                        Result *receipt = nullptr) {
  auto location = ParseLocation(target);
  const bool dosTime = localToFormat == "arj" || localToFormat == "cab";
  bool reducedTime = false;
  for (const auto &[name, member] : members) {
    control.Check();
    bool matches = false;
    if (location.Local()) {
      struct stat st;
      matches = !lstat(Join(target, name).c_str(), &st) &&
                st.st_uid == member.uid && st.st_gid == member.gid &&
                (st.st_mode & 07777) == member.mode &&
                st.st_mtim.tv_sec == member.modified && st.st_mtim.tv_nsec == member.nanoseconds;
    } else {
      location.path = "/" + name;
      FileEntry entry;
      const bool exists = StatFile(location.URL(), entry, control);
      Require(exists && !entry.link && entry.directory == member.directory,
              "Destination member type differs for " + name + ". Source entries were retained.");
      Require(entry.permissions == member.mode,
              "Destination permissions differ for " + name + ". Source entries were retained.");
      matches = entry.modifiedKnown && entry.modified == member.modified &&
                entry.ModifiedNanoseconds() == member.nanoseconds;
      if (!matches && !localToFormat.empty()) {
        const auto expectedSeconds = dosTime ? member.modified - (member.modified % 2) : member.modified;
        // No timestamp field for CAB's implicit directories. DOS writers store
        // even seconds; other archive writers must at least reproduce the second.
        matches = (entry.modifiedKnown && entry.modified == expectedSeconds) ||
                  (localToFormat == "cab" && member.directory && !entry.modifiedKnown);
        reducedTime |= matches;
      }
    }
    Require(matches, "Destination timestamp or ownership differs for " + name +
                         ". Source entries were retained.");
  }
  if (reducedTime && receipt) {
    receipt->copyLimitations = true;
    receipt->outcomes.push_back({{}, target, "archive timestamp precision",
                                localToFormat == "cab" ? "CAB keeps file times to two seconds and stores no folder times." :
                                dosTime ? "ARJ keeps modification times to two seconds." :
                                "Non-TAR modification times verified to the second; finer precision was not retained."});
  }
}

void VerifyTarMetadata(const std::string &path, const std::string &format,
                     const TarMemberMap &expected, const Hooks &hooks) {
  if (expected.empty())
    return;
  TarMemberMap actual;
  Require(DetectArchiveEditFormat(path, hooks, &actual) == format,
          "Updated TAR format changed. Source entries were retained.");
  for (const auto &[name, member] : expected) {
    const auto found = actual.find(name);
    Require(found != actual.end() && found->second == member,
            "Updated TAR metadata or ownership differs for " + name + ". Source entries were retained.");
  }
}
void VerifyMovedEntry(const std::string &, const std::string &,
                      const TransferControl &, bool exactTree = false, unsigned depth = 0);
Result RunArchiveEdit(const Job &job, const Hooks &hooks,
                      const std::function<void()> &beforePublish = {},
                      const TarMemberMap &incomingMetadata = {}) {
  Result result;
  try {
    Require(!job.destination.empty() && !job.destinationFolder.empty(),
            "Archive edit target is missing.");
    Require(!job.sources.empty() || !job.archiveRemove.empty() ||
                !job.archiveFolders.empty() || !job.archiveRenames.empty(),
            "Choose an archive change first.");
    Guards(job);
    VerifyArchiveSets(job);
    TarMemberMap metadata;
    const auto format = DetectArchiveEditFormat(job.destination, hooks, &metadata);
    Require(format == job.archiveFormat,
            "Archive format changed after selection.");
    for (const auto &[name, member] : incomingMetadata) {
      const auto existing = metadata.find(name);
      Require(existing == metadata.end() || !member.directory || !existing->second.directory ||
                  existing->second == member,
              "Cannot merge directories with conflicting TAR metadata: " + name +
                  ". Both original entries were retained.");
      metadata.insert_or_assign(name, member);
    }
    const auto passwordGeneration = ArchivePasswordGeneration();
    const auto originalSet = FindArchiveVolumes(job.destination);
    ArchiveEditProtection protection(originalSet.files.front().identity,
                                     originalSet.Fingerprint());
    Stage stage(job, result, hooks);
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
    auto internal = InnerCopyHooks(hooks, true);
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
      stage.CheckInterrupted(copied);
      if (copied.cancelled)
        throw Cancelled();
      Require(copied.errors.empty(), copied.errors.empty()
                                         ? "Archive extraction failed."
                                         : copied.errors.front());
      Require(!copied.skipped && copied.completed == int(extract.sources.size()),
              "Archive extraction was incomplete. The original archive was retained.");
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
      ChangeTarMetadataPaths(metadata, relative);
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
      ChangeTarMetadataPaths(metadata, from, to);
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
      if (!metadata.empty()) {
        adding.fileDone = [&](const std::string &, const std::string &destination) {
          const auto name = destination.substr(work.size() + 1);
          auto replaced = metadata.find(name);
          if (replaced == metadata.end() || replaced->second.directory || incomingMetadata.count(name))
            return;
          // A replacement is not the old member. In particular, never attach
          // its set-id bits to different bytes.
          struct stat st;
          Require(!lstat(destination.c_str(), &st) && S_ISREG(st.st_mode),
                  "Cannot verify replacement TAR member: " + name);
          replaced->second.accessed.reset();
          replaced->second.changed.reset();
          replaced->second.created.reset();
          replaced->second.mode = st.st_mode & 0777;
          replaced->second.modified = st.st_mtim.tv_sec;
          replaced->second.nanoseconds = st.st_mtim.tv_nsec;
        };
      }
      auto added = Run(add, adding);
      stage.CheckInterrupted(added);
      if (added.cancelled)
        throw Cancelled();
      Require(added.errors.empty(), added.errors.empty()
                                        ? "Could not add files to archive."
                                        : added.errors.front());
      Require(!added.skipped && added.completed == int(add.sources.size()),
              "Archive additions were incomplete. The original archive was retained.");
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
                        secret && format == "7z", secret, internal, metadata);
    Require(!fsync(output.n), "Cannot flush updated archive to disk.");
    CopyMetadata(job.destination, output.n, internal);
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
      VerifyTarMetadata(outputPath, format, metadata, internal);
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
    stage.recovery->Advance(RecoveryPhase::Ready);
    stage.recovery->Advance(RecoveryPhase::Publishing);
    if (beforePublish)
      beforePublish();
    // Journal writes and callbacks can take time. Never adopt a fresh baseline
    // here: the output was built from the revision captured before extraction.
    check();
    originalSet.Verify();
    FD parent(stage.parent.Dup()), stageDir(stage.dir.Dup());
    Require(parent.n >= 0 && stageDir.n >= 0,
            "Cannot open the archive directory before publishing. Original archive was retained.");
    const auto targetLeaf = Leaf(job.destination);
    FD originalFd(openat(parent.n, targetLeaf.c_str(),
                         O_RDONLY | O_NOFOLLOW | O_NONBLOCK));
    struct stat original, named;
    Require(originalFd.n >= 0 && !fstat(originalFd.n, &original) &&
                S_ISREG(original.st_mode) &&
                ArchivePasswordIdentity(original) == originalSet.files.front().identity,
            "The archive changed while it was being edited. Nothing was changed.");
    Require(!fstatat(stageDir.n, "updated", &named, AT_SYMLINK_NOFOLLOW) &&
                Unchanged(verifiedOutput, named),
            "The verified output changed. Original archive was retained.");
    // Haiku has no revision-conditional rename. Check the target name against
    // the validated descriptor immediately before replacing it in that parent.
    Require(!fstatat(parent.n, targetLeaf.c_str(), &named, AT_SYMLINK_NOFOLLOW) &&
                Unchanged(original, named),
            "The archive changed while it was being edited. Nothing was changed.");
    Require(
        !renameat(stageDir.n, "updated", parent.n, targetLeaf.c_str()),
        "Could not publish the updated archive. The original was left in place.");
    stage.retainRecovery = true;
    // Rename may update ctime. Capture the new version from our still-open
    // verified output, never from whatever now occupies the destination path.
    struct stat published;
    Require(!fstat(output.n, &published) && published.st_size == verifiedOutput.st_size &&
                published.st_mtim.tv_sec == verifiedOutput.st_mtim.tv_sec &&
                published.st_mtim.tv_nsec == verifiedOutput.st_mtim.tv_nsec,
            "Archive was published, but its verified contents could not be confirmed.");
    const auto identity = ArchivePasswordIdentity(published);
    ArchiveVolumes receipt;
    receipt.files.push_back({job.destination, identity, published.st_size,
                             published.st_dev, published.st_ino});
    result.publishedArchiveVersion = receipt.Fingerprint();
    Require(!fsync(parent.n),
            "Updated archive was published, but its directory could not be flushed. Durability after restart is unknown.");
    stage.recovery->Advance(RecoveryPhase::Published);
    stage.retainRecovery = false;
    if (secret)
      try {
        RememberArchivePassword(identity, secret, passwordGeneration);
        RememberArchivePassword(result.publishedArchiveVersion, secret,
                                passwordGeneration);
      } catch (
          ...) { /* Password caching is optional after the archive is durable. */
      }
    result.completed = 1;
    result.completedRoots.push_back({job.destination, job.destination});
    if (hooks.fileDone)
      hooks.fileDone(job.destination, job.destination);
  } catch (const RemovableCopyInterrupted &error) {
    result.interruptedCopyFolder = error.folder.empty() ? job.destinationFolder : error.folder;
    result.errors.push_back(error.what());
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
    auto internal = InnerCopyHooks(hooks, true);
    internal.proceed = [&] {
      if (hooks.proceed && !hooks.proceed())
        throw Cancelled();
      Guards(job);
      VerifyArchiveSets(job);
      return true;
    };
    TarMemberMap fromTar;
    if (target.Local()) {
      const auto removal = PrepareArchiveEditJob(source.archive, {}, job.sources);
      fromTar = MovedTarMetadata(removal, "", internal);
      RequirePortableTarMove(fromTar, true);
    }
    std::unique_ptr<LocalMoveTree> localSource;
    if (source.Local())
      localSource = std::make_unique<LocalMoveTree>(
          job.sources, internal, true, job.expectedDestination.device);
    Stage stage(job, result, hooks);
    const auto snapshot = PathOf(stage.dir);
    Job extract{Action::Copy, job.sources, snapshot};
    FreezeLocalJob(extract, snapshot);
    auto extracted = Run(extract, internal);
    stage.CheckInterrupted(extracted);
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
    TarMemberMap movedMetadata;
    std::unique_ptr<LocalMoveTree> localDestination;
    if (target.Archive()) {
      auto add =
          PrepareArchiveEditJob(target.archive, additions, {}, target.path);
      movedMetadata = localSource->TarMetadata(add.archivePath);
      const bool tarTarget = IsTarFormat(add.archiveFormat);
      if (!tarTarget)
        RequirePortableTarMove(movedMetadata, false);
      auto written = tarTarget ? RunArchiveEdit(add, internal, {}, movedMetadata)
                               : RunArchiveEdit(add, internal);
      stage.CheckInterrupted(written);
      if (written.cancelled)
        throw Cancelled();
      Require(written.errors.empty() && written.completed == 1,
              written.errors.empty() ? "Destination archive update failed."
                                     : written.errors.front());
      copied = true;
      published = PublishedArchiveGuard(add, written.publishedArchiveVersion);
    } else {
      // Local directory merges may publish several files. A skipped or failed
      // file retains every source member; never treat a partial copy as a move.
      Job copy{Action::Copy, additions, job.destination};
      copy.copyForArchiveMove = true;
      FreezeLocalJob(copy, job.destination);
      copied = true;
      auto written = Run(copy, InnerCopyHooks(internal, false));
      stage.CheckInterrupted(written);
      if (written.cancelled)
        throw Cancelled();
      Require(written.errors.empty() && !written.skipped &&
                  written.completed == int(additions.size()),
              written.errors.empty()
                  ? "Incomplete destination copy. Source archive retained."
                  : written.errors.front());
      localDestination = std::make_unique<LocalMoveTree>(
          destinations, internal, false, job.expectedDestination.device);
      // The local copy already flushed files and finalized the directory
      // tree in post-order before reporting success. This snapshot guards
      // the verified bytes through source cleanup.
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
    if (!target.Archive())
      VerifyPortableMove(fromTar, job.destination, verify);
    else if (IsTarFormat(published.archiveFormat))
      VerifyTarMetadata(target.archive, published.archiveFormat, movedMetadata, internal);
    else
      VerifyPortableMove(movedMetadata, job.destination, verify, published.archiveFormat, &result);
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
      stage.CheckInterrupted(removed);
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
  } catch (const RemovableCopyInterrupted &error) {
    result.interruptedCopyFolder = error.folder.empty() ? job.destinationFolder : error.folder;
    result.errors.push_back(error.what());
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
    auto internal = InnerCopyHooks(hooks, true);
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
      Stage stage(job, result, hooks);
      const auto snapshot = PathOf(stage.dir);
      Job extract{Action::Copy, job.sources, snapshot};
      FreezeLocalJob(extract, snapshot);
      auto extracted = Run(extract, internal);
      stage.CheckInterrupted(extracted);
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
      const auto movedMetadata = MovedTarMetadata(sourceGuard, add.archivePath, internal);
      const bool tarTarget = IsTarFormat(add.archiveFormat);
      if (!tarTarget)
        RequirePortableTarMove(movedMetadata, false);
      auto written = tarTarget ? RunArchiveEdit(add, internal, {}, movedMetadata)
                               : RunArchiveEdit(add, internal);
      stage.CheckInterrupted(written);
      if (written.cancelled)
        throw Cancelled();
      Require(written.errors.empty() && written.completed == 1,
              written.errors.empty() ? "Destination archive update failed."
                                     : written.errors.front());
      copied = true;
      // Destination is now committed and fsynced. Never roll it back to stale
      // bytes if source cleanup fails; retain both copies and report the failure.
      auto published = PublishedArchiveGuard(add, written.publishedArchiveVersion);
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
      if (tarTarget)
        VerifyTarMetadata(target.archive, add.archiveFormat, movedMetadata, internal);
      else
        VerifyPortableMove(movedMetadata, job.destination, verify);
      for (const auto &path : job.sources)
        VerifyMovedEntry(Join(snapshot, LocationLeaf(path)),
                         ChildLocation(job.destination, LocationLeaf(path)),
                         verify);
      checkSource();
      // Rebuild the source only while the verified destination identity and
      // contents remain frozen, including the final pre-rename guard.
      result = RunArchiveEdit(sourceGuard, internal);
      if (!result.interruptedCopyFolder.empty())
        stage.retainRecovery = true;
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
  } catch (const RemovableCopyInterrupted &error) {
    result.interruptedCopyFolder = error.folder.empty() ? job.destinationFolder : error.folder;
    result.errors.push_back(error.what());
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
  std::string lastProgressPath;
  Hooks tracked = hooks;
  if (hooks.fileDone)
    tracked.progress = [&](const std::string &path, uint64_t bytes) {
      if (completedPaths.empty() || path != lastProgressPath) {
        completedPaths.insert(path);
        lastProgressPath = path;
      }
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
    std::string resolvedFolder;
    for (const auto &source : job.sources) {
      if (IsVirtual(source))
        continue;
      if (resolvedFolder.empty()) {
        std::unique_ptr<char, decltype(&free)> destination(
            realpath(job.destinationFolder.c_str(), nullptr), free);
        Require(bool(destination), "Cannot resolve archive paths.");
        resolvedFolder = destination.get();
      }
      std::unique_ptr<char, decltype(&free)> resolvedSource(
          realpath(source.c_str(), nullptr), free);
      Require(bool(resolvedSource), "Cannot resolve archive paths.");
      const std::string from = resolvedSource.get();
      Require(resolvedFolder != from &&
                  resolvedFolder.rfind(Join(from, ""), 0) != 0,
              "Choose a destination outside the selected folder.");
    }
    Stage stage(job, result, hooks);
    Guards(job);
    auto check = [&] {
      if (hooks.proceed && !hooks.proceed())
        throw Cancelled();
      Guards(job);
    };
    check();
    BEntry publish;
    node_ref publishIdentity;
    struct stat writtenArchive;
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
      Require(!fstat(output.n, &writtenArchive) &&
                  publish.SetTo(&stage.dir, "archive", false) == B_OK,
              "Archive output disappeared.");
      publishIdentity.device = writtenArchive.st_dev;
      publishIdentity.node = writtenArchive.st_ino;
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
      auto guarded = InnerCopyHooks(tracked, true);
      // Defer all member completion notifications until atomic publication.
      // Otherwise the UI leaves successfully extracted members marked failed.
      if (hooks.fileDone)
        guarded.fileDone = [&](const std::string &path, const std::string &) {
          completedPaths.insert(path);
        };
      guarded.proceed = [&] {
        check();
        return true;
      };
      auto copied = Run(copy, guarded);
      stage.CheckInterrupted(copied);
      if (copied.cancelled)
        throw Cancelled();
      Require(copied.errors.empty(),
              copied.errors.empty() ? "" : copied.errors.front());
      Require(
          !copied.skipped && copied.completed == int(copy.sources.size()),
          "Archive contents were not fully copied; extraction was not published.");
      for (const auto &item : original) {
        struct stat s;
        Require(!lstat(item.first.c_str(), &s) && Unchanged(item.second, s),
                "Archive changed during extraction.");
      }
      // Member directories were finalized by Run before reporting success.
      // Cover the staging root itself, including a completely empty archive.
      PinnedDirectory directory(stage.path, stage.identity.device,
                                stage.identity.node);
      try {
        directory.Flush(check);
      } catch (const RemovableCopyInterrupted &) {
        stage.retainRecovery = true;
        throw;
      }
      Require(stage.dir.GetEntry(&publish) == B_OK,
              "Extraction staging folder disappeared.");
      publishIdentity = stage.identity;
    }
    check();
    stage.recovery->Advance(RecoveryPhase::Ready);
    stage.recovery->Advance(RecoveryPhase::Publishing);
    Guards(job);
    VerifyArchiveSets(job);
    stage.namespaceParent->Verify();
    auto verifyOutput = [&] {
      node_ref current;
      Require(publish.GetNodeRef(&current) == B_OK && current == publishIdentity,
              "Archive job output was replaced.");
    };
    verifyOutput();
    if (job.action == Action::CreateArchive) {
      PublishNewArchive(stage, job, publish, publishIdentity, writtenArchive,
                        check);
    } else {
      // Haiku checks a directory's destination separately from its rename.
      Require(publish.MoveTo(&stage.parent, job.entryName.c_str(), false) == B_OK,
              "Could not publish the extracted folder. Check the destination.");
      stage.published = true;
    }
    stage.retainRecovery = true;
    try {
      stage.namespaceParent->Flush(check);
      verifyOutput();
    } catch (const Cancelled &) {
      result.errors.push_back("Result published, but directory durability after restart is unknown.");
      throw;
    } catch (const RemovableCopyInterrupted &error) {
      throw RemovableCopyInterrupted(std::string("Result published: ") + error.what(),
                                     error.folder);
    } catch (const std::runtime_error &error) {
      throw std::runtime_error(std::string("Result published: ") + error.what());
    }
    stage.recovery->Advance(RecoveryPhase::Published);
    stage.retainRecovery = false;
    result.completed = 1;
    if (hooks.fileDone) {
      completedPaths.insert(job.sources.begin(), job.sources.end());
      for (const auto &source : completedPaths)
        hooks.fileDone(source, job.destination);
    }
  } catch (const RemovableCopyInterrupted &error) {
    result.interruptedCopyFolder = error.folder.empty() ? job.destinationFolder : error.folder;
    result.errors.push_back(error.what());
  } catch (const Cancelled &) {
    result.cancelled = true;
  } catch (const std::exception &e) {
    result.errors.push_back(e.what());
  }
  return result;
}
}
