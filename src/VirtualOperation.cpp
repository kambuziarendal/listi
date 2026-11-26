// SPDX-License-Identifier: MIT
#include "FileOperation.h"
#include "JobReportStore.h"
#include "DirectoryFinalization.h"
#include "RemovableCopy.h"
#include "StagingSpace.h"
#include "Sha256.h"
#include "RecoveryInventory.h"
#include "UnavailableVolume.h"
#include "VirtualFile.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace Listi {
namespace {
struct TempFile {
  RecoveryRecord record;
  std::string folder, path;
  struct stat directory{}, file{};
  int fd = -1, parent = -1;
  TempFile(const std::string &name, const std::string &source, const std::string &target)
      : record("scratch", std::filesystem::canonical("/tmp"), "Listi-transfer-", source, target),
        folder(record.Stage()), path(ChildLocation(folder, name)) {
    if (mkdir(folder.c_str(), 0700))
      throw std::runtime_error("Cannot create private transfer staging.");
    parent = open(folder.c_str(), O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    try {
      if (parent < 0 || fstat(parent, &directory))
        throw std::runtime_error("Cannot open private transfer staging.");
      record.Advance(RecoveryPhase::Writing, &directory);
      fd = openat(parent, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (fd < 0 || fstat(fd, &file))
        throw std::runtime_error("Cannot create transfer staging file.");
    } catch (...) {
      Cleanup();
      throw;
    }
  }
  ~TempFile() { Cleanup(); }
  void Cleanup() noexcept {
    if (fd >= 0)
      close(fd);
    if (parent < 0)
      return;
    struct stat current;
    const auto leaf = std::filesystem::path(path).filename();
    if (!fstatat(parent, leaf.c_str(), &current, AT_SYMLINK_NOFOLLOW) &&
        current.st_dev == file.st_dev && current.st_ino == file.st_ino)
      unlinkat(parent, leaf.c_str(), 0);
    try {
      const auto base = ParentLocation(folder);
      if (!stat(base.c_str(), &current)) {
        PinnedDirectory namespaceParent(base, current.st_dev, current.st_ino);
        if (!lstat(folder.c_str(), &current) && current.st_dev == directory.st_dev &&
            current.st_ino == directory.st_ino &&
            !unlinkat(namespaceParent.Descriptor(), LocationLeaf(folder).c_str(), AT_REMOVEDIR)) {
          namespaceParent.FlushAfterCleanup();
          record.Retire();
        }
      }
    } catch (...) { /* Read-only review still available. */ }
    close(parent);
  }
  void Write(const char *p, size_t n) {
    while (n) {
      auto r = WriteStaging(fd, p, n);
      if (r < 0 && errno == EINTR)
        continue;
      if (r <= 0)
        throw std::runtime_error(
            std::string("Could not write private transfer staging: ") +
            strerror(r < 0 ? errno : EIO));
      p += r;
      n -= r;
    }
  }
  void Finish(const FileEntry &source) {
    // Preserve available portable metadata, never import setuid/setgid bits.
    if (fchmod(fd, source.permissions & 0777))
      throw std::runtime_error(strerror(errno));
    if (source.modifiedKnown || source.modified > 0) {
      struct timespec times[2] = {
          {0, UTIME_OMIT}, {source.modified, source.ModifiedNanoseconds()}};
      if (futimens(fd, times))
        throw std::runtime_error(strerror(errno));
    }
    if (fsync(fd))
      throw std::runtime_error(strerror(errno));
    if (close(fd)) {
      fd = -1;
      throw std::runtime_error(strerror(errno));
    }
    fd = -1;
  }
};
std::string RemoteDigest(const std::string &path, const TransferControl &control) {
  Sha256 hash;
  ReadFile(path, [&](const char *data, size_t count) {
    if (control.cleanup) {
      control.Check();
      control.cleanup->Consume(count);
    }
    hash.Update(data, count);
  }, control);
  return hash.Finish();
}
bool MatchesRemoteSelection(const FileEntry &selected, const FileEntry &current) {
  return selected.directory == current.directory && selected.link == current.link &&
         (!selected.sizeKnown || (current.sizeKnown && selected.size == current.size)) &&
         (!selected.modifiedKnown || selected.SameModification(current)) &&
         (selected.providerIdentity.empty() || selected.providerIdentity == current.providerIdentity);
}

// Enumeration order may change even when the source tree does not.
bool SameListing(std::vector<FileEntry> &before, std::vector<FileEntry> &after) {
  auto byName = [](const FileEntry &a, const FileEntry &b) { return a.name < b.name; };
  std::sort(before.begin(), before.end(), byName);
  std::sort(after.begin(), after.end(), byName);
  return std::equal(before.begin(), before.end(), after.begin(), after.end(),
                    [](const FileEntry &a, const FileEntry &b) {
                      return a.name == b.name && a.SameRevision(b);
                    });
}
std::string TransferIdentity(const std::string &path) {
  auto location = ParseLocation(path);
  const unsigned defaultPort = location.scheme == "sftp"   ? 22
                               : location.scheme == "ftp"  ? 21
                               : location.scheme == "ftps" ? 990
                               : location.scheme == "smb"  ? 445
                                                           : 0;
  if (location.port == defaultPort)
    location.port = 0;
  return location.URL();
}
bool SameLocal(const struct stat &a, const struct stat &b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
         a.st_mode == b.st_mode && a.st_size == b.st_size &&
         a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
         a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
         a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
         a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
struct Runner {
  const Hooks &hooks;
  const Job &job;
  // Preserve each local destination directory across staging, recursive descent
  // and conflict questions. Contents may change; the directory identity may not.
  std::map<std::string, NodeIdentity> directories;
  // Keep claims across selected roots, unlike the scoped directory guards.
  std::map<std::string, std::string> directoryOwners;
  void ClaimDirectory(const std::string &key, const std::string &src, const std::string &dst) {
    const auto inserted = directoryOwners.emplace(key, src);
    if (!inserted.second && inserted.first->second != src)
      throw std::runtime_error("Two source folders resolve to the same destination: " + dst);
  }
  std::string selectedSource;
  void Guard() {
    if (job.checkDestinationWindow)
      job.checkDestinationWindow();
    if (InterruptedVolumes().Contains(selectedSource))
      throw std::runtime_error(selectedSource + ": " + kUnavailableVolumeText);
    struct stat st;
    auto source = job.expectedSources.find(selectedSource);
    if (source != job.expectedSources.end() &&
        (lstat(source->first.c_str(), &st) ||
         st.st_dev != source->second.device ||
         st.st_ino != source->second.inode))
      throw std::runtime_error("Selected source changed; operation stopped.");
    for (const auto &item : directories)
      if (stat(item.first.c_str(), &st) || !S_ISDIR(st.st_mode) ||
          st.st_dev != item.second.device || st.st_ino != item.second.inode)
        throw std::runtime_error(
            "Selected destination changed; operation stopped.");
  }
  void RememberDirectory(const std::string &path) {
    if (IsVirtual(path) || directories.count(path)) {
      Guard();
      return;
    }
    struct stat st;
    if (stat(path.c_str(), &st) || !S_ISDIR(st.st_mode))
      throw std::runtime_error("Destination folder is unavailable.");
    directories[path] = {int32_t(st.st_dev), int64_t(st.st_ino)};
    Guard();
  }
  Result result;
  void AddError(const std::string &error) {
    if (hooks.report)
      hooks.report->AddError(error);
    else
      result.errors.push_back(error);
  }
  void RecordOutcome(EntryOutcome entry) {
    if (hooks.report)
      hooks.report->WriteOutcome(entry, false);
    else
      result.outcomes.push_back(std::move(entry));
  }
  bool replaceAll = false, skipAll = false;
  bool selectedDirectory = false;
  size_t visited = 0;
  size_t deletedEntries = 0;
  size_t publishedEntries = 0, omissions = 0;
  std::string firstOmission;
  void NoteOmission(const std::string &error) {
    ++omissions;
    if (firstOmission.empty())
      firstOmission = error;
    AddError(error);
  }
  void NoteLimitedListing(const std::string &path) {
    NoteOmission(path + ": Only entries shown by the FTP server can be copied; folder completeness cannot be verified.");
  }
  void RecordPartialRoot(const std::string &dst, size_t publishedBefore,
                         const std::string &detail) {
    if (!selectedDirectory || publishedEntries == publishedBefore)
      return;
    ++result.partialCopies;
    if (!hooks.privateStaging)
      RecordOutcome({selectedSource, dst, "partial", detail});
  }
  TransferControl Control(const std::string &p, bool bytes = true) {
    return {[this] {
              Guard();
              return !hooks.proceed || hooks.proceed();
            },
            [&, p, bytes](uint64_t n) {
              if (hooks.progress)
                hooks.progress(p, bytes ? n : 0);
            }};
  }
  uint64_t Stage(const std::string &src, const FileEntry &expected, TempFile &temp,
             const TransferControl &control, Sha256 *digest = nullptr) {
    uint64_t bytes = 0;
    if (expected.sizeKnown && expected.size < 0)
      throw std::runtime_error("Source reported an invalid transfer size.");
    const uint64_t limit = expected.sizeKnown
                               ? uint64_t(expected.size)
                               : std::numeric_limits<uint64_t>::max();
    ReadFile(
        src,
        [&](const char *data, size_t size) {
          if (size > limit - bytes)
            throw std::runtime_error(
                "Source exceeded its declared size; destination was not committed.");
          temp.Write(data, size);
          if (digest)
            digest->Update(data, size);
          bytes += size;
        },
        control);
    FileEntry after;
    // Remote metadata is not a transactional snapshot. Reject observable
    // changes; never claim this detects same-size edits on servers without
    // timestamps.
    if (!StatFile(src, after, control) || !expected.SameRevision(after) ||
        (expected.sizeKnown && bytes != uint64_t(expected.size)))
      throw std::runtime_error(
          "Source changed during transfer; destination was not committed");
    temp.Finish(expected);
    return bytes;
  }
  bool Replace(const std::string &p) {
    if (skipAll) {
      ++result.skipped;
      return false;
    }
    if (replaceAll)
      return true;
    Decision d = hooks.conflict ? hooks.conflict(p) : Decision::Skip;
    if (d == Decision::Abort)
      throw Cancelled();
    if (d == Decision::ReplaceAll)
      replaceAll = true;
    if (d == Decision::SkipAll)
      skipAll = true;
    if (d == Decision::Skip || d == Decision::SkipAll) {
      ++result.skipped;
      return false;
    }
    return true;
  }
  void Copy(const std::string &src, const std::string &dst, int depth = 0,
            const FileEntry *discovered = nullptr) {
    if (depth > 128 || ++visited > 200000)
      throw std::runtime_error("Folder tree exceeds the transfer limit");
    if (InterruptedVolumes().Contains(src))
      throw std::runtime_error(src + ": " + kUnavailableVolumeText);
    const auto parent = ParentLocation(dst);
    struct DirectoryScope {
      std::map<std::string, NodeIdentity> &entries;
      std::string parent, destination;
      bool hadParent, hadDestination;
      ~DirectoryScope() {
        if (!hadDestination)
          entries.erase(destination);
        if (!hadParent)
          entries.erase(parent);
      }
    } scope{directories, parent, dst, directories.count(parent) != 0,
            directories.count(dst) != 0};
    auto control = Control(src);
    control.Check();
    RememberDirectory(parent);
    FileEntry source, target;
    if (!StatFile(src, source, control, discovered))
      throw std::runtime_error("Source no longer exists");
    if (depth == 0) {
      const auto selected = job.expectedRemoteSources.find(src);
      if (selected != job.expectedRemoteSources.end() && !MatchesRemoteSelection(selected->second, source))
        throw std::runtime_error("Selected network entry changed before copying: " + src);
    }
    if (hooks.fileSize && !source.directory && !source.link &&
        source.sizeKnown && source.size >= 0)
      hooks.fileSize(src, source.size);
    if (hooks.progress)
      hooks.progress(src, 0);
    if (source.link)
      throw std::runtime_error(
          "Links are not followed during archive/network transfers");
    bool exists = StatFile(dst, target, control);
    if (exists && target.link)
      throw std::runtime_error(
          "Destination is a link; choose a real folder or file");
    if (source.directory) {
      if (depth == 0)
        selectedDirectory = true;
      if (exists && !target.directory)
        throw std::runtime_error("Destination is not a folder");
      std::vector<std::string> omitted;
      ListingMethod initialMethod;
      auto entries = ListFiles(src, control, &omitted, &initialMethod);
      // Track completeness for this subtree independently of earlier siblings.
      const size_t omittedBefore = omissions;
      if (VisibleEntriesOnly(initialMethod))
        NoteLimitedListing(src);
      for (const auto &path : omitted) {
        NoteOmission(path + ": " + kUnavailableVolumeText);
        if (!hooks.privateStaging)
          RecordOutcome({path, ChildLocation(dst, LocationLeaf(path)),
                         "unprocessed", kUnavailableVolumeText});
      }
      // The folder itself is visible even if FTP lists zero children.
      // Preserve it, but never label its contents completely copied.
      if (entries.empty() && !omitted.empty())
        return;
      std::optional<PinnedDirectory> localParent, directory;
      if (!IsVirtual(dst)) {
        const auto &identity = directories.at(parent);
        localParent.emplace(parent, identity.device, identity.inode, true);
      }
      if (localParent) {
        localParent->Verify();
        const auto leaf = LocationLeaf(dst);
        if (!exists) {
          if (mkdirat(localParent->Descriptor(), leaf.c_str(), 0755))
            throw std::runtime_error(dst + ": " + strerror(errno));
          ++publishedEntries;
        }
        struct stat named;
        if (fstatat(localParent->Descriptor(), leaf.c_str(), &named,
                    AT_SYMLINK_NOFOLLOW) || !S_ISDIR(named.st_mode))
          throw std::runtime_error(dst + ": destination directory changed.");
        ClaimDirectory("local:" + std::to_string(named.st_dev) + ":" +
                       std::to_string(named.st_ino), src, dst);
        directories.emplace(dst, NodeIdentity{int32_t(named.st_dev), int64_t(named.st_ino)});
        Guard();
        directory.emplace(dst, named.st_dev, named.st_ino);
      } else {
        if (!exists) {
          MakeFolder(dst, control);
          ++publishedEntries;
          if (!StatFile(dst, target, control) || !target.directory || target.link)
            throw std::runtime_error("Created folder could not be verified: " + dst);
        }
        // A provider can resolve an alias to the existing folder yet return
        // the requested spelling from stat. Without identity, only merge a
        // folder if the parent actually lists that spelling.
        if (exists && target.providerIdentity.empty()) {
          const auto siblings = ListFiles(parent, control);
          const auto leaf = LocationLeaf(dst);
          if (std::none_of(siblings.begin(), siblings.end(), [&](const FileEntry &e) {
                return e.directory && !e.link && e.name == leaf;
              }))
            throw std::runtime_error("Cannot establish destination folder identity: " + dst);
        }
        auto key = target.providerIdentity.empty() ? TransferIdentity(dst) :
            ParseLocation(dst).Authority() + "#directory:" + target.providerIdentity;
        ClaimDirectory(key, src, dst);
      }
      int skippedBefore = result.skipped;
      for (const auto &e : entries)
        Copy(e.path, ChildLocation(dst, e.name), depth + 1, &e);
      control.Check();
      const bool ordinaryCopy = job.action == Action::Copy &&
          !job.copyForArchiveMove && !hooks.privateStaging;
      std::string limitations;
      if (!exists && !IsVirtual(dst)) {
        if (!directory->SetCopyMode(source.permissions & 0777, ordinaryCopy))
          limitations = kNtfsPermissionNotice;
        if (source.modifiedKnown || source.modified > 0) {
          struct timespec times[2] = {
              {0, UTIME_OMIT}, {source.modified, source.ModifiedNanoseconds()}};
          if (futimens(directory->Descriptor(), times))
            throw std::runtime_error(strerror(errno));
        }
      }
      if (directory && !FinalizeDirectory(*directory, *localParent,
                                          [&] { control.Check(); }, ordinaryCopy)) {
        if (!limitations.empty())
          limitations += " ";
        limitations += kNtfsDurabilityNotice;
      }
      if (!limitations.empty()) {
        result.copyLimitations = true;
        RecordOutcome({src, dst, "filesystem limitations", std::move(limitations)});
      }
      std::vector<std::string> nowOmitted;
      ListingMethod finalMethod;
      auto current = ListFiles(src, control, &nowOmitted, &finalMethod);
      if (!VisibleEntriesOnly(initialMethod) && VisibleEntriesOnly(finalMethod))
        NoteLimitedListing(src);
      std::sort(omitted.begin(), omitted.end());
      std::sort(nowOmitted.begin(), nowOmitted.end());
      if (omitted != nowOmitted || !SameListing(entries, current))
        throw std::runtime_error("Source folder changed during transfer: " + src);
      if (result.skipped == skippedBefore && omissions == omittedBefore) {
        if (hooks.fileDone)
          hooks.fileDone(src, dst);
      }
      return;
    }
    if (exists && target.directory)
      throw std::runtime_error("Destination is a folder");
    if (!IsVirtual(dst)) {
      struct stat before {};
      if (exists && lstat(dst.c_str(), &before))
        throw std::runtime_error("Destination changed before staging.");
      TempFile temp(LocationLeaf(dst), src, dst);
      Stage(src, source, temp, control);
      control.Check();
      struct stat after {};
      const bool nowExists = lstat(dst.c_str(), &after) == 0;
      if ((!nowExists && errno != ENOENT) || nowExists != exists ||
          (exists && !SameLocal(before, after)))
        throw std::runtime_error(
            "Destination changed during transfer; it was left untouched");
      Hooks local = hooks;
      local.report.reset(); // This internal result drives publication error handling.
      local.proceed = control.proceed;
      local.fileDone = {};
      local.fileFailed = {};
      local.fileSize = {};
      // The temporary source is private, but the destination is public
      // unless the containing job is itself staging an archive operation.
      local.copyConflict = {};
      // Local Run has its own cancellation type. Let its proceed callback
      // drive cancellation and translate our conflict abort at the boundary.
      local.progress = [](const std::string &, uint64_t) {};
      local.conflict = [&](const std::string &p) {
        try {
          return Replace(p) ? Decision::Replace : Decision::Skip;
        } catch (const Cancelled &) {
          return Decision::Abort;
        }
      };
      Job publish{Action::Copy, {temp.path}, ParentLocation(dst)};
      publish.expectedDestination = directories.at(publish.destination);
      publish.copyForArchiveMove = job.copyForArchiveMove;
      auto done = Run(publish, local);
      result.copyLimitations |= done.copyLimitations;
      for (const auto &entry : done.outcomes)
        if (entry.status == "copied" || entry.status == "partial" ||
            entry.status == "published-flush-failed" ||
            entry.status == "published-recovery-pending")
          ++publishedEntries;
      // Replace already counted skipped conflicts. Completion is counted by
      // RunVirtual once per selected root, not once per published child.
      // A containing archive job must not report its private staging paths.
      if (!hooks.privateStaging) {
        for (auto &entry : done.outcomes) {
          entry.source = src;
          entry.destination = dst;
          RecordOutcome(std::move(entry));
        }
      }
      if (!done.interruptedCopyFolder.empty())
        throw RemovableCopyInterrupted(done.errors.empty() ?
            "Removable destination flush interrupted." : done.errors.front(),
            done.interruptedCopyFolder);
      if (done.cancelled)
        throw Cancelled();
      if (!done.errors.empty())
        throw std::runtime_error(done.errors.front());
      if (done.completed) {
        if (hooks.fileDone)
          hooks.fileDone(src, dst);
      }
      return;
    }
    if (ParseLocation(dst).Archive())
      throw std::runtime_error(
          "This direct transfer cannot update an archive. Copy entries to a "
          "regular folder to edit them.");
    if (exists && !Replace(dst))
      return;
    TempFile temp(LocationLeaf(dst), src, dst);
    Sha256 expected;
    const auto stagedBytes = Stage(src, source, temp, Control(src, false), &expected);
    EntryOutcome entry{src, dst, "upload not published", "", UploadVerification{}};
    try {
      PublishUpload(src, dst, target, exists, temp, expected, stagedBytes,
                    control, entry);
    } catch (...) {
      RecordOutcome(std::move(entry));
      throw;
    }
    RecordOutcome(std::move(entry));
  }
  void PublishUpload(const std::string &src, const std::string &dst,
                     const FileEntry &target, bool exists, const TempFile &temp,
                     Sha256 &expected, uint64_t stagedBytes,
                     const TransferControl &control, EntryOutcome &entry) {
    auto &receipt = *entry.upload;
    RecoveryRecord recovery("upload", ParentLocation(dst), ".listi-", src, dst);
    const auto &staged = recovery.Stage();
    const auto &backup = recovery.Backup();
    const auto expectedDigest = expected.Finish();
    std::string originalDigest;
    FileEntry stagedRevision;
    bool stagedExists = false, backupExists = false, committed = false,
         commitAttempted = false;
    try {
      FileEntry check;
      if (StatFile(staged, check, control) || StatFile(backup, check, control))
        throw std::runtime_error("Temporary transfer name collision");
      // Record the bytes an acknowledged backup must still contain before
      // rollback or cleanup. A name, timestamp and size alone are not evidence.
      if (exists)
        originalDigest = RemoteDigest(dst, control);
      recovery.Advance(RecoveryPhase::Writing);
      stagedExists = true;
      UploadFile(temp.path, staged, control);
      receipt.bytesWritten = stagedBytes;
      control.Check();
      if (!StatFile(staged, stagedRevision, control) || stagedRevision.directory || stagedRevision.link ||
          (stagedRevision.sizeKnown && (stagedRevision.size < 0 ||
                                        uint64_t(stagedRevision.size) != stagedBytes)))
        throw std::runtime_error(
            "Uploaded file is missing or incomplete; destination was not replaced");
      receipt.destinationSizeChecked = stagedRevision.sizeKnown;
      uint64_t received = 0;
      Sha256 actual;
      ReadFile(staged, [&](const char *data, size_t n) {
        if (n > stagedBytes - received)
          throw std::runtime_error("Uploaded file is larger than the source");
        actual.Update(data, n);
        received += n;
      }, Control(src, false));
      if (received != stagedBytes || actual.Finish() != expectedDigest)
        throw std::runtime_error(
            "Uploaded content differs from the source; destination was not replaced.");
      if (!StatFile(staged, check, control) || !stagedRevision.SameRevision(check))
        throw std::runtime_error(
            "Staged upload changed during verification; destination was not replaced.");
      receipt.contentVerified = true;
      recovery.Advance(RecoveryPhase::Ready);
      bool now = StatFile(dst, check, control);
      if (now != exists || (now && (!target.SameRevision(check) ||
                                    RemoteDigest(dst, control) != originalDigest)))
        throw std::runtime_error(
            "Destination changed during transfer; it was left untouched");
      recovery.Advance(RecoveryPhase::Publishing);
      if (exists) {
        // The server may rename successfully and lose the acknowledgement.
        // Track the recovery name before sending the mutation, not afterward.
        backupExists = true;
        RenameFile(dst, backup, control);
      }
      control.Check();
      commitAttempted = true;
      receipt.publication = Publication::Unknown;
      RenameFile(staged, dst, control);
      stagedExists = false;
      committed = true;
      ++publishedEntries;
      receipt.publication = Publication::Published;
      recovery.Advance(RecoveryPhase::Published);
      entry.status = "completed";
      entry.detail = "Upload content verified.";
      if (backupExists) {
        if (RemoteDigest(backup, control) != originalDigest)
          throw std::runtime_error("Original backup changed; retained for manual inspection.");
        RemoveFile(backup, false, control);
        backupExists = false;
      }
      recovery.Retire();
      if (hooks.fileDone)
        hooks.fileDone(src, dst);
    } catch (...) {
      auto failure = std::current_exception();
      try {
        std::rethrow_exception(failure);
      } catch (const Cancelled &) {
        result.cancelled = true;
      } catch (...) {
      }
      NetworkCleanupBudget budget;
      TransferControl cleanup{[&] {
        if (hooks.proceed && !hooks.proceed())
          result.cancelled = true;
        return !result.cancelled;
      }, {}, &budget};
      // No network work after explicit cancellation. SMB's synchronous library
      // and session lock have no total deadline; defer its recovery on error.
      const auto provider = ParseLocation(dst);
      const bool ftp = provider.scheme == "ftp" || provider.scheme == "ftps";
      const bool reconcile = !result.cancelled && provider.scheme != "smb";
      auto matches = [&](const std::string &path, const FileEntry &file,
                         const std::string &digest) {
        return !file.directory && !file.link && file.sizeKnown && file.size >= 0 &&
               uint64_t(file.size) <= budget.remaining &&
               RemoteDigest(path, cleanup) == digest;
      };
      // Prefer restoring the original if either check would consume the budget.
      // FTP RNTO cannot restore without clobbering a concurrent writer.
      // Leave the backup and its journal for explicit recovery instead.
      if (reconcile && backupExists && !ftp)
        try {
          FileEntry current, saved;
          if (!StatFile(backup, saved, cleanup)) {
            backupExists = false;
          } else if (target.SameRevision(saved) && matches(backup, saved, originalDigest) &&
                     !StatFile(dst, current, cleanup)) {
            RenameFile(backup, dst, cleanup);
            backupExists = false;
          }
        } catch (const Cancelled &) {
          result.cancelled = true;
        } catch (...) {
        }
      if (reconcile && stagedExists)
        try {
          FileEntry remote;
          if (!StatFile(staged, remote, cleanup)) {
            // After STOR, some FTP services hide even an exact path probe.
            // Keep the recovery record until our own removal is acknowledged.
            stagedExists = ftp;
          } else if (receipt.contentVerified && stagedRevision.SameRevision(remote) &&
                   matches(staged, remote, expectedDigest)) {
            RemoveFile(staged, false, cleanup);
            stagedExists = StatFile(staged, remote, cleanup);
          }
        } catch (const Cancelled &) {
          result.cancelled = true;
        } catch (...) {
        }
      if (committed)
        entry.status = "published; cleanup incomplete";
      else if (commitAttempted)
        entry.status = "outcome unknown";
      if (stagedExists)
        receipt.recoveryPaths.push_back(staged);
      if (backupExists)
        receipt.recoveryPaths.push_back(backup);
      if (backupExists || stagedExists || (commitAttempted && !committed)) {
        std::string message = committed ? "New file committed; cleanup failed. "
                                        : "Transfer interrupted. ";
        if (commitAttempted && !committed)
          message += "Publication was not acknowledged. Check " + dst + ". ";
        if (backupExists)
          message +=
              "Original file recovery path (verify before use): " + backup +
              ". ";
        if (stagedExists)
          message += "Temporary transfer may remain at " + staged + ". ";
        try {
          std::rethrow_exception(failure);
        } catch (const Cancelled &) {
          message += "Operation was cancelled.";
        } catch (const std::exception &e) {
          message += e.what();
        }
        recovery.NoteFailure(result.cancelled);
        throw std::runtime_error(message);
      }
      recovery.Retire();
      std::rethrow_exception(failure);
    }
  }
  void DeleteTree(const std::string &p, const FileEntry &expected, int depth = 0) {
    if (depth > 128 || ++visited > 200000)
      throw std::runtime_error("Folder tree exceeds deletion limit");
    auto c = Control(p);
    c.Check();
    FileEntry e;
    if (!StatFile(p, e, c))
      throw std::runtime_error("Entry no longer exists");
    if (!MatchesRemoteSelection(expected, e))
      throw std::runtime_error("Entry changed before deletion: " + p);
    const bool directory = expected.directory && !expected.link;
    if (directory)
      for (const auto &child : ListFiles(p, c))
        DeleteTree(child.path, child, depth + 1);
    RemoveFile(p, directory, c);
    ++deletedEntries;
    RecordOutcome({p, "", "deleted", ""});
  }
};
} // namespace
Result RunVirtual(const Job &job, const Hooks &hooks) {
  Runner runner{hooks, job};
  try {
    if (job.expectedDestination.device >= 0) {
      const auto &folder = job.destinationFolder.empty()
                               ? job.destination
                               : job.destinationFolder;
      if (!IsVirtual(folder))
        runner.directories[folder] = job.expectedDestination;
    }
    runner.Guard();
    if (job.action == Action::Trash || job.action == Action::Restore)
      throw std::runtime_error("Trash and Restore are available only on local "
                               "supported volumes. Nothing was deleted.");
    if (ParseLocation(job.destination).Archive() &&
        (job.action == Action::Copy || job.action == Action::Move ||
         job.action == Action::MakeDirectory || job.action == Action::Rename))
      throw std::runtime_error(
          "Use the archive editing actions to change a writable archive.");
    if (job.action == Action::MakeDirectory) {
      MakeFolder(job.destination, runner.Control(job.destination));
      runner.result.completed = 1;
      return runner.result;
    }
    for (auto &source : job.sources) {
      const size_t publishedBefore = runner.publishedEntries;
      std::string dst;
      try {
        runner.selectedSource = source;
        runner.selectedDirectory = false;
        runner.firstOmission.clear();
        if (InterruptedVolumes().ContainsResolved(source))
          throw std::runtime_error(kUnavailableVolumeText);
        auto control = runner.Control(source);
        int skippedBefore = runner.result.skipped;
        if (LocationLeaf(source).empty())
          throw std::runtime_error(
              "A location root cannot be transferred or deleted");
        control.Check();
        dst = (job.action == Action::Rename)
                       ? job.destination
                       : ChildLocation(job.destination, LocationLeaf(source));
        const auto canonicalSource = TransferIdentity(source);
        const auto canonicalDestination = TransferIdentity(dst);
        if (job.action == Action::Delete) {
          if (ParseLocation(source).Archive())
            throw std::runtime_error(
                "Use Delete from archive to remove entries from a writable archive");
          const auto expected = job.expectedRemoteSources.find(source);
          if (expected == job.expectedRemoteSources.end())
            throw std::runtime_error("No confirmed network entry. Refresh and select it again.");
          const auto deletedBefore = runner.deletedEntries;
          try {
            runner.DeleteTree(source, expected->second);
          } catch (...) {
            if (runner.deletedEntries != deletedBefore) {
              ++runner.result.partialDeletions;
              runner.RecordOutcome({source, "", "partially deleted",
                                    "Deletion stopped after removing entries from this folder."});
            }
            throw;
          }
        } else if (job.action == Action::Move || job.action == Action::Rename) {
          auto from = ParseLocation(source), to = ParseLocation(dst);
          if (from.Archive() || from.Local() ||
              from.Authority() != to.Authority())
            throw std::runtime_error(
                "Use Copy for transfers between local, archive and network "
                "locations. The source is retained.");
          FileEntry target;
          if (StatFile(dst, target, control))
            throw std::runtime_error(
                "Rename/move target already exists; it was not replaced");
          if (canonicalDestination == canonicalSource ||
              canonicalDestination.compare(0, canonicalSource.size() + 1,
                                           canonicalSource + "/") == 0)
            throw std::runtime_error("Cannot move a folder into itself");
          if (from.scheme == "ftp" || from.scheme == "ftps")
            throw std::runtime_error(
                "FTP cannot rename without risking replacement of a concurrently created target. "
                "The source was retained.");
          const auto selected = job.expectedRemoteSources.find(source);
          if (selected == job.expectedRemoteSources.end())
            throw std::runtime_error("No confirmed network entry. Refresh and select it again.");
          FileEntry current;
          if (!StatFile(source, current, control) || !MatchesRemoteSelection(selected->second, current))
            throw std::runtime_error("Selected network entry changed or disappeared: " + source);
          RenameFile(source, dst, control);
          if (hooks.fileDone)
            hooks.fileDone(source, dst);
        } else {
          if (canonicalDestination == canonicalSource ||
              canonicalDestination.compare(0, canonicalSource.size() + 1,
                                           canonicalSource + "/") == 0)
            throw std::runtime_error(
                "Cannot copy an entry onto itself or its descendants");
          runner.Copy(source, dst);
        }
        if (!runner.firstOmission.empty()) {
          runner.RecordPartialRoot(dst, publishedBefore, runner.firstOmission);
          if (runner.publishedEntries == publishedBefore && !hooks.privateStaging)
            runner.RecordOutcome({source, dst, "failed",
                                  runner.firstOmission});
          if (hooks.fileFailed)
            hooks.fileFailed(source, runner.firstOmission);
        } else if (runner.result.skipped == skippedBefore) {
          ++runner.result.completed;
        } else {
          runner.RecordPartialRoot(dst, publishedBefore, "Some entries were skipped.");
        }
      } catch (const RemovableCopyInterrupted &e) {
        runner.RecordPartialRoot(dst, publishedBefore, e.what());
        runner.result.interruptedCopyFolder = e.folder.empty() ? job.destination : e.folder;
        runner.AddError(source + ": " + e.what());
        if (hooks.fileFailed)
          hooks.fileFailed(source, e.what());
        break;
      } catch (const Cancelled &) {
        runner.RecordPartialRoot(dst, publishedBefore, "Cancelled after publishing part of the folder.");
        throw;
      } catch (const std::exception &e) {
        runner.RecordPartialRoot(dst, publishedBefore, e.what());
        runner.AddError(source + ": " + e.what());
        if (hooks.fileFailed)
          hooks.fileFailed(source, e.what());
        if (runner.result.cancelled)
          break;
      }
    }
  } catch (const Cancelled &) {
    runner.result.cancelled = true;
  } catch (const std::exception &e) {
    runner.AddError(e.what());
  }
  return runner.result;
}
} // namespace Listi
