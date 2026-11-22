// SPDX-License-Identifier: MIT
#include "FileOperation.h"
#include "BulkRename.h"
#include "ArchiveVolumes.h"
#include "ArchiveJobs.h"
#include "ImageConvert.h"
#include "NamedEntry.h"
#include "JobSnapshot.h"
#include "MoveVolume.h"
#include "RemovableCopy.h"
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
void Fail(const std::string &p) {
  throw std::runtime_error(p + ": " + strerror(errno));
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
  std::string guardedSource;
  std::map<std::string, size_t> outcomeIndex;
  struct BackingDevice {
    std::string path, mount;
    NodeIdentity node;
    bool removable = false;
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
        throw RemovableCopyInterrupted(message);
      }
      throw std::runtime_error(message);
    }
    backing.node = {st.st_dev, st.st_ino};
  }
  void CheckDestinationDevice(dev_t volume) {
    CheckDevice(volume, guardedJob->destination);
  }
  void CheckSourceDevice(dev_t volume) {
    CheckDevice(volume, ParentLocation(guardedSource));
  }
  size_t mutations = 0;
  EntryOutcome &Outcome(const std::string &src, const std::string &dst = {}) {
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
  }

  bool deferCompletion = false, recoverableMove = false;
  struct CopiedEntry {
    std::string destination;
    struct stat committed;
  };
  std::map<std::string, CopiedEntry> copied;
  void Done(const std::string &src, const std::string &dst,
            const struct stat &expected) {
    ++mutations;
    CheckDestinationDevice(expected.st_dev);
    struct stat committed;
    if (lstat(dst.c_str(), &committed) || expected.st_dev != committed.st_dev ||
        expected.st_ino != committed.st_ino || expected.st_mode != committed.st_mode ||
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
  void Attributes(const std::string &src, const std::string &dst,
                  bool verifyMerge = false) {
    BNode from(src.c_str()), to(dst.c_str());
    char name[B_ATTR_NAME_LENGTH];
    if (from.InitCheck() != B_OK || to.InitCheck() != B_OK)
      throw std::runtime_error("Could not open copy metadata");
    status_t status;
    std::vector<char> buf(65536);
    std::vector<char> existing(verifyMerge ? buf.size() : 0);
    while ((status = from.GetNextAttrName(name)) == B_OK) {
      Check();
      attr_info info;
      if (from.GetAttrInfo(name, &info) != B_OK)
        throw std::runtime_error(src + ": attribute changed during copy");
      if (verifyMerge) {
        attr_info target;
        if (to.GetAttrInfo(name, &target) != B_OK ||
            target.type != info.type || target.size != info.size)
          throw std::runtime_error(dst + ": folder merge would lose source attribute " +
                                   name + "; source retained");
      } else if (info.size == 0)
        throw std::runtime_error(
            src + ": zero-length attribute cannot be preserved: " + name);
      off_t pos = 0;
      while (pos < info.size) {
        ssize_t n = from.ReadAttr(name, info.type, pos, buf.data(),
                                  std::min<off_t>(buf.size(), info.size - pos));
        if (n <= 0)
          throw std::runtime_error(src + ": could not read attribute " + name);
        if (verifyMerge) {
          if (to.ReadAttr(name, info.type, pos, existing.data(), n) != n ||
              memcmp(existing.data(), buf.data(), n) != 0)
            throw std::runtime_error(dst + ": folder merge would change attribute " +
                                     name + "; source retained");
        } else if (to.WriteAttr(name, info.type, pos, buf.data(), n) != n)
          throw std::runtime_error(dst + ": could not preserve attribute " +
                                   name);
        pos += n;
      }
    }
    if (status != B_ENTRY_NOT_FOUND)
      throw std::runtime_error(src + ": could not enumerate attributes");
  }
  bool Allow(const std::string &dst) {
    if (!Exists(dst))
      return true;
    if (guardedJob && guardedJob->action == Action::Duplicate)
      throw std::runtime_error(
          dst + ": already exists; duplicate does not overwrite");
    if (skipAll) {
      result.skipped++;
      return false;
    }
    if (replaceAll)
      return true;
    Decision d = h.conflict ? h.conflict(dst) : Decision::Abort;
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
  bool Remove(const std::string &p) {
    Check();
    struct stat st;
    if (lstat(p.c_str(), &st) != 0)
      Fail(p);
    if (S_ISDIR(st.st_mode)) {
      Dir dir(opendir(p.c_str()), closedir);
      if (!dir)
        Fail(p);
      while (auto e = NextEntry(dir.get(), p)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
          continue;
        Remove(Join(p, e->d_name));
      }
      if (rmdir(p.c_str()) != 0)
        Fail(p);
    } else if (unlink(p.c_str()) != 0)
      Fail(p);
    Tick(p);
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
    if (recoverableMove && !S_ISDIR(st.st_mode) && st.st_nlink > 1)
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
  bool SameEntry(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
           a.st_mode == b.st_mode && a.st_size == b.st_size &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
           a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
           a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
  }
  void VerifyCopy(const std::string &source, dev_t sourceVolume) {
    if (!deferCompletion)
      return;
    const auto recorded = copied.find(source);
    if (recorded == copied.end())
      throw std::runtime_error(source + ": no committed copy; source retained");
    const auto &copy = recorded->second;
    CheckDestinationDevice(copy.committed.st_dev);
    if (!recoverableMove)
      RequireSameMoveVolume(sourceVolume, copy.committed.st_dev);
    struct stat now;
    if (lstat(copy.destination.c_str(), &now) || !SameEntry(copy.committed, now)) {
      auto &row = Outcome(source);
      row.status = "failed";
      row.detail = copy.destination + ": destination changed; source retained";
      if (source != guardedSource)
        Outcome(guardedSource).status = "partial";
      throw std::runtime_error(row.detail);
    }
  }
  void VerifyCopiedTree(const std::string &root, const Snapshot &before) {
    Snapshot after;
    SnapshotTree(root, after, before.at(root).st_dev);
    if (before.size() != after.size())
      throw std::runtime_error(
          root + ": source changed; copied data kept and source retained");
    for (auto &entry : before) {
      auto found = after.find(entry.first);
      VerifyCopy(entry.first, entry.second.st_dev);
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
      FD parent(open(parentPath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
      struct stat now, parentNode;
      if (parent.n < 0 || fstat(parent.n, &parentNode) ||
          fstatat(parent.n, leaf.c_str(), &now, AT_SYMLINK_NOFOLLOW))
        Fail(it->first);
      const auto expectedParent = before.find(parentPath);
      if (expectedParent != before.end() &&
          (parentNode.st_dev != expectedParent->second.st_dev ||
           parentNode.st_ino != expectedParent->second.st_ino))
        throw std::runtime_error(parentPath + ": source folder changed during cleanup");
      VerifyCopy(it->first, it->second.st_dev);
      bool directory = S_ISDIR(it->second.st_mode);
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
  void Commit(const std::string &tmp, const std::string &dst, bool replace,
              const struct stat &original) {
    Guard();
    struct stat now;
    bool exists = Exists(dst, &now);
    if (exists != replace || (exists && !SameEntry(now, original)))
      throw std::runtime_error(
          dst + ": destination changed during operation; left untouched");
    BEntry e(tmp.c_str());
    std::string parent = dst.substr(0, dst.find_last_of('/'));
    if (parent.empty())
      parent = "/";
    BDirectory directory(parent.c_str());
    status_t status = e.MoveTo(&directory, Leaf(dst).c_str(), replace);
    if (status != B_OK)
      throw std::runtime_error(dst + ": " + strerror(status));
  }
  bool Copy(const std::string &src, const std::string &dst,
            unsigned depth = 0) {
    Outcome(src, dst);
    const size_t before = mutations;
    try {
      const bool complete = CopyEntry(src, dst, depth);
      if (!complete)
        Outcome(src).status = before == mutations ? "skipped" : "partial";
      return complete;
    } catch (const Cancelled &) {
      auto &row = Outcome(src);
      if (before != mutations)
        row.status = "partial";
      if (row.status == "unprocessed")
        row.detail = "Cancelled before commit.";
      throw;
    } catch (const std::runtime_error &error) {
      if (before != mutations && Outcome(src).status == "unprocessed")
        Outcome(src).status = "partial";
      Problem(src, error.what());
      throw;
    }
  }
  bool CopyEntry(const std::string &src, const std::string &dst, unsigned depth) {
    Check();
    if (depth > 64)
      throw std::runtime_error("Folder tree exceeds the safe copy depth");
    Guard();
    struct stat st;
    if (lstat(src.c_str(), &st) != 0)
      Fail(src);
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
      // Merge directories, but never traverse destination symbolic links.
      if (!existed) {
        if (mkdir(dst.c_str(), 0700) != 0)
          Fail(dst);
        ++mutations;
        if (lstat(dst.c_str(), &before) || !S_ISDIR(before.st_mode))
          throw std::runtime_error(dst + ": new destination folder changed");
      }
      CheckDestinationDevice(before.st_dev);
      copyDirectories.emplace_back(dst, before);
      struct DirectoryScope {
        DirectoryStack &stack;
        ~DirectoryScope() { stack.pop_back(); }
      } scope{copyDirectories};
      // A merge keeps the destination folder's own metadata. Never remove
      // the source folder on Move if that would discard a distinct attribute.
      if (existed && deferCompletion)
        Attributes(src, dst, true);
      bool complete = true;
      Dir dir(opendir(src.c_str()), closedir);
      if (!dir)
        Fail(src);
      while (auto e = NextEntry(dir.get(), src)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
          continue;
        if (!Copy(Join(src, e->d_name), Join(dst, e->d_name), depth + 1))
          complete = false;
      }
      Guard();
      if (!existed) {
        Attributes(src, dst);
        if (chmod(dst.c_str(), st.st_mode & 07777))
          Fail(dst);
        struct timespec times[2] = {st.st_atim, st.st_mtim};
        if (utimensat(AT_FDCWD, dst.c_str(), times, 0))
          Fail(dst);
      }
      struct stat expected;
      if (lstat(dst.c_str(), &expected))
        Fail(dst);
      Tick(src);
      Guard();
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
    if (!Allow(dst))
      return false;
    Guard();
    const auto parentPath = ParentLocation(dst);
    FD parent(open(parentPath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
    struct stat parentIdentity;
    if (parent.n < 0 || fstat(parent.n, &parentIdentity))
      Fail(parentPath);
    // Keep staging on the target volume without extending a possibly
    // maximum-length destination leaf.
    std::string pattern = dst.substr(0, dst.rfind('/') + 1) + ".listi-copy-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    FD out(mkstemp(name.data()));
    if (out.n < 0)
      Fail(dst);
    std::string tmp = name.data();
    struct stat stagingIdentity;
    if (fstat(out.n, &stagingIdentity))
      Fail(tmp);
    try {
      CheckDestinationDevice(stagingIdentity.st_dev);
      if (deferCompletion || guardedJob->copyForArchiveMove)
        RequireSameMoveVolume(guardedJob->expectedDestination.device,
                              stagingIdentity.st_dev);
      if (S_ISLNK(st.st_mode)) {
        char link[PATH_MAX];
        ssize_t n = readlink(src.c_str(), link, sizeof(link) - 1);
        if (n < 0)
          Fail(src);
        link[n] = 0;
        if (unlink(tmp.c_str()) || symlink(link, tmp.c_str()) ||
            lstat(tmp.c_str(), &stagingIdentity))
          Fail(tmp);
        // BNode opens the link itself, including a dangling link.
        Attributes(src, tmp);
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
        const bool removableSource =
            backingDevices.at(st.st_dev).removable;
        if (removableSource || backingDevices.at(stagingIdentity.st_dev).removable) {
          Attributes(src, tmp);
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
          WriteRemovableStagingFile(std::exchange(in.n, -1),
                                    std::exchange(out.n, -1), st, checkpoint);
          interruptedFolder.clear();
        } else {
          std::vector<char> buf(131072);
          for (;;) {
            Check();
            ssize_t n = read(in.n, buf.data(), buf.size());
            if (n == 0)
              break;
            if (n < 0) {
              if (errno == EINTR)
                continue;
              Fail(src);
            }
            ssize_t at = 0;
            while (at < n) {
              ssize_t w = write(out.n, buf.data() + at, n - at);
              if (w < 0 && errno == EINTR)
                continue;
              if (w <= 0)
                Fail(dst);
              at += w;
            }
            Tick(src, n);
          }
          struct stat after;
          if (fstat(in.n, &after) || !SameEntry(after, st))
            throw std::runtime_error(src + ": source changed while copying");
          Attributes(src, tmp);
          if (fchmod(out.n, st.st_mode & 07777))
            Fail(tmp);
          struct timespec times[2] = {st.st_atim, st.st_mtim};
          CheckDestinationDevice(stagingIdentity.st_dev);
          if (futimens(out.n, times) || fsync(out.n))
            Fail(tmp);
        }
      }
      Check();
      struct stat current;
      if (lstat(src.c_str(), &current) || !SameEntry(current, st))
        throw std::runtime_error(src + ": source changed before commit");
      struct stat staged;
      if (lstat(tmp.c_str(), &staged) || staged.st_dev != stagingIdentity.st_dev ||
          staged.st_ino != stagingIdentity.st_ino)
        throw std::runtime_error(tmp + ": staging file changed");
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
      Commit(tmp, dst, existed, before);
      // Removable media have their own interruptible file flush. A directory
      // flush on the queue thread would reintroduce a hang on unplug.
      if (!backingDevices.at(stagingIdentity.st_dev).removable && fsync(parent.n)) {
        ++mutations;
        Outcome(src, dst).status = "published-flush-failed";
        throw std::runtime_error(dst +
            ": published, but its directory could not be flushed. Source retained; "
            "durability after restart is unknown.");
      }
      Done(src, dst, staged);
    } catch (const RemovableCopyInterrupted &) {
      // The mount may still answer cached lookups or hang on cleanup. Never
      // remove a similarly named stage by path on a reconnected disk.
      throw;
    } catch (...) {
      const auto device = backingDevices.find(stagingIdentity.st_dev);
      if (device != backingDevices.end() && device->second.removable)
        CheckDestinationDevice(stagingIdentity.st_dev);
      struct stat current;
      const auto leaf = Leaf(tmp);
      if (fstatat(parent.n, leaf.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0 &&
          current.st_dev == stagingIdentity.st_dev && current.st_ino == stagingIdentity.st_ino &&
          unlinkat(parent.n, leaf.c_str(), 0))
        result.errors.push_back(tmp + ": temporary copy could not be removed: " +
                                strerror(errno));
      throw;
    }
    return true;
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
    if (job.action == Action::MakeDirectory) {
      Outcome("", job.destination);
      Check();
      Guard();
      if (mkdir(job.destination.c_str(), 0755))
        Fail(job.destination);
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
          BDirectory directory(job.destination.c_str());
          node_ref node;
          if (directory.GetNodeRef(&node) != B_OK ||
              node.device != job.expectedDestination.device ||
              node.node != job.expectedDestination.inode)
            throw std::runtime_error(
                "The link destination changed; nothing was created.");
          // One native exclusive create: existing files, folders and broken
          // symlinks are never replaced. No shell or source dereferencing.
          const auto text = LinkText(job);
          status_t status = directory.CreateSymLink(job.entryName.c_str(),
                                                    text.c_str(), nullptr);
          if (status != B_OK)
            throw std::runtime_error("Could not create link: " +
                                     std::string(strerror(status)));
          const auto destination = Join(job.destination, job.entryName);
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
          const std::string destination =
              job.action == Action::Trash
                  ? MoveToTrash(source, &expected)
                  : RestoreFromTrash(source, &warning, &expected);
          auto &row = Outcome(source, destination);
          row.status = "moved";
          if (!warning.empty()) {
            row.detail = warning;
            result.errors.push_back(warning);
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
          struct stat empty {};
          Commit(source, destination, false, empty);
          Outcome(source, destination).status = "moved";
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
          Guard();
          BEntry e(source.c_str());
          BDirectory dir(job.destination.c_str());
          status_t s = e.MoveTo(&dir, leaf.c_str(), false);
          if (s == B_OK) {
            Outcome(source, destination).status = "moved";
            result.completedRoots.emplace_back(source, destination);
            ++result.completed;
            if (h.fileDone)
              h.fileDone(source, destination);
            if (h.progress)
              h.progress(source, 0);
            continue;
          }
          // Do not fall back to copy-and-delete if the mount changed after preflight.
          throw std::runtime_error(source + ": " + strerror(s));
        }
        Snapshot beforeMove;
        if (job.action == Action::Move) {
          SnapshotTree(source, beforeMove, sourceStat.st_dev);
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
        result.interruptedCopyFolder = interruptedFolder;
        Problem(source, e.what());
        if (h.fileFailed)
          h.fileFailed(source, e.what());
        throw; // Later selected roots must not enter the disconnected mount.
      } catch (const std::runtime_error &e) {
        Problem(source, e.what());
        result.errors.push_back(e.what());
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
      j.action == Action::EditArchive || j.action == Action::MoveArchive)
    return RunArchiveJob(j, h);
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
    r.result.errors.push_back(e.what());
    if (r.result.outcomes.size() == 1)
      r.Problem(r.result.outcomes.front().source, e.what());
  }
  return r.result;
}
} // namespace Listi
