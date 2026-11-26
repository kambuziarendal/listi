// SPDX-License-Identifier: MIT
#include "FileOperation.h"
#include "UnavailableVolume.h"
#include "JobReportStore.h"
#include "BulkRename.h"
#include "DirectoryFinalization.h"
#include "ArchiveVolumes.h"
#include "ArchiveJobs.h"
#include "ImageConvert.h"
#include "NamedEntry.h"
#include "JobSnapshot.h"
#include "MoveVolume.h"
#include "RemovableCopy.h"
#include "StagingSpace.h"
#include "RecoveryInventory.h"
#include "Trash.h"
#include "VirtualFile.h"
#include <Directory.h>
#include <Entry.h>
#include <Node.h>
#include <Path.h>
#include <Volume.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fs_attr.h>
#include <fs_info.h>
#include <limits.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <set>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace Listi {
Result RunVirtual(const Job &, const Hooks &);

std::string Join(const std::string &a, const std::string &b) {
  return a + (a.empty() || a.back() == '/' ? "" : "/") + b;
}
std::string Leaf(const std::string &p) {
  auto n = p.find_last_of('/');
  return n == std::string::npos ? p : p.substr(n + 1);
}
namespace {
struct FD {
  int n;
  explicit FD(int n) : n(n) {}
  ~FD() {
    if (n >= 0)
      close(n);
  }
};
using Dir = std::unique_ptr<DIR, decltype(&closedir)>;
std::string Canonical(const std::string &p) {
  char out[PATH_MAX];
  if (!realpath(p.c_str(), out))
    throw std::runtime_error(p + ": " + strerror(errno));
  return out;
}
class FilesystemError : public std::runtime_error {
public:
  FilesystemError(const std::string &path, int error)
      : std::runtime_error(path + ": " + strerror(error)), path(path), error(error) {}
  const std::string path;
  const int error;
};
void Fail(const std::string &p) {
  throw FilesystemError(p, errno);
}
dirent *NextEntry(DIR *directory, const std::string &path) {
  errno = 0;
  auto entry = readdir(directory);
  if (!entry && errno)
    Fail(path);
  return entry;
}
bool Exists(const std::string &p, struct stat *st = nullptr) {
  struct stat local;
  if (lstat(p.c_str(), st ? st : &local) == 0)
    return true;
  if (errno == ENOENT)
    return false;
  Fail(p);
  return false;
}
struct Runner {
  const Hooks &h;
  bool replaceAll = false, skipAll = false;
  Result result;
  const Job *guardedJob = nullptr;
  std::set<dev_t> approvedFatVolumes;
  std::string guardedSource;
  std::map<std::string, size_t> outcomeIndex;
  struct BackingDevice {
    std::string path, mount, folder;
    NodeIdentity node;
    bool removable = false, ntfs = false, fat = false, attributes = false;
    bool writableExt = false;
  };
  std::map<dev_t, BackingDevice> backingDevices;
  std::string interruptedFolder;
  void CheckDevice(dev_t volume, const std::string &folder) {
    auto it = backingDevices.find(volume);
    if (it == backingDevices.end()) {
      fs_info info{};
      if (fs_stat_dev(volume, &info))
        throw std::runtime_error(folder + ": volume is no longer available.");
      BackingDevice backing;
      backing.folder = folder;
      backing.ntfs = strcasecmp(info.fsh_name, "ntfs") == 0;
      backing.fat = strcasecmp(info.fsh_name, "fat") == 0;
      backing.attributes = (info.flags & B_FS_HAS_ATTR) != 0;
      backing.writableExt = !(info.flags & B_FS_IS_READONLY) &&
          (!strcasecmp(info.fsh_name, "ext2") ||
           !strcasecmp(info.fsh_name, "ext3") ||
           !strcasecmp(info.fsh_name, "ext4"));
      if (strncmp(info.device_name, "/dev/disk/", 10) == 0) {
        backing.path = info.device_name;
        backing.removable = (info.flags & B_FS_IS_REMOVABLE) ||
                            backing.path.rfind("/dev/disk/usb/", 0) == 0;
      }
      if (backing.removable) {
        BVolume disk(volume);
        BDirectory root;
        BEntry entry;
        BPath path;
        if (disk.GetRootDirectory(&root) == B_OK &&
            root.GetEntry(&entry) == B_OK && entry.GetPath(&path) == B_OK)
          backing.mount = path.Path();
      }
      it = backingDevices.emplace(volume, std::move(backing)).first;
    }
    auto &backing = it->second;
    if (backing.path.empty())
      return; // Not a disk-backed filesystem.
    // A disconnected BFS volume can still report successful stat and fsync
    // calls against cached files. Check the backing device in devfs instead.
    struct stat st;
    if (stat(backing.path.c_str(), &st) ||
        (backing.node.device >= 0 &&
         (backing.node.device != st.st_dev || backing.node.inode != st.st_ino))) {
      const auto message = folder +
          ": device disconnected or changed. Transfer not confirmed; "
          "check both volumes after reconnecting.";
      if (backing.removable) {
        interruptedFolder = backing.mount.empty() ? folder : backing.mount;
        InterruptedVolumes().Block(interruptedFolder, volume);
        throw RemovableCopyInterrupted(message);
      }
      throw std::runtime_error(message);
    }
    backing.node = {st.st_dev, st.st_ino};
  }
  void CheckRemovableFailure(const std::runtime_error &failure) {
    const auto *error = dynamic_cast<const FilesystemError *>(&failure);
    for (const auto &[volume, backing] : backingDevices) {
      if (!backing.removable)
        continue;
      const auto &folder = backing.mount.empty() ? backing.folder : backing.mount;
      if (error && IsRemovableIOError(error->error) &&
          (error->path == folder ||
           error->path.compare(0, folder.size() + 1, folder + "/") == 0)) {
        InterruptedVolumes().Block(folder, volume);
        throw RemovableCopyInterrupted(error->what(), folder);
      }
      // Some filesystem wrappers report an ordinary runtime_error. Check
      // devfs, not the mount that may still answer stale cached metadata.
      CheckDevice(volume, folder);
    }
  }
  void CheckDestinationDevice(dev_t volume) {
    CheckDevice(volume, guardedJob->destination);
  }
  bool PlainCopy() const {
    return guardedJob->action == Action::Copy && !deferCompletion &&
           !guardedJob->copyForArchiveMove && !h.privateStaging;
  }
  bool FatCopy(dev_t volume) const {
    return PlainCopy() && backingDevices.at(volume).fat;
  }
  bool NtfsCopy(dev_t volume) const {
    return PlainCopy() && backingDevices.at(volume).ntfs;
  }
  bool FlushDestination(const PinnedDirectory &directory, dev_t volume,
                        bool cleanup = false, bool allowUnsupportedNtfs = false) {
    const auto &backing = backingDevices.at(volume);
    const auto previous = interruptedFolder;
    if (backing.removable)
      interruptedFolder = backing.mount.empty() ? guardedJob->destination
                                                 : backing.mount;
    bool flushed = true;
    if (cleanup)
      directory.FlushAfterCleanup([&] { CheckDestinationDevice(volume); });
    else
      flushed = directory.Flush([&] {
        CheckDestinationDevice(volume);
        Check();
      }, allowUnsupportedNtfs);
    interruptedFolder = previous;
    return flushed;
  }
  void CheckSourceDevice(dev_t volume) {
    CheckDevice(volume, ParentLocation(guardedSource));
  }
  size_t mutations = 0;
  std::optional<EntryOutcome> pendingOutcome;
  void FlushOutcome() {
    if (pendingOutcome) {
      h.report->WriteOutcome(*pendingOutcome, true);
      pendingOutcome.reset();
    }
  }
  void AddError(const std::string &error) {
    if (h.report)
      h.report->AddError(error);
    else
      result.errors.push_back(error);
  }
  EntryOutcome &Outcome(const std::string &src, const std::string &dst = {}) {
    if (h.report) {
      if (!pendingOutcome || pendingOutcome->source != src) {
        FlushOutcome();
        pendingOutcome = h.report->FindOutcome(src);
      }
      if (!dst.empty())
        pendingOutcome->destination = dst;
      return *pendingOutcome;
    }
    auto index = outcomeIndex.emplace(src, result.outcomes.size());
    if (index.second)
      result.outcomes.push_back({src, dst, "unprocessed", {}});
    auto &row = result.outcomes[index.first->second];
    if (!dst.empty())
      row.destination = dst;
    return row;
  }
  void Problem(const std::string &src, const std::string &error) {
    auto &row = Outcome(src);
    if (row.status == "unprocessed")
      row.status = "failed";
    row.detail = error;
  }
  void Removed(const std::string &src) {
    Outcome(src).status = deferCompletion ? "moved" : "completed";
    // A selected folder may still exist after some of its children have
    // been removed. Don't report that its original tree is intact.
    std::string parent = src;
    while (parent != guardedSource) {
      const auto slash = parent.rfind('/');
      if (slash == std::string::npos)
        break;
      parent.resize(slash);
      Outcome(parent).status = "partial";
    }
  }
  using DirectoryStack = std::vector<std::pair<std::string, struct stat>>;
  DirectoryStack copyDirectories;
  // The active stack protects ancestors; claims also survive returning from
  // a sibling or starting the next selected root.
  std::map<std::pair<dev_t, ino_t>, std::string> directoryClaims;
  void ClaimDirectory(const struct stat &node, const std::string &source,
                      const std::string &destination) {
    auto claim = directoryClaims.emplace(std::make_pair(node.st_dev, node.st_ino), source);
    if (!claim.second && claim.first->second != source)
      throw std::runtime_error(destination + ": already used by source folder " +
                               claim.first->second + "; not merged with " + source);
  }
  void GuardDirectories() const {
    for (const auto &[path, expected] : copyDirectories) {
      struct stat now;
      if (lstat(path.c_str(), &now) || !S_ISDIR(now.st_mode) ||
          now.st_dev != expected.st_dev || now.st_ino != expected.st_ino)
        throw std::runtime_error(path + ": destination folder changed during copy");
    }
  }
  void Guard() {
    GuardDirectories();
    if (!guardedJob)
      return;
    struct stat st;
    if (!guardedJob->expectedSources.empty()) {
      auto expected = guardedJob->expectedSources.find(guardedSource);
      if (expected == guardedJob->expectedSources.end() ||
          lstat(guardedSource.c_str(), &st) ||
          st.st_dev != expected->second.device ||
          st.st_ino != expected->second.inode)
        throw std::runtime_error(guardedSource +
                                 ": selected source changed; left untouched");
    }
    const auto &destination = guardedJob->expectedDestination;
    const auto &path = guardedJob->destinationFolder.empty()
                           ? guardedJob->destination
                           : guardedJob->destinationFolder;
    if (destination.device >= 0 &&
        (stat(path.c_str(), &st) || !S_ISDIR(st.st_mode) ||
         st.st_dev != destination.device || st.st_ino != destination.inode))
      throw std::runtime_error(
          "Selected destination changed; operation stopped.");
    if (guardedJob->checkDestinationWindow)
      guardedJob->checkDestinationWindow();
  }

  bool deferCompletion = false, recoverableMove = false;
  // A same-volume merge or replacement can end in unlink, unlike Copy or
  // a cross-volume move that keeps the intact original in its volume's Trash.
  bool UnlinksMoveSource() const { return deferCompletion && !recoverableMove; }
  static bool SameOwner(const struct stat &a, const struct stat &b) {
    return a.st_uid == b.st_uid && a.st_gid == b.st_gid;
  }
  void SetMoveOwner(int fd, const struct stat &source, const std::string &path) {
    if (!UnlinksMoveSource() && !(deferCompletion && S_ISDIR(source.st_mode)))
      return;
    struct stat current;
    if (fstat(fd, &current) ||
        (!SameOwner(source, current) && fchown(fd, source.st_uid, source.st_gid)))
      Fail(path + ": cannot preserve move ownership");
  }
  void VerifyMoveMetadata(const struct stat &source, const struct stat &target,
                          const std::string &path) {
    if (!UnlinksMoveSource() && !(deferCompletion && S_ISDIR(source.st_mode)))
      return;
    if (!SameOwner(source, target))
      throw std::runtime_error(path + ": move ownership differs; source retained");
    if ((source.st_mode ^ target.st_mode) & 07777) {
      char modes[64];
      snprintf(modes, sizeof(modes), "source %04o, destination %04o",
               unsigned(source.st_mode & 07777), unsigned(target.st_mode & 07777));
      throw std::runtime_error(path + ": move permissions differ (" + modes +
                               "); source retained");
    }
  }
  struct CopiedEntry {
    std::string destination;
    struct stat committed;
  };
  std::map<std::string, CopiedEntry> copied;
  bool SamePublishedMode(const struct stat &staged, const struct stat &published) const {
    // Haiku FAT derives execute bits from the filename. Renaming a regular
    // .tmp stage to .png changes 0755 to 0644 on the very same open inode.
    mode_t differing = staged.st_mode ^ published.st_mode;
    if (S_ISREG(staged.st_mode) && FatCopy(staged.st_dev))
      differing &= ~(S_IXUSR | S_IXGRP | S_IXOTH);
    return differing == 0;
  }
  void Done(const std::string &src, const std::string &dst,
            const struct stat &expected) {
    ++mutations;
    CheckDestinationDevice(expected.st_dev);
    struct stat committed;
    if (lstat(dst.c_str(), &committed) || expected.st_dev != committed.st_dev ||
        expected.st_ino != committed.st_ino || !SamePublishedMode(expected, committed) ||
        expected.st_size != committed.st_size ||
        expected.st_mtim.tv_sec != committed.st_mtim.tv_sec ||
        expected.st_mtim.tv_nsec != committed.st_mtim.tv_nsec)
      throw std::runtime_error(dst + ": committed destination moved or changed");
    Outcome(src, dst).status = deferCompletion ? "source-retained" : "copied";
    if (deferCompletion) {
      copied.emplace(src, CopiedEntry{dst, committed});
    } else if (h.fileDone)
      h.fileDone(src, dst);
  }
  void Check() {
    if (h.proceed && !h.proceed())
      throw Cancelled();
    GuardDirectories();
  }
  void Tick(const std::string &p, uint64_t n = 0) {
    Check();
    if (h.progress)
      h.progress(p, n);
  }
  bool NtfsAttributesUnavailable(int fd, dev_t volume, const std::string &path) {
    if (!NtfsCopy(volume) || backingDevices.at(volume).attributes)
      return false;
    // BNode collapses open_attr_dir failures into B_FILE_ERROR. Probe the
    // opened inode to distinguish missing support from a genuine failure.
    DIR *directory = fs_fopen_attr_dir(fd);
    if (!directory) {
      if (errno == B_UNSUPPORTED)
        return true;
      Fail(path);
    }
    if (fs_close_attr_dir(directory))
      Fail(path);
    return false;
  }
  void CopyNotice(const std::string &src, const char *notice) {
    auto &detail = Outcome(src).detail;
    if (!detail.empty())
      detail += " ";
    detail += notice;
    result.copyLimitations = true;
  }
  void Attributes(const std::string &src, const std::string &dst,
                  bool verifyMerge = false, int destinationFD = -1,
                  dev_t destinationVolume = -1, BNode *pinnedSource = nullptr) {
    const bool destinationAttributesUnavailable = destinationFD >= 0 &&
        destinationVolume >= 0 &&
        NtfsAttributesUnavailable(destinationFD, destinationVolume, dst);
    const bool fatDestination =
        destinationFD >= 0 && destinationVolume >= 0 && FatCopy(destinationVolume);
    const dev_t fatVolume = fatDestination ? destinationVolume : -1;
    const bool allowExtOmission = destinationVolume >= 0 && PlainCopy() &&
        backingDevices.at(destinationVolume).writableExt;
    BNode localSource, to;
    if (!pinnedSource)
      localSource.SetTo(src.c_str());
    BNode &from = pinnedSource ? *pinnedSource : localSource;
    if (destinationFD < 0)
      to.SetTo(dst.c_str());
    char name[B_ATTR_NAME_LENGTH];
    if (from.InitCheck() != B_OK || (destinationFD < 0 && to.InitCheck() != B_OK))
      throw std::runtime_error("Could not open copy metadata");
    status_t status;
    std::vector<char> buf(65536);
    std::vector<char> existing(verifyMerge ? buf.size() : 0);
    bool omitted = false;
    bool wroteAttribute = false;
    while ((status = from.GetNextAttrName(name)) == B_OK) {
      Check();
      if (fatVolume >= 0) {
        if (!approvedFatVolumes.count(fatVolume)) {
          if (!h.confirmFatCopy)
            throw std::runtime_error(src + ": FAT cannot preserve Haiku attributes.");
          if (!h.confirmFatCopy(src, guardedJob->destination))
            throw Cancelled();
          CheckDestinationDevice(fatVolume);
          Guard();
          approvedFatVolumes.insert(fatVolume);
        }
        omitted = true;
        continue;
      }
      if (destinationAttributesUnavailable) {
        omitted = true;
        continue;
      }
      attr_info info;
      if (from.GetAttrInfo(name, &info) != B_OK)
        throw std::runtime_error(src + ": attribute changed during copy");
      if (verifyMerge) {
        attr_info target;
        if (to.GetAttrInfo(name, &target) != B_OK ||
            target.type != info.type || target.size != info.size)
          throw std::runtime_error(dst + ": folder merge would lose source attribute " +
                                   name + "; source retained");
        if (info.size == 0)
          continue;
      } else if (info.size == 0 && !allowExtOmission) {
        throw std::runtime_error(
            src + ": zero-length attribute cannot be preserved: " + name);
      }
      off_t pos = 0;
      do {
        const ssize_t n = info.size == 0 ? 0 :
            from.ReadAttr(name, info.type, pos, buf.data(),
                          std::min<off_t>(buf.size(), info.size - pos));
        if (info.size != 0 && n <= 0)
          throw std::runtime_error(src + ": could not read attribute " + name);
        if (verifyMerge) {
          if (to.ReadAttr(name, info.type, pos, existing.data(), n) != n ||
              memcmp(existing.data(), buf.data(), n) != 0)
            throw std::runtime_error(dst + ": folder merge would change attribute " +
                                     name + "; source retained");
        } else {
          // A zero-byte write does not reliably create an attribute on
          // Haiku; only try it to detect the EXT driver's lack of support.
          const ssize_t written = destinationFD >= 0
              ? fs_write_attr(destinationFD, name, info.type, pos, buf.data(), n)
              : to.WriteAttr(name, info.type, pos, buf.data(), n);
          const auto writeError = destinationFD >= 0 ? errno : written;
          // The Haiku EXT driver exposes read-only extended attributes on
          // writable volumes. Only accept its specific create-attribute error,
          // and only before any attribute data was written.
          if (written < 0 && writeError == EROFS && allowExtOmission && !wroteAttribute) {
            CopyNotice(src, "Haiku cannot write attributes on this EXT destination; source attributes were not copied.");
            return;
          }
          if (written != n)
            throw std::runtime_error(dst + ": could not preserve attribute " + name +
                                     (written < 0 ? ": " + std::string(strerror(writeError))
                                                  : ": short write"));
          if (n == 0)
            throw std::runtime_error(
                src + ": zero-length attribute cannot be preserved: " + name);
          wroteAttribute = true;
        }
        pos += n;
      } while (pos < info.size);
    }
    if (status != B_ENTRY_NOT_FOUND)
      throw std::runtime_error(src + ": could not enumerate attributes");
    if (omitted)
      CopyNotice(src, fatVolume >= 0
          ? "Copied without Haiku attributes on FAT by request."
          : "Haiku cannot write attributes on this NTFS destination; source attributes were not copied.");
  }
  bool OrdinaryAttributes(const std::string &src, const std::string &dst,
                          int in, int out, dev_t sourceVolume, dev_t targetVolume) {
    if (NtfsAttributesUnavailable(in, sourceVolume, src)) {
      CopyNotice(src, "Haiku does not expose source attributes or Windows permissions on this NTFS driver; used destination creation permissions.");
      return false;
    }
    Attributes(src, dst, false, out, targetVolume);
    return true;
  }
  bool SetCopyMode(int fd, mode_t mode, dev_t volume, const std::string &path) {
    if (!fchmod(fd, mode & 07777))
      return true;
    if (errno == B_UNSUPPORTED && NtfsCopy(volume))
      return false;
    Fail(path);
    return false;
  }
  void PermissionNotice(const std::string &src) {
    CopyNotice(src, kNtfsPermissionNotice);
  }
  void DurabilityNotice(const std::string &src) {
    CopyNotice(src, kNtfsDurabilityNotice);
  }
  bool Allow(std::string &dst, const std::string &src, const struct stat &incoming,
             const struct stat &existing, bool &keepBoth) {
    if (guardedJob && guardedJob->action == Action::Duplicate)
      throw std::runtime_error(
          dst + ": already exists; duplicate does not overwrite");
    if (skipAll) {
      result.skipped++;
      return false;
    }
    if (replaceAll)
      return true;
    Decision d;
    if (h.copyConflict && !h.privateStaging && !guardedJob->copyForArchiveMove &&
        guardedJob->action == Action::Copy && S_ISREG(incoming.st_mode) &&
        S_ISREG(existing.st_mode)) {
      const auto leaf = Leaf(dst);
      const auto dot = leaf.find_last_of('.');
      const auto split = dot == 0 || dot == std::string::npos ? leaf.size() : dot;
      const auto prefix = Join(ParentLocation(dst), leaf.substr(0, split));
      std::string proposed;
      for (uint64_t number = 2;; ++number) {
        Check();
        proposed = prefix + " (" + std::to_string(number) + ")" + leaf.substr(split);
        struct stat candidate;
        if (lstat(proposed.c_str(), &candidate) != 0) {
          if (errno != ENOENT)
            proposed.clear();
          break;
        }
      }
      if (proposed.empty())
        d = h.conflict ? h.conflict(dst) : Decision::Abort;
      else
        d = h.copyConflict({src, dst, proposed, incoming.st_size, existing.st_size,
                            incoming.st_mtime, existing.st_mtime});
      if (d == Decision::KeepBoth) {
        if (proposed.empty())
          throw std::runtime_error(dst + ": no suitable new name");
        dst = proposed;
        keepBoth = true;
      }
    } else {
      d = h.conflict ? h.conflict(dst) : Decision::Abort;
      if (d == Decision::KeepBoth)
        throw std::runtime_error("Keep both is unavailable for this conflict");
    }
    if (d == Decision::Abort)
      throw Cancelled();
    if (d == Decision::SkipAll)
      skipAll = true;
    if (d == Decision::Skip || d == Decision::SkipAll) {
      result.skipped++;
      return false;
    }
    Guard();
    if (d == Decision::ReplaceAll)
      replaceAll = true;
    return true;
  }
  using Snapshot = std::map<std::string, struct stat>;
  void SnapshotTree(const std::string &path, Snapshot &entries, dev_t volume,
                    unsigned depth = 0) {
    Check();
    if (depth > 64 || entries.size() >= 200000)
      throw std::runtime_error("Folder tree exceeds the safe operation limit");
    struct stat st;
    if (lstat(path.c_str(), &st))
      Fail(path);
    if (st.st_dev != volume)
      throw std::runtime_error(path +
                               ": refusing to traverse another mounted volume");
    // Copy-and-cleanup cannot preserve links to the original inode. The
    // same-volume rename path does not need a snapshot and preserves links.
    if (guardedJob->action == Action::Move && !S_ISDIR(st.st_mode) && st.st_nlink > 1)
      throw std::runtime_error(path +
                               ": use Copy for hard-linked entries; source retained");
    entries.emplace(path, st);
    if (!S_ISDIR(st.st_mode))
      return;
    Dir dir(opendir(path.c_str()), closedir);
    if (!dir)
      Fail(path);
    while (auto e = NextEntry(dir.get(), path)) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
        continue;
      SnapshotTree(Join(path, e->d_name), entries, volume, depth + 1);
    }
  }
  // Find existing nested directory conflicts before any sibling is delivered.
  // CopyEntry checks again through pinned parents before using each merge.
  void CheckMoveMerges(const std::string &source, const std::string &destination,
                       const Snapshot &tree) {
    for (const auto &[path, selected] : tree) {
      if (!S_ISDIR(selected.st_mode))
        continue;
      Check();
      const auto target = destination + path.substr(source.size());
      struct stat existing;
      if (!Exists(target, &existing))
        continue;
      if (!S_ISDIR(existing.st_mode))
        throw std::runtime_error(target + ": destination is not a directory; not replaced");
      RequireSameMoveVolume(guardedJob->expectedDestination.device, existing.st_dev);
      try {
        VerifyMoveMetadata(selected, existing, target);
      } catch (const std::runtime_error &error) {
        Outcome(path, target);
        Problem(path, error.what());
        throw;
      }
    }
  }
  bool SameEntry(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
           a.st_mode == b.st_mode && SameOwner(a, b) && a.st_size == b.st_size &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
           a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
           a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
  }
  void VerifyCopy(const std::string &source, const struct stat &original) {
    if (!deferCompletion)
      return;
    const auto recorded = copied.find(source);
    if (recorded == copied.end())
      throw std::runtime_error(source + ": no committed copy; source retained");
    const auto &copy = recorded->second;
    CheckDestinationDevice(copy.committed.st_dev);
    if (!recoverableMove)
      RequireSameMoveVolume(original.st_dev, copy.committed.st_dev);
    struct stat now;
    if (lstat(copy.destination.c_str(), &now) || !SameEntry(copy.committed, now)) {
      auto &row = Outcome(source);
      row.status = "failed";
      row.detail = copy.destination + ": destination changed; source retained";
      const std::string error = row.detail;
      if (source != guardedSource)
        Outcome(guardedSource).status = "partial";
      throw std::runtime_error(error);
    }
    VerifyMoveMetadata(original, now, copy.destination);
  }
  void VerifyCopiedTree(const std::string &root, const Snapshot &before) {
    Snapshot after;
    SnapshotTree(root, after, before.at(root).st_dev);
    if (before.size() != after.size())
      throw std::runtime_error(
          root + ": source changed; copied data kept and source retained");
    for (auto &entry : before) {
      auto found = after.find(entry.first);
      VerifyCopy(entry.first, entry.second);
      if (found == after.end() || !SameEntry(entry.second, found->second))
        throw std::runtime_error(
            root + ": source changed; copied data kept and source retained");
    }
  }
  void RetainMovedSource(const std::string &source, const Snapshot &before) {
    const auto volume = before.at(source).st_dev;
    CheckSourceDevice(volume);
    CheckDestinationDevice(guardedJob->expectedDestination.device);
    Check();
    VerifyCopiedTree(source, before);
    Guard();
    CheckSourceDevice(volume);
    CheckDestinationDevice(guardedJob->expectedDestination.device);
    // A cached read/flush is not proof of durable destination data. Only
    // rename the intact original into its own volume's recoverable Trash.
    const auto trash =
        MoveToTrash(source, &guardedJob->expectedSources.at(source));
    for (const auto &entry : copied)
      Outcome(entry.first).status = "moved";
    Outcome(source).detail = "Original kept in Trash: " + trash;
    ++result.recoverableMoves;
  }
  void RemoveCopied(const std::string &root, const Snapshot &before,
                    bool *removedAny = nullptr) {
    VerifyCopiedTree(root, before);
    // Reverse path order visits children before parents. Never enumerate new
    // entries during deletion: rmdir fails safely if a new child appears after
    // the snapshot.
    for (auto it = before.rbegin(); it != before.rend(); ++it) {
      Check();
      Guard();
      const auto parentPath = ParentLocation(it->first);
      const auto leaf = Leaf(it->first);
      FD parent(open(parentPath.c_str(), O_RDONLY | O_DIRECTORY));
      struct stat now, parentNode;
      if (parent.n < 0 || fstat(parent.n, &parentNode) ||
          fstatat(parent.n, leaf.c_str(), &now, AT_SYMLINK_NOFOLLOW))
        Fail(it->first);
      const auto expectedParent = before.find(parentPath);
      if (expectedParent != before.end() &&
          (parentNode.st_dev != expectedParent->second.st_dev ||
           parentNode.st_ino != expectedParent->second.st_ino))
        throw std::runtime_error(parentPath + ": source folder changed during cleanup");
      VerifyCopy(it->first, it->second);
      bool directory = S_ISDIR(it->second.st_mode);
      VerifyMoveMetadata(it->second, now, it->first);
      if (now.st_ino != it->second.st_ino || now.st_dev != it->second.st_dev ||
          (!directory && !SameEntry(it->second, now)))
        throw std::runtime_error(
            it->first + ": source changed during cleanup; left untouched");
      if (unlinkat(parent.n, leaf.c_str(), directory ? AT_REMOVEDIR : 0)) {
        const std::string error = it->first + ": " + strerror(errno);
        Problem(it->first, error);
        throw std::runtime_error(error);
      }
      Removed(it->first);
      if (removedAny)
        *removedAny = true;
      // The removal has committed. Poll before the next entry, not after
      // this one: a late cancellation cannot bring the source back.
      if (h.progress)
        h.progress(it->first, 0);
    }
  }
  void CopyBytes(int input, int output, const std::string &src,
                 const std::string &dst, bool reportProgress) {
    std::vector<char> buffer(131072);
    for (;;) {
      Check();
      const auto count = read(input, buffer.data(), buffer.size());
      if (!count)
        return;
      if (count < 0) {
        if (errno == EINTR)
          continue;
        Fail(src);
      }
      for (ssize_t offset = 0; offset < count;) {
        const auto written = WriteStaging(output, buffer.data() + offset, count - offset);
        if (written < 0 && errno == EINTR)
          continue;
        if (written <= 0)
          throw FilesystemError(dst, written == 0 ? EIO : errno);
        offset += written;
      }
      if (reportProgress)
        Tick(src, count);
    }
  }
  void SyncCopiedFile(int fd, const struct stat &source,
                      const std::string &path, bool ntfs) {
    const struct timespec times[2] = {source.st_atim, source.st_mtim};
    // NTFS updates mtime when flushed file pages reach the driver.
    if ((ntfs && fsync(fd)) || futimens(fd, times) || fsync(fd))
      Fail(path);
  }
  void CommitUnusedName(const std::string &source, const std::string &dst,
                        const struct stat &selected, const PinnedDirectory &parent) {
    Guard();
    parent.Verify();
    if (Exists(dst))
      throw std::runtime_error(dst + ": already exists; not replaced");
    BDirectory directory(ParentLocation(dst).c_str());
    struct stat opened, pinned;
    if (directory.GetStat(&opened) != B_OK || fstat(parent.Descriptor(), &pinned) ||
        opened.st_dev != pinned.st_dev || opened.st_ino != pinned.st_ino)
      throw std::runtime_error(dst + ": destination folder changed; left untouched");
    BEntry entry(source.c_str(), false);
    if (entry.GetStat(&opened) != B_OK || opened.st_dev != selected.st_dev ||
        opened.st_ino != selected.st_ino)
      throw std::runtime_error(source + ": entry changed; left untouched");
    // Haiku's native no-replace option is not atomic against concurrent writers.
    const status_t status = entry.MoveTo(&directory, Leaf(dst).c_str(), false);
    if (status != B_OK)
      throw std::runtime_error(dst + ": " + strerror(status));
  }
  struct stat PublishNewFile(const std::string &src, const std::string &dst,
                             const std::string &tmp, PinnedDirectory &parentDirectory,
                             FD &input, BNode &metadata, const struct stat &staged,
                             bool &published, bool ntfsTarget) {
    const int parent = parentDirectory.Descriptor();
    const auto leaf = Leaf(tmp);
    // BFS does not support hard links, and BEntry::MoveTo(false) checks
    // existence separately from rename. Claim the new leaf exclusively
    // and never reopen it by name for writing. Keep the verified stage
    // and publishing journal until the new inode has been flushed.
    struct stat opened;
    if (input.n < 0 || fstat(input.n, &opened) || !SameEntry(opened, staged))
      throw std::runtime_error(tmp + ": staging changed before publication");
    if (!StagingSpaceAvailable(parent, staged.st_size))
      Fail(dst);
    parentDirectory.Verify();
    FD final(openat(parent, Leaf(dst).c_str(), O_RDWR | O_CREAT | O_EXCL |
                    O_NOFOLLOW | O_CLOEXEC, 0600));
    if (final.n < 0) {
      if (errno == EEXIST)
        throw std::runtime_error(dst + ": new destination already exists; no file was replaced");
      Fail(dst);
    }
    published = true;
    ++mutations;
    Outcome(src, dst).status = "partial";
    const auto stagingDetail = Outcome(src).detail;
    Outcome(src).detail = "Publication incomplete; inspect the new file and recovery stage.";
    FD verify(dup(final.n));
    if (verify.n < 0)
      Fail(dst);
    SetMoveOwner(final.n, staged, dst);
    if (metadata.InitCheck() == B_OK)
      Attributes(tmp, dst, false, final.n, staged.st_dev, &metadata);
    const auto &backing = backingDevices.at(staged.st_dev);
    if (backing.removable) {
      const auto permissions = ntfsTarget ? StagingPermissions::PreserveIfSupported
                                          : StagingPermissions::Preserve;
      try {
        WriteRemovableStagingFile(std::exchange(input.n, -1),
                                  std::exchange(final.n, -1), staged,
                                  [&](size_t) {
                                    Check();
                                    CheckDestinationDevice(staged.st_dev);
                                  }, true, permissions, ntfsTarget);
      } catch (const RemovableCopyInterrupted &) {
        throw RemovableCopyInterrupted(dst + ": publication interrupted; "
            "inspect the incomplete file and recovery stage.",
            backing.mount.empty() ? ParentLocation(dst) : backing.mount);
      }
    } else {
      CopyBytes(input.n, final.n, tmp, dst, false);
      SetCopyMode(final.n, staged.st_mode, staged.st_dev, dst);
      SyncCopiedFile(final.n, staged, dst, ntfsTarget);
      if (close(std::exchange(final.n, -1)))
        Fail(dst);
    }
    struct stat finalStat;
    if (fstat(verify.n, &finalStat) || finalStat.st_size != staged.st_size ||
        !SamePublishedMode(staged, finalStat) ||
        finalStat.st_mtim.tv_sec != staged.st_mtim.tv_sec ||
        finalStat.st_mtim.tv_nsec != staged.st_mtim.tv_nsec ||
        fstatat(parent, Leaf(dst).c_str(), &opened, AT_SYMLINK_NOFOLLOW) ||
        !SameEntry(opened, finalStat))
      throw std::runtime_error(dst + ": published file changed");
    VerifyMoveMetadata(staged, finalStat, dst);
    if (fstatat(parent, leaf.c_str(), &opened, AT_SYMLINK_NOFOLLOW) ||
        !SameEntry(opened, staged))
      throw std::runtime_error(tmp + ": staging changed during publication");
    FlushDestination(parentDirectory, staged.st_dev, false, ntfsTarget);
    Outcome(src).detail = stagingDetail;
    if (unlinkat(parent, leaf.c_str(), 0)) {
      ++mutations;
      Outcome(src, dst).status = "published-recovery-pending";
      Fail(tmp);
    }
    return finalStat;
  }
  bool Copy(const std::string &src, std::string &dst,
            unsigned depth = 0) {
    Outcome(src, dst);
    const size_t before = mutations;
    bool folder = false;
    auto recordPartial = [&] {
      if (before == mutations)
        return false;
      if (folder && !depth && !deferCompletion)
        ++result.partialCopies;
      if (Outcome(src).status == "unprocessed")
        Outcome(src).status = "partial";
      return true;
    };
    try {
      const bool complete = CopyEntry(src, dst, depth, folder);
      if (!complete && !recordPartial())
        Outcome(src).status = "skipped";
      return complete;
    } catch (const Cancelled &) {
      recordPartial();
      auto &row = Outcome(src);
      if (row.status == "unprocessed")
        row.detail = "Cancelled before commit.";
      throw;
    } catch (const std::runtime_error &error) {
      recordPartial();
      Problem(src, error.what());
      throw;
    }
  }
  bool CopyEntry(const std::string &src, std::string &dst, unsigned depth,
                 bool &folder) {
    Check();
    if (depth > 64)
      throw std::runtime_error("Folder tree exceeds the safe copy depth");
    Guard();
    struct stat st;
    if (lstat(src.c_str(), &st) != 0)
      Fail(src);
    folder = S_ISDIR(st.st_mode);
    if (h.fileSize && S_ISREG(st.st_mode))
      h.fileSize(src, st.st_size);
    struct stat before {};
    bool existed = Exists(dst, &before);
    CheckSourceDevice(st.st_dev);
    if (existed && (deferCompletion || guardedJob->copyForArchiveMove))
      RequireSameMoveVolume(guardedJob->expectedDestination.device, before.st_dev);
    if (existed && guardedJob && guardedJob->action == Action::Duplicate)
      throw std::runtime_error(
          dst + ": already exists; duplicate does not overwrite");
    if (existed && st.st_dev == before.st_dev && st.st_ino == before.st_ino)
      throw std::runtime_error(src +
                               ": source and destination are the same entry");
    if (S_ISDIR(st.st_mode)) {
      if (existed && !S_ISDIR(before.st_mode))
        throw std::runtime_error(
            dst + ": destination is not a directory; not replaced");
      const auto parentPath = ParentLocation(dst);
      struct stat parentIdentity;
      if (stat(parentPath.c_str(), &parentIdentity))
        Fail(parentPath);
      PinnedDirectory parent(parentPath, parentIdentity.st_dev,
                             parentIdentity.st_ino, true);
      // Merge directories, but never traverse destination symbolic links.
      if (!existed) {
        if (mkdirat(parent.Descriptor(), Leaf(dst).c_str(), 0700))
          Fail(dst);
        ++mutations;
        if (fstatat(parent.Descriptor(), Leaf(dst).c_str(), &before,
                    AT_SYMLINK_NOFOLLOW) || !S_ISDIR(before.st_mode))
          throw std::runtime_error(dst + ": new destination folder changed");
      }
      parent.Verify();
      PinnedDirectory directory(dst, before.st_dev, before.st_ino);
      CheckDestinationDevice(before.st_dev);
      CheckDestinationDevice(parentIdentity.st_dev);
      const bool ntfsDirectory = NtfsCopy(before.st_dev);
      const bool ntfsParent = NtfsCopy(parentIdentity.st_dev);
      ClaimDirectory(before, src, dst);
      copyDirectories.emplace_back(dst, before);
      struct DirectoryScope {
        DirectoryStack &stack;
        ~DirectoryScope() { stack.pop_back(); }
      } scope{copyDirectories};
      // A merge keeps the destination folder's own metadata. Never remove
      // the source folder on Move if that would discard a distinct attribute.
      if (existed && deferCompletion) {
        VerifyMoveMetadata(st, before, dst);
        Attributes(src, dst, true);
      }
      bool complete = true;
      Dir dir(opendir(src.c_str()), closedir);
      if (!dir)
        Fail(src);
      while (auto e = NextEntry(dir.get(), src)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
          continue;
        auto childDestination = Join(dst, e->d_name);
        if (!Copy(Join(src, e->d_name), childDestination, depth + 1))
          complete = false;
      }
      Guard();
      if (!existed) {
        SetMoveOwner(directory.Descriptor(), st, dst);
        const bool useSourceMode =
            OrdinaryAttributes(src, dst, dirfd(dir.get()), directory.Descriptor(),
                               st.st_dev, before.st_dev);
        if (useSourceMode && !SetCopyMode(directory.Descriptor(), st.st_mode, before.st_dev, dst))
          PermissionNotice(src);
        struct timespec times[2] = {st.st_atim, st.st_mtim};
        if (futimens(directory.Descriptor(), times))
          Fail(dst);
      }
      struct stat expected;
      if (lstat(dst.c_str(), &expected))
        Fail(dst);
      VerifyMoveMetadata(st, expected, dst);
      Tick(src);
      Guard();
      parent.Verify();
      try {
        const bool flushed = FlushDestination(directory, expected.st_dev, false, ntfsDirectory);
        CheckDestinationDevice(parentIdentity.st_dev);
        const bool parentFlushed = FlushDestination(parent, parentIdentity.st_dev, false, ntfsParent);
        if (!flushed || !parentFlushed)
          DurabilityNotice(src);
        directory.Verify();
        Guard();
      } catch (const Cancelled &) {
        auto &row = Outcome(src, dst);
        row.status = "published-flush-failed";
        row.detail = "Cancelled during directory finalization; durability after restart is unknown.";
        throw;
      } catch (const std::runtime_error &) {
        Outcome(src, dst).status = "published-flush-failed";
        throw;
      }
      if (complete) {
        struct stat current;
        if (lstat(src.c_str(), &current) || !SameEntry(st, current))
          throw std::runtime_error(src + ": source folder changed during copy");
        Done(src, dst, expected);
      }
      return complete;
    }
    if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode))
      throw std::runtime_error(src + ": special files are not copied");
    if (existed && S_ISDIR(before.st_mode))
      throw std::runtime_error(dst +
                               ": a directory cannot be replaced by a file");
    if (existed && PlainCopy()) {
      CheckDestinationDevice(before.st_dev);
      if (NtfsCopy(before.st_dev))
        throw std::runtime_error(dst + ": NTFS directory durability is unavailable; "
                                     "existing files were left untouched. Copy to an unused name.");
    }
    bool keepBoth = false;
    if (existed && !Allow(dst, src, st, before, keepBoth))
      return false;
    if (keepBoth)
      Outcome(src, dst);
    Guard();
    const auto parentPath = ParentLocation(dst);
    struct stat parentIdentity;
    if (lstat(parentPath.c_str(), &parentIdentity))
      Fail(parentPath);
    PinnedDirectory parentDirectory(parentPath, parentIdentity.st_dev,
                                    parentIdentity.st_ino);
    const int parent = parentDirectory.Descriptor();
    // Keep staging on the target volume without extending a possibly
    // maximum-length destination leaf.
    if (!StagingSpaceAvailable(parent, 0))
      Fail(parentPath);
    RecoveryRecord recovery("copy", parentPath, ".listi-copy-", src, dst);
    const auto &tmp = recovery.Stage();
    const auto leaf = Leaf(tmp);
    const bool publishNewFile =
        S_ISREG(st.st_mode) && (keepBoth || (!existed && !h.privateStaging));
    FD out(-1), stagedInput(-1);
    BNode stagedMetadata;
    struct stat stagingIdentity{};
    bool bound = false, published = false;
    try {
      if (S_ISLNK(st.st_mode)) {
        char link[PATH_MAX];
        const auto n = readlink(src.c_str(), link, sizeof(link));
        if (n < 0 || size_t(n) == sizeof(link))
          Fail(src);
        link[n] = 0;
        if (symlinkat(link, parent, leaf.c_str()) ||
            fstatat(parent, leaf.c_str(), &stagingIdentity, AT_SYMLINK_NOFOLLOW))
          Fail(tmp);
      } else {
        out.n = openat(parent, leaf.c_str(),
                       O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (out.n < 0 || fstat(out.n, &stagingIdentity))
          Fail(tmp);
      }
      bound = true;
      recovery.Advance(RecoveryPhase::Writing, &stagingIdentity);
      CheckDestinationDevice(stagingIdentity.st_dev);
      if (publishNewFile) {
        // Keep an independent read descriptor before copying source permissions
        // onto the stage; those permissions may prevent a later open.
        stagedInput.n = openat(parent, leaf.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        struct stat pinned;
        if (stagedInput.n < 0 || fstat(stagedInput.n, &pinned) ||
            pinned.st_dev != stagingIdentity.st_dev || pinned.st_ino != stagingIdentity.st_ino)
          throw std::runtime_error(tmp + ": could not pin staging file");
        // FAT only synthesizes attributes from the stage's temporary name.
        if (!FatCopy(pinned.st_dev) &&
            !NtfsAttributesUnavailable(stagedInput.n, pinned.st_dev, tmp) &&
            (stagedMetadata.SetTo(tmp.c_str()) != B_OK ||
             stagedMetadata.GetStat(&pinned) != B_OK ||
             pinned.st_dev != stagingIdentity.st_dev || pinned.st_ino != stagingIdentity.st_ino))
          throw std::runtime_error(tmp + ": could not pin staging attributes");
      }
      const bool ntfsTarget = S_ISREG(st.st_mode) && NtfsCopy(stagingIdentity.st_dev);
      if (deferCompletion || guardedJob->copyForArchiveMove)
        RequireSameMoveVolume(guardedJob->expectedDestination.device,
                              stagingIdentity.st_dev);
      if (S_ISLNK(st.st_mode)) {
        if (UnlinksMoveSource() && !SameOwner(st, stagingIdentity) &&
            fchownat(parent, leaf.c_str(), st.st_uid, st.st_gid, AT_SYMLINK_NOFOLLOW))
          Fail(tmp + ": cannot preserve move link ownership");
        // BNode opens the link itself, including a dangling link.
        Attributes(src, tmp, false, -1, stagingIdentity.st_dev);
        struct timespec times[2] = {st.st_atim, st.st_mtim};
        if (utimensat(AT_FDCWD, tmp.c_str(), times, AT_SYMLINK_NOFOLLOW))
          Fail(tmp);
      } else {
        FD in(open(src.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK));
        if (in.n < 0)
          Fail(src);
        struct stat opened;
        if (fstat(in.n, &opened) || !S_ISREG(opened.st_mode) ||
            !SameEntry(opened, st))
          throw std::runtime_error(src + ": source changed before copying");
        auto copyAttributes = [&] {
          return OrdinaryAttributes(src, tmp, in.n, out.n, st.st_dev,
                                    stagingIdentity.st_dev);
        };
        const bool removableSource =
            backingDevices.at(st.st_dev).removable;
        if (removableSource || backingDevices.at(stagingIdentity.st_dev).removable) {
          SetMoveOwner(out.n, st, tmp);
          const bool useSourceMode = copyAttributes();
          // No filename is passed to the worker: only this queue thread can
          // replace the final target, after checking both identities again.
          const std::function<void(size_t)> checkpoint = [&](size_t bytes) {
            if (removableSource)
              CheckSourceDevice(st.st_dev);
            CheckDestinationDevice(stagingIdentity.st_dev);
            if (bytes)
              Tick(src, bytes);
            else
              Check();
          };
          // If cancellation leaves a kernel call pending, suspend the
          // removable endpoint even when devfs still shows it attached.
          const auto &endpoint = backingDevices.at(
              removableSource ? st.st_dev : stagingIdentity.st_dev);
          interruptedFolder = endpoint.mount.empty()
                                  ? (removableSource ? ParentLocation(guardedSource)
                                                     : guardedJob->destination)
                                  : endpoint.mount;
          auto permissions = StagingPermissions::Preserve;
          if (!useSourceMode)
            permissions = StagingPermissions::KeepCreationMode;
          else if (ntfsTarget)
            permissions = StagingPermissions::PreserveIfSupported;
          bool modeSet;
          try {
            modeSet = WriteRemovableStagingFile(
                std::exchange(in.n, -1), std::exchange(out.n, -1), st,
                checkpoint, true, permissions, ntfsTarget);
          } catch (const RemovableCopyInterrupted &) {
            // A checkpoint identifies its endpoint. A failed read/write or
            // unresolved syscall does not, so suspend each removable endpoint.
            if (!InterruptedVolumes().Contains(interruptedFolder)) {
              if (removableSource)
                InterruptedVolumes().Block(interruptedFolder, st.st_dev);
              const auto &target = backingDevices.at(stagingIdentity.st_dev);
              if (target.removable)
                InterruptedVolumes().Block(
                    target.mount.empty() ? guardedJob->destination : target.mount,
                    stagingIdentity.st_dev);
            }
            throw;
          }
          interruptedFolder.clear();
          if (!modeSet)
            PermissionNotice(src);
        } else {
          CopyBytes(in.n, out.n, src, dst, true);
          struct stat after;
          if (fstat(in.n, &after) || !SameEntry(after, st))
            throw std::runtime_error(src + ": source changed while copying");
          SetMoveOwner(out.n, st, tmp);
          if (copyAttributes() && !SetCopyMode(out.n, st.st_mode, stagingIdentity.st_dev, tmp))
            PermissionNotice(src);
          CheckDestinationDevice(stagingIdentity.st_dev);
          SyncCopiedFile(out.n, st, tmp, ntfsTarget);
        }
      }
      Check();
      struct stat current;
      if (lstat(src.c_str(), &current) || !SameEntry(current, st))
        throw std::runtime_error(src + ": source changed before commit");
      struct stat staged;
      if (fstatat(parent, leaf.c_str(), &staged, AT_SYMLINK_NOFOLLOW) ||
          staged.st_dev != stagingIdentity.st_dev ||
          staged.st_ino != stagingIdentity.st_ino)
        throw std::runtime_error(tmp + ": staging file changed");
      VerifyMoveMetadata(st, staged, tmp);
      if (S_ISREG(st.st_mode) && staged.st_size != st.st_size)
        throw std::runtime_error(src + ": copied size does not match the source");
      if (out.n >= 0 && close(std::exchange(out.n, -1)))
        Fail(tmp);
      CheckDestinationDevice(stagingIdentity.st_dev);
      struct stat namedParent;
      if (lstat(parentPath.c_str(), &namedParent) ||
          namedParent.st_dev != parentIdentity.st_dev ||
          namedParent.st_ino != parentIdentity.st_ino)
        throw std::runtime_error(parentPath + ": destination folder changed");
      recovery.PrepareCopyPublication();
      Guard();
      struct stat currentTarget;
      if (fstatat(parent, leaf.c_str(), &currentTarget, AT_SYMLINK_NOFOLLOW) ||
          currentTarget.st_dev != stagingIdentity.st_dev ||
          currentTarget.st_ino != stagingIdentity.st_ino)
        throw std::runtime_error(tmp + ": staging identity changed before publication");
      if (publishNewFile) {
        staged = PublishNewFile(src, dst, tmp, parentDirectory, stagedInput,
                                stagedMetadata, staged, published, ntfsTarget);
      } else if (existed) {
        const auto target = Leaf(dst);
        if (fstatat(parent, target.c_str(), &currentTarget, AT_SYMLINK_NOFOLLOW) ||
            !SameEntry(before, currentTarget))
          throw std::runtime_error(dst + ": destination changed during operation; left untouched");
        if (renameat(parent, leaf.c_str(), parent, target.c_str()))
          Fail(dst);
      } else {
        CommitUnusedName(tmp, dst, staged, parentDirectory);
      }
      published = true;
      bool flushed = false;
      try {
        flushed = FlushDestination(parentDirectory, stagingIdentity.st_dev,
                                   false, ntfsTarget);
      } catch (const RemovableCopyInterrupted &) {
        ++mutations;
        Outcome(src, dst).status = "published-flush-failed";
        throw;
      } catch (const Cancelled &) {
        ++mutations;
        auto &row = Outcome(src, dst);
        row.status = "published-flush-failed";
        row.detail = "Published before cancellation; durability after restart is unknown.";
        throw;
      } catch (const std::runtime_error &error) {
        ++mutations;
        Outcome(src, dst).status = "published-flush-failed";
        throw std::runtime_error(dst + ": published, but its parent directory "
                                 "was not flushed. " + error.what());
      }
      try {
        // Retire only after directory durability is acknowledged. If the
        // driver cannot do that, leave a review record even on successful I/O.
        if (flushed)
          recovery.Retire();
        else {
          recovery.Advance(RecoveryPhase::Published);
          DurabilityNotice(src);
        }
      } catch (const std::exception &error) {
        ++mutations;
        Outcome(src, dst).status = "published-recovery-pending";
        throw std::runtime_error(dst + ": published, but recovery bookkeeping failed: " + error.what());
      }
      Done(src, dst, staged);
    } catch (const RemovableCopyInterrupted &) {
      recovery.NoteFailure(false);
      // The mount may still answer cached lookups or hang on cleanup. Never
      // remove a similarly named stage by path on a reconnected disk.
      throw;
    } catch (...) {
      bool cancelled = false;
      try { throw; }
      catch (const Cancelled &) { cancelled = true; }
      catch (const std::runtime_error &error) {
        try {
          CheckRemovableFailure(error);
        } catch (const RemovableCopyInterrupted &) {
          recovery.NoteFailure(false);
          throw;
        }
      } catch (...) {}
      recovery.NoteFailure(cancelled);
      const auto device = backingDevices.find(stagingIdentity.st_dev);
      if (device != backingDevices.end() && device->second.removable)
        CheckDestinationDevice(stagingIdentity.st_dev);
      struct stat current;
      if (!published) {
        bool absent = false;
        if (!fstatat(parent, leaf.c_str(), &current, AT_SYMLINK_NOFOLLOW)) {
          absent = bound && current.st_dev == stagingIdentity.st_dev &&
                   current.st_ino == stagingIdentity.st_ino &&
                   !unlinkat(parent, leaf.c_str(), 0);
        } else {
          absent = errno == ENOENT;
        }
        if (absent) {
          try {
            CheckDestinationDevice(parentIdentity.st_dev);
            FlushDestination(parentDirectory, parentIdentity.st_dev, true);
            recovery.Retire();
          } catch (const RemovableCopyInterrupted &error) {
            AddError(tmp + ": " + error.what() +
                                    " Staging recovery record retained.");
            throw;
          } catch (const std::exception &error) {
            AddError(tmp + ": " + error.what() +
                                    " Staging recovery record retained.");
          }
        } else {
          AddError(tmp + ": temporary name retained for manual inspection.");
        }
      }
      throw;
    }
    return true;
  }
  PinnedDirectory PinParent(const std::string &folder, NodeIdentity expected = {}) {
    if (expected.device < 0) {
      struct stat current;
      if (stat(folder.c_str(), &current))
        Fail(folder);
      return PinnedDirectory(folder, current.st_dev, current.st_ino, true);
    }
    return PinnedDirectory(folder, expected.device, expected.inode, true);
  }
  void FlushPublishedName(const std::string &source, const std::string &destination,
                          const PinnedDirectory &parent,
                          const PinnedDirectory *oldParent = nullptr) {
    Outcome(source, destination).status = "published-flush-failed";
    try {
      parent.Flush();
      if (oldParent)
        oldParent->Flush();
    } catch (const RemovableCopyInterrupted &error) {
      Outcome(source).detail = error.what();
      InterruptedVolumes().Block(error.folder);
      result.interruptedCopyFolder = error.folder;
      throw;
    } catch (const std::runtime_error &error) {
      const std::string message = destination + ": name change already applied; check the "
                                  "destination before retrying. " + error.what();
      Outcome(source).detail = message;
      throw std::runtime_error(message);
    }
  }
  void MoveName(const Job &job, const std::string &source,
                const std::string &destination, const struct stat &original) {
    const auto folder = ParentLocation(destination);
    auto parent = PinParent(folder, job.expectedDestination);
    const auto fromFolder = ParentLocation(source);
    auto oldParent = PinParent(fromFolder);
    oldParent.Verify();
    CommitUnusedName(source, destination, original, parent);
    if (S_ISDIR(original.st_mode))
      ClaimDirectory(original, source, destination);
    FlushPublishedName(source, destination, parent, fromFolder == folder ? nullptr : &oldParent);
    Outcome(source, destination).status = "moved";
  }
  void Execute(const Job &job) {
    guardedJob = &job;
    const bool named =
        job.action == Action::Duplicate || job.action == Action::Link;
    for (const auto &source : job.sources) {
      std::string target;
      if (job.action == Action::Rename)
        target = job.destination;
      else if (named || job.action == Action::Copy || job.action == Action::Move)
        target = Join(job.destination, named ? job.entryName : Leaf(source));
      Outcome(source, target);
    }
    if (named) {
      if (job.sources.size() != 1 || job.expectedSources.size() != 1 ||
          job.expectedDestination.device < 0 ||
          (!job.destinationFolder.empty() &&
           job.destinationFolder != job.destination))
        throw std::runtime_error(
            "Named entry operations require one guarded local source and destination.");
      ValidateNewEntryName(job.destination, job.entryName);
    }
    if (job.action == Action::MakeFile || job.action == Action::MakeDirectory) {
      const auto folder = job.destinationFolder.empty() ? ParentLocation(job.destination)
                                                        : job.destinationFolder;
      const auto leaf = Leaf(job.destination);
      ValidateEntryLeaf(leaf);
      if (folder.empty() || job.destination != Join(folder, leaf) ||
          (job.action == Action::MakeFile && job.expectedDestination.device < 0))
        throw std::runtime_error("Choose an available local folder and one entry name.");
      Outcome("", job.destination);
      Check();
      Guard();
      auto parent = PinParent(folder, job.expectedDestination);
      if (job.action == Action::MakeFile) {
        FD created(openat(parent.Descriptor(), leaf.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666));
        if (created.n < 0)
          Fail(job.destination);
        Outcome("").status = "created";
        if (fsync(created.n))
          Fail(job.destination + " created but not flushed");
      } else if (mkdirat(parent.Descriptor(), leaf.c_str(), 0755))
        Fail(job.destination);
      FlushPublishedName("", job.destination, parent);
      result.completed++;
      Outcome("").status = "completed";
      return;
    }
    for (auto &source : job.sources) {
      Check();
      copied.clear();
      recoverableMove = false;
      interruptedFolder.clear();
      deferCompletion = job.action == Action::Move;
      try {
        guardedSource = source;
        const bool transfer = job.action == Action::Copy || job.action == Action::Move ||
                              job.action == Action::Duplicate;
        if (transfer) {
          if (job.expectedDestination.device >= 0)
            CheckDestinationDevice(job.expectedDestination.device);
          const auto expected = job.expectedSources.find(source);
          if (expected != job.expectedSources.end())
            CheckSourceDevice(expected->second.device);
        }
        Guard();
        // Do not allow volume roots, parent pseudoentries or empty paths to be
        // removed.
        std::string leaf = Leaf(source);
        if (source.empty() || source == "/" || leaf.empty() || leaf == "." ||
            leaf == "..")
          throw std::runtime_error("Refusing unsafe source path: " + source);
        struct stat sourceStat;
        if (lstat(source.c_str(), &sourceStat))
          Fail(source);
        if (job.action == Action::Link) {
          Check();
          Guard();
          auto directory = PinParent(job.destination, job.expectedDestination);
          const auto text = LinkText(job);
          const auto destination = Join(job.destination, job.entryName);
          if (symlinkat(text.c_str(), directory.Descriptor(), job.entryName.c_str()))
            Fail(destination);
          FlushPublishedName(source, destination, directory);
          Outcome(source, destination).status = "completed";
          result.completedRoots.emplace_back(source, destination);
          ++result.completed;
          if (h.fileDone)
            h.fileDone(source, destination);
          continue;
        }
        if (S_ISDIR(sourceStat.st_mode) && job.action != Action::Copy &&
            job.action != Action::Duplicate) {
          std::string parent = source.substr(0, source.find_last_of('/'));
          if (parent.empty())
            parent = "/";
          struct stat parentStat;
          if (stat(parent.c_str(), &parentStat))
            Fail(parent);
          if (sourceStat.st_dev != parentStat.st_dev)
            throw std::runtime_error(
                source +
                ": mounted volumes cannot be deleted, moved or renamed");
        }
        if (job.action == Action::Trash || job.action == Action::Restore) {
          std::string warning;
          const auto frozen = job.expectedSources.find(source);
          const NodeIdentity observed{sourceStat.st_dev, sourceStat.st_ino};
          const NodeIdentity &expected =
              frozen == job.expectedSources.end() ? observed : frozen->second;
          const auto confirmed = job.restoreTargets.find(source);
          if (!job.restoreTargets.empty() &&
              (confirmed == job.restoreTargets.end() || confirmed->second.empty()))
            throw std::runtime_error(source + ": original location unavailable at confirmation; retained in Trash.");
          const std::string destination =
              job.action == Action::Trash
                  ? MoveToTrash(source, &expected)
                  : RestoreFromTrash(source, &warning, &expected, confirmed == job.restoreTargets.end()
                        ? nullptr : &confirmed->second);
          auto &row = Outcome(source, destination);
          row.status = "moved";
          if (!warning.empty()) {
            row.detail = warning;
            AddError(warning);
          }
          result.completed++;
          if (h.fileDone)
            h.fileDone(source, destination);
          continue;
        }
        if (job.action == Action::Delete) {
          Snapshot deletion;
          SnapshotTree(source, deletion, sourceStat.st_dev);
          if (!SameEntry(sourceStat, deletion.at(source)))
            throw std::runtime_error(source +
                                     ": selected source changed; left untouched");
          for (const auto &entry : deletion)
            Outcome(entry.first);
          RemoveCopied(source, deletion);
          result.completed++;
          continue;
        }
        std::string destination = job.action == Action::Rename
                                      ? job.destination
                                      : Join(Canonical(job.destination),
                                             named ? job.entryName : leaf);
        if (S_ISDIR(sourceStat.st_mode)) {
          const auto slash = destination.find_last_of('/');
          const auto parent = Canonical(slash == 0 ? "/" : destination.substr(0, slash));
          const auto from = Canonical(source);
          if (parent == from ||
              parent.compare(0, from.size() + 1, from + "/") == 0)
            throw std::runtime_error(
                "Cannot copy or move a directory inside itself");
        }
        if (job.action == Action::Rename) {
          if (Exists(destination))
            throw std::runtime_error(
                destination + ": already exists; rename does not overwrite");
          MoveName(job, source, destination, sourceStat);
          if (h.fileDone)
            h.fileDone(source, destination);
          result.completed++;
          continue;
        }
        recoverableMove = job.action == Action::Move &&
                          sourceStat.st_dev != job.expectedDestination.device;
        if (recoverableMove) {
          CheckSourceDevice(sourceStat.st_dev);
          CheckDestinationDevice(job.expectedDestination.device);
          CheckTrashSource(source);
        }
        if (job.action == Action::Move && !recoverableMove &&
            !Exists(destination)) {
          MoveName(job, source, destination, sourceStat);
          result.completedRoots.emplace_back(source, destination);
          ++result.completed;
          if (h.fileDone)
            h.fileDone(source, destination);
          if (h.progress)
            h.progress(source, 0);
          continue;
        }
        Snapshot beforeMove;
        if (job.action == Action::Move) {
          SnapshotTree(source, beforeMove, sourceStat.st_dev);
          CheckMoveMerges(source, destination, beforeMove);
          for (const auto &entry : beforeMove)
            Outcome(entry.first, destination + entry.first.substr(source.size()));
        }
        if (Copy(source, destination)) {
          if (job.action == Action::Move) {
            Guard();
            bool removedAny = false;
            try {
              if (recoverableMove)
                RetainMovedSource(source, beforeMove);
              else
                RemoveCopied(source, beforeMove, &removedAny);
            } catch (const Cancelled &) {
              if (!removedAny)
                result.retainedMoveCopy.emplace(source, destination);
              throw;
            }
          }
          result.completedRoots.emplace_back(source, destination);
          result.completed++;
          if (h.fileDone)
            for (auto &entry : copied)
              h.fileDone(entry.first, entry.second.destination);
        }
      } catch (const RemovableCopyInterrupted &e) {
        result.interruptedCopyFolder = e.folder.empty() ? interruptedFolder : e.folder;
        if (!InterruptedVolumes().Contains(result.interruptedCopyFolder)) {
          for (const auto &[device, backing] : backingDevices)
            if (backing.removable && !backing.mount.empty() &&
                backing.mount == result.interruptedCopyFolder)
              InterruptedVolumes().Block(result.interruptedCopyFolder, device);
          InterruptedVolumes().Block(result.interruptedCopyFolder);
        }
        Problem(source, e.what());
        if (h.fileFailed)
          h.fileFailed(source, e.what());
        throw; // Later selected roots must not enter the disconnected mount.
      } catch (const std::runtime_error &e) {
        if (job.action == Action::Copy || job.action == Action::Move ||
            job.action == Action::Duplicate) {
          try {
            CheckRemovableFailure(e);
          } catch (const RemovableCopyInterrupted &lost) {
            result.interruptedCopyFolder = lost.folder.empty() ? interruptedFolder : lost.folder;
            Problem(source, lost.what());
            if (h.fileFailed)
              h.fileFailed(source, lost.what());
            throw;
          }
        }
        Problem(source, e.what());
        AddError(e.what());
        if (h.fileFailed)
          h.fileFailed(source, e.what());
      }
    }
  }
};
} // namespace
Result Run(const Job &j, const Hooks &h) {
  if (j.action == Action::BulkRename || j.action == Action::UndoBulkRename)
    return RunBulkRename(j, h);
  if (j.action == Action::ConvertImage)
    return RunImageConversion(j, h);
  if (j.action == Action::CreateArchive || j.action == Action::ExtractArchive ||
      j.action == Action::EditArchive || j.action == Action::MoveArchive) {
    // Archive routines make private intermediate calls to Run and use their
    // synchronous Result to decide whether publication may proceed.
    Hooks archive = h;
    archive.report.reset();
    return RunArchiveJob(j, archive);
  }
  // Source-only actions must not depend on the location in the other pane.
  const bool usesDestination =
      j.action == Action::Copy || j.action == Action::Move ||
      j.action == Action::Rename || j.action == Action::MakeDirectory ||
      j.action == Action::Duplicate || j.action == Action::Link;
  bool virtualJob = usesDestination && IsVirtual(j.destination);
  for (auto &path : j.sources)
    virtualJob = virtualJob || IsVirtual(path);
  if (virtualJob) {
    if (j.action == Action::Duplicate || j.action == Action::Link) {
      Result result;
      result.errors.push_back(
          "Guarded file clipboard transfers require local entries.");
      return result;
    }
    try {
      for (const auto &entry : j.expectedArchiveSets)
        if (FindArchiveVolumes(entry.first).Fingerprint() != entry.second)
          throw std::runtime_error(
              "Archive changed after selection. Select its entries again.");
    } catch (const std::exception &e) {
      Result result;
      result.errors.push_back(e.what());
      return result;
    }
    return RunVirtual(j, h);
  }

  Runner r{h};
  try {
    if (j.action == Action::Move) {
      Job move = j;
      // A queued selection already carries its original identities. Guard()
      // rejects a stale entry individually without cancelling its valid siblings.
      const bool frozen = move.expectedDestination.device >= 0 &&
          std::all_of(move.sources.begin(), move.sources.end(),
                      [&](const std::string &path) { return move.expectedSources.count(path); });
      if (!frozen)
        FreezeLocalJob(move, move.destination);
      r.Execute(move);
    } else
      r.Execute(j);
  } catch (const Cancelled &) {
    r.result.cancelled = true;
  } catch (const std::exception &e) {
    r.AddError(e.what());
    if (h.report && r.pendingOutcome && h.report->OutcomeCount() == 0)
      r.Problem(r.pendingOutcome->source, e.what());
    else if (r.result.outcomes.size() == 1)
      r.Problem(r.result.outcomes.front().source, e.what());
  }
  r.FlushOutcome();
  return std::move(r.result);
}
} // namespace Listi
