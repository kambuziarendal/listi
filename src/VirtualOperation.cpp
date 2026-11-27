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
#include <set>
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
         (!selected.permissionsKnown || (current.permissionsKnown && selected.permissions == current.permissions)) &&
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
  if (location.Local() || location.Archive())
    return location.URL();
  return location.CanonicalAuthority() + EncodePath(location.path);
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
  struct RemoteDirectory {
    FileEntry revision;
    std::optional<std::map<std::string, std::string>> exactFolders;
  };
  // The job destination survives across roots. Descendant guards live only
  // while traversing that subtree. Never cache a listing without an identity.
  std::map<std::string, RemoteDirectory> remoteDirectories;
  std::string unverifiedDirectory;
  void VerifyRemoteDirectories(TransferControl control) {
    control.requireExactName = true;
    for (auto &[path, expected] : remoteDirectories) {
      FileEntry current;
      if (!StatFile(path, current, control) || !current.directory || current.link ||
          expected.revision.providerIdentity != current.providerIdentity) {
        expected.exactFolders.reset();
        throw std::runtime_error("Remote destination folder changed: " + path);
      }
      if (!expected.revision.SameModification(current)) {
        expected.exactFolders.reset();
        expected.revision = current;
      }
      if (current.providerIdentity.empty() && unverifiedDirectory.empty())
        unverifiedDirectory = path;
    }
  }
  void BindRemoteParent(const std::string &parent, const TransferControl &control) {
    if (!remoteDirectories.count(parent)) {
      FileEntry folder;
      if (!StatFile(parent, folder, control) || !folder.directory || folder.link)
        throw std::runtime_error("Remote destination folder unavailable: " + parent);
      remoteDirectories.emplace(parent, RemoteDirectory{folder, {}});
    }
    VerifyRemoteDirectories(control);
  }
  std::string DirectoryLimitation(const std::string &path) const {
    return "The server provides no stable identity for " + path +
           "; folder replacement cannot be excluded.";
  }
  // Keep all source ancestors in scope until their local subtree is complete.
  std::map<std::string, FileEntry> downloadFolders;
  void VerifyDownloadAccess(const TransferControl &control) {
    for (const auto &[path, expected] : downloadFolders) {
      FileEntry now;
      if (!StatFile(path, now, control) || !now.directory || now.link ||
          now.providerIdentity != expected.providerIdentity ||
          (expected.permissionsKnown && !now.permissionsKnown) ||
          (expected.permissions & ~now.permissions & 0777))
        throw std::runtime_error("Source folder changed before setting destination permissions or publishing more entries: " + path);
    }
  }
  bool ExactRemoteFolder(const std::string &parent, const std::string &leaf, const FileEntry &target,
                         const TransferControl &control) {
    auto &guard = remoteDirectories.at(parent);
    if (guard.exactFolders) {
      const auto prior = guard.exactFolders->find(leaf);
      if (prior != guard.exactFolders->end() && !target.providerIdentity.empty() &&
          prior->second == target.providerIdentity)
        return true;
    }
    std::map<std::string, std::string> names;
    for (const auto &entry : ListFiles(parent, control))
      if (entry.directory && !entry.link)
        names.emplace(entry.name, entry.providerIdentity);
    const auto found = names.find(leaf);
    const bool exact = found != names.end() && found->second == target.providerIdentity;
    if (!guard.revision.providerIdentity.empty())
      guard.exactFolders = std::move(names);
    return exact;
  }
  // Keep claims across selected roots, unlike the scoped directory guards.
  std::map<std::string, std::string> directoryOwners;
  void ClaimDirectory(const std::string &key, const std::string &src, const std::string &dst) {
    const auto inserted = directoryOwners.emplace(key, src);
    if (!inserted.second && inserted.first->second != src)
      throw std::runtime_error("Two source folders resolve to the same destination: " + dst);
  }
  struct FileClaims {
    std::set<std::string> reserved;
    std::optional<std::set<std::string>> replaceable;
  };
  std::map<std::string, FileClaims> fileClaims;
  void CheckFileDestination(const std::string &dst, bool exists, const TransferControl &control) {
    const auto parent = ParentLocation(dst);
    const auto leaf = LocationLeaf(dst);
    auto &claims = fileClaims[parent];
    if (claims.reserved.count(leaf))
      throw std::runtime_error("Another selected file already claimed this destination: " + dst);
    if (exists) {
      // Stat may echo the spelling requested even when it resolved an alias.
      // Enumerate once, when replacement first matters. A newly created name
      // was absent at lookup; it must never become replaceable later this job.
      if (!claims.replaceable) {
        std::set<std::string> names;
        if (IsVirtual(parent)) {
          for (const auto &entry : ListFiles(parent, control))
            names.insert(entry.name);
        } else {
          for (const auto &entry : std::filesystem::directory_iterator(parent)) {
            control.Check();
            names.insert(entry.path().filename().string());
          }
        }
        for (const auto &reserved : claims.reserved) {
          if (!names.erase(reserved))
            throw std::runtime_error("Cannot distinguish a previous destination name in " +
                                     parent + ". Use separate names.");
        }
        claims.replaceable = std::move(names);
      }
      if (!claims.replaceable->count(leaf))
        throw std::runtime_error("Destination aliases another name or was not listed separately: " +
                                 dst + ". Use separate names.");
    }
  }
  void ReservePublishedName(const std::string &dst) {
    auto &claims = fileClaims.at(ParentLocation(dst));
    const auto leaf = LocationLeaf(dst);
    claims.reserved.insert(leaf);
    if (claims.replaceable)
      claims.replaceable->erase(leaf);
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
  SftpSessions sftpSessions;
  TransferControl Control(const std::string &p, bool bytes = true) {
    const auto version = job.expectedArchiveSets.empty() ? job.expectedArchiveSets.end()
        : job.expectedArchiveSets.find(ParseLocation(p).archive);
    return {[this] {
              Guard();
              return !hooks.proceed || hooks.proceed();
            },
            [&, p, bytes](uint64_t n) {
              if (hooks.progress)
                hooks.progress(p, bytes ? n : 0);
            }, nullptr, version == job.expectedArchiveSets.end() ? nullptr : &version->second, nullptr, &sftpSessions};
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
  // Walk only projected paths which already exist. A missing directory
  // cannot contain an alias; a later arrival is checked again at publication.
  void CheckExistingOutputs(const std::string &src, const std::string &dst,
                            const TransferControl &control, size_t &count, unsigned depth = 0) {
    control.Check();
    struct stat target;
    if (lstat(dst.c_str(), &target)) {
      if (errno == ENOENT)
        return;
      throw std::runtime_error("Cannot check destination: " + dst);
    }
    hooks.checkOverwrite(dst, target);
    if (!S_ISDIR(target.st_mode))
      return;
    FileEntry from;
    if (!StatFile(src, from, control))
      throw std::runtime_error("Source is unavailable: " + src);
    if (!from.directory || from.link)
      return;
    if (depth >= 128)
      throw std::runtime_error("Folder tree exceeds the transfer limit");
    for (const auto &child : ListFiles(src, control)) {
      if (++count > 200000)
        throw std::runtime_error("Folder tree exceeds the transfer limit");
      CheckExistingOutputs(child.path, ChildLocation(dst, child.name), control, count, depth + 1);
    }
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
    if (IsVirtual(parent)) {
      BindRemoteParent(parent, control);
    } else {
      RememberDirectory(parent);
    }
    struct RemoteScope {
      decltype(remoteDirectories) &guards;
      const std::string &path;
      ~RemoteScope() { guards.erase(path); }
    } remoteScope{remoteDirectories, dst};
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
      struct SourceScope {
        decltype(downloadFolders) &folders;
        const std::string &path;
        ~SourceScope() { folders.erase(path); }
      } sourceScope{downloadFolders, src};
      if (localParent && IsVirtual(src) && !ParseLocation(src).Archive()) {
        downloadFolders.emplace(src, source);
        VerifyDownloadAccess(control);
      }
      if (localParent) {
        localParent->Verify();
        const auto leaf = LocationLeaf(dst);
        if (exists) {
          localParent->RequireExactLeaf(leaf);
        } else {
          if (mkdirat(localParent->Descriptor(), leaf.c_str(), 0700))
            throw std::runtime_error(dst + ": " + strerror(errno));
          ++publishedEntries;
        }
        struct stat named;
        if (fstatat(localParent->Descriptor(), leaf.c_str(), &named,
                    AT_SYMLINK_NOFOLLOW) || !S_ISDIR(named.st_mode))
          throw std::runtime_error(dst + ": destination directory changed.");
        if (exists && hooks.checkOverwrite)
          hooks.checkOverwrite(dst, named);
        // Do not populate a private source into a more accessible folder,
        // even on a retry or a filesystem that ignored the creation mode.
        if ((named.st_mode & 0077 & ~source.permissions) != 0)
          throw std::runtime_error(dst + ": destination folder grants access not present on the source. "
                                  "Nothing was added here. Choose a separate destination.");
        ClaimDirectory("local:" + std::to_string(named.st_dev) + ":" +
                       std::to_string(named.st_ino), src, dst);
        directories.emplace(dst, NodeIdentity{int32_t(named.st_dev), int64_t(named.st_ino)});
        Guard();
        directory.emplace(dst, named.st_dev, named.st_ino);
        if (!hooks.privateStaging && !ParseLocation(src).Archive()) {
          std::string note = exists ? "Already present on this attempt; existing permissions retained." :
                                      "This job created this folder with restricted access.";
          if (exists && (named.st_mode & 0777) != (source.permissions & 0777)) {
            result.copyLimitations = true;
            note += " Different source permissions were not applied.";
          }
          RecordOutcome({src, dst, exists ? "existing folder" : "folder created", std::move(note)});
        }
      } else {
        if (!exists) {
          VerifyRemoteDirectories(control);
          MakeFolder(dst, control);
          ++publishedEntries;
          if (!StatFile(dst, target, control) || !target.directory || target.link)
            throw std::runtime_error("Created folder could not be verified: " + dst);
        }
        // A provider can resolve an alias to the existing folder yet return
        // the requested spelling from stat. Even a stable ID cannot prove
        // that the user intended this spelling on a subsequent job.
        if (exists && !ExactRemoteFolder(parent, LocationLeaf(dst), target, control))
          throw std::runtime_error("Cannot establish destination folder identity: " + dst);
        remoteDirectories.emplace(dst, RemoteDirectory{target, {}});
        VerifyRemoteDirectories(control);
        auto key = target.providerIdentity.empty() ? TransferIdentity(dst) :
            ParseLocation(dst).CanonicalAuthority() + "#directory:" + target.providerIdentity;
        ClaimDirectory(key, src, dst);
      }
      int skippedBefore = result.skipped;
      for (const auto &e : entries)
        Copy(e.path, ChildLocation(dst, e.name), depth + 1, &e);
      control.Check();
      std::vector<std::string> nowOmitted;
      ListingMethod finalMethod;
      auto current = ListFiles(src, control, &nowOmitted, &finalMethod);
      if (!VisibleEntriesOnly(initialMethod) && VisibleEntriesOnly(finalMethod))
        NoteLimitedListing(src);
      std::sort(omitted.begin(), omitted.end());
      std::sort(nowOmitted.begin(), nowOmitted.end());
      if (omitted != nowOmitted || !SameListing(entries, current))
        throw std::runtime_error("Source folder changed during transfer: " + src);
      const bool ordinaryCopy = job.action == Action::Copy &&
          !job.copyForArchiveMove && !hooks.privateStaging;
      std::string limitations;
      VerifyDownloadAccess(control);
      if (!exists && !IsVirtual(dst)) {
        if (source.permissions & 0077) {
          // A private staging folder must not be opened using stale access
          // bits if the source became private while a child was transferred.
          FileEntry finalSource;
          if (!StatFile(src, finalSource, control) || !source.SameRevision(finalSource) ||
              (source.permissions & 0777) != (finalSource.permissions & 0777))
            throw std::runtime_error("Source folder changed before setting destination permissions: " + src);
        }
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
      VerifyRemoteDirectories(control);
      if (result.skipped == skippedBefore && omissions == omittedBefore) {
        if (IsVirtual(dst) && !unverifiedDirectory.empty() && depth == 0) {
          result.copyLimitations = true;
          RecordOutcome({src, dst, "filesystem limitations",
                         "Entries were delivered by path. " + DirectoryLimitation(unverifiedDirectory)});
        }
        if (hooks.fileDone)
          hooks.fileDone(src, dst);
      }
      return;
    }
    if (exists && target.directory)
      throw std::runtime_error("Destination is a folder");
    CheckFileDestination(dst, exists, control);
    if (!IsVirtual(dst)) {
      struct stat before {};
      if (exists && lstat(dst.c_str(), &before))
        throw std::runtime_error("Destination changed before staging.");
      // The first conflict still goes through the normal local publisher.
      // Subsequent ordinary files need no scratch space for an already
      // chosen Skip All. Never bypass name claims or protected inputs.
      if (exists && skipAll && IsVirtual(src) && S_ISREG(before.st_mode)) {
        control.Check();
        if (hooks.checkOverwrite)
          hooks.checkOverwrite(dst, before);
        ++result.skipped;
        if (!hooks.privateStaging)
          RecordOutcome({src, dst, "skipped", ""});
        return;
      }
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
      // Conflict questions and the final rename happen inside the local publisher.
      if (control.archiveVersion)
        publish.expectedArchiveSets.emplace(ParseLocation(src).archive, *control.archiveVersion);
      publish.expectedDestination = directories.at(publish.destination);
      publish.copyForArchiveMove = job.copyForArchiveMove;
      if (!downloadFolders.empty()) {
        publish.checkDestinationWindow = [&] { VerifyDownloadAccess(control); };
        VerifyDownloadAccess(control);
      }
      auto done = Run(publish, local);
      result.copyLimitations |= done.copyLimitations;
      for (const auto &entry : done.outcomes)
        if (entry.status == "copied" || entry.status == "partial" ||
            entry.status == "published-flush-failed" ||
            entry.status == "published-recovery-pending") {
          ReservePublishedName(dst);
          ++publishedEntries;
        }
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
      VerifyRemoteDirectories(control);
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
        VerifyRemoteDirectories(control);
        backupExists = true;
        RenameFile(dst, backup, control);
      }
      control.Check();
      // Reserve before the server mutation: a lost acknowledgement still
      // leaves a possible delivery that a later root must not overwrite.
      VerifyRemoteDirectories(control);
      ReservePublishedName(dst);
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
      for (const auto &[path, folder] : remoteDirectories)
        if (folder.revision.providerIdentity.empty()) {
          result.copyLimitations = true;
          entry.detail += " Delivered by path. " + DirectoryLimitation(path);
          break;
        }
      if (backupExists) {
        if (RemoteDigest(backup, control) != originalDigest)
          throw std::runtime_error("Original backup changed; retained for manual inspection.");
        VerifyRemoteDirectories(control);
        RemoveFile(backup, false, control);
        backupExists = false;
      }
      VerifyRemoteDirectories(control);
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
      }, {}, &budget, nullptr, nullptr, &sftpSessions};
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
            VerifyRemoteDirectories(cleanup);
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
            VerifyRemoteDirectories(cleanup);
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
  FileEntry PrepareMove(const std::string &source, const std::string &dst,
                        const TransferControl &control) {
    auto from = ParseLocation(source), to = ParseLocation(dst);
    if (from.Archive() || from.Local() ||
        from.CanonicalAuthority() != to.CanonicalAuthority())
      throw std::runtime_error(
          "Use Copy for transfers between local, archive and network "
          "locations. The source is retained.");
    BindRemoteParent(ParentLocation(dst), control);
    FileEntry target;
    if (StatFile(dst, target, control))
      throw std::runtime_error(
          "Rename/move target already exists; it was not replaced");
    const auto canonicalSource = TransferIdentity(source);
    const auto canonicalDestination = TransferIdentity(dst);
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
    return current;
  }
  std::set<std::string> unconfirmedMoves;
  void MoveNetworkEntry(const std::string &source, const std::string &dst,
                        const FileEntry &selected, TransferControl control) {
    const auto key = TransferIdentity(source);
    if (unconfirmedMoves.count(key))
      throw std::runtime_error("Previous move unconfirmed; no further move requested for " + source);
    bool submitted = false;
    control.mutationSubmitted = &submitted;
    VerifyRemoteDirectories(control);
    std::string detail;
    try {
      RenameFile(source, dst, control);
    } catch (const std::exception &error) {
      if (!submitted)
        throw;
      control.mutationSubmitted = nullptr;
      FileEntry original, target;
      bool moved = false, retained = false;
      try {
        VerifyRemoteDirectories(control);
        const bool sourcePresent = StatFile(source, original, control);
        const bool targetPresent = StatFile(dst, target, control);
        VerifyRemoteDirectories(control);
        if (!selected.providerIdentity.empty()) {
          moved = !sourcePresent && targetPresent && selected.SameRevision(target);
          retained = sourcePresent && !targetPresent && selected.SameRevision(original);
        }
      } catch (const Cancelled &) {
        result.cancelled = true;
      } catch (const std::exception &) {
        // An incomplete pair of observations cannot establish either outcome.
      }
      if (!moved) {
        std::string reason = retained ? "The same entry remains at the source." :
            "Move unconfirmed. Inspect both paths before selecting again.";
        if (!retained)
          unconfirmedMoves.insert(key);
        RecordOutcome({source, dst, retained ? "not moved" : "move unconfirmed", reason});
        if (result.cancelled)
          throw Cancelled();
        throw std::runtime_error(reason + " " + error.what());
      }
      detail = "Reply lost; fresh lookups confirmed the selected entry at the destination only.";
    }
    try {
      VerifyRemoteDirectories(control);
    } catch (const std::exception &error) {
      unconfirmedMoves.insert(key);
      const std::string reason =
          "Server reported the rename, but the destination folder could not be verified. Inspect both paths.";
      RecordOutcome({source, dst, "move destination unconfirmed", reason});
      if (dynamic_cast<const Cancelled *>(&error))
        throw;
      throw std::runtime_error(reason + " " + error.what());
    }
    if (!unverifiedDirectory.empty()) {
      result.copyLimitations = true;
      detail += (detail.empty() ? "" : " ") + DirectoryLimitation(unverifiedDirectory);
    }
    RecordOutcome({source, dst, "moved", detail});
    result.completedRoots.emplace_back(source, dst);
    if (hooks.fileDone)
      hooks.fileDone(source, dst);
  }
  struct DeleteFolder {
    const std::string &path;
    const FileEntry &entry;
    const DeleteFolder *parent;
  };
  void VerifyDeleteFolders(const DeleteFolder *folder, const TransferControl &control) {
    for (; folder; folder = folder->parent) {
      FileEntry current;
      if (!StatFile(folder->path, current, control) || !current.directory || current.link ||
          current.providerIdentity != folder->entry.providerIdentity)
        throw std::runtime_error("Folder changed during deletion: " + folder->path);
      // Child removals can legitimately change directory size and timestamps.
    }
  }
  void DeleteTree(const std::string &p, const FileEntry &expected, int depth = 0,
                  const DeleteFolder *parent = nullptr) {
    if (depth > 128 || ++visited > 200000)
      throw std::runtime_error("Folder tree exceeds deletion limit");
    auto c = Control(p);
    c.requireExactName = true;
    c.Check();
    FileEntry e;
    if (!StatFile(p, e, c))
      throw std::runtime_error("Entry no longer exists");
    if (!MatchesRemoteSelection(expected, e))
      throw std::runtime_error("Entry changed before deletion: " + p);
    const bool directory = expected.directory && !expected.link;
    const DeleteFolder folder{p, e, parent};
    const DeleteFolder *chain = directory ? &folder : parent;
    if (directory) {
      const auto children = ListFiles(p, c);
      VerifyDeleteFolders(chain, c);
      for (size_t i = 0; i < children.size(); ++i) {
        try {
          DeleteTree(children[i].path, children[i], depth + 1, chain);
        } catch (...) {
          for (++i; i < children.size(); ++i)
            RecordOutcome({children[i].path, "", "unprocessed", "No removal requested."});
          throw;
        }
      }
    }
    VerifyDeleteFolders(chain, c);
    bool submitted = false;
    c.mutationSubmitted = &submitted;
    try {
      RemoveFile(p, directory, c);
    } catch (...) {
      // Do not resend a destructive request. A fresh read may establish
      // absence; a replacement or unreadable path proves no such thing.
      auto failure = std::current_exception();
      if (!submitted)
        std::rethrow_exception(failure);
      c.mutationSubmitted = nullptr;
      FileEntry now;
      bool consulted = false, present = true;
      try {
        present = StatFile(p, now, c);
        VerifyDeleteFolders(parent, c);
        consulted = true;
      } catch (...) {}
      if (consulted && !present) {
        ++deletedEntries;
        RecordOutcome({p, "", "deleted", "Removal was not acknowledged; a fresh lookup confirmed absence at this path."});
      } else if (consulted && !e.providerIdentity.empty() &&
                 e.SameRevision(now)) {
        RecordOutcome({p, "", "not deleted", "The same entry is still present."});
      } else {
        ++result.unconfirmedDeletions;
        RecordOutcome({p, "", "deletion unconfirmed",
                       "Removal was requested but not acknowledged. Inspect this path before selecting it again."});
      }
      std::rethrow_exception(failure);
    }
    ++deletedEntries;
    RecordOutcome({p, "", "deleted",
                   "Server acknowledged removal by path; replacement after the last lookup cannot be excluded."});
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
    if (job.displayedRemoteDestination) {
      const bool named = job.action == Action::Rename || job.action == Action::MakeDirectory;
      const auto parent = named ? ParentLocation(job.destination)
                                : ParseLocation(job.destination).URL();
      FileEntry shown;
      shown.providerIdentity = *job.displayedRemoteDestination;
      runner.remoteDirectories.emplace(parent, Runner::RemoteDirectory{shown, {}});
      runner.VerifyRemoteDirectories(runner.Control(parent));
    }
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
    if (hooks.checkOverwrite && job.action == Action::Copy && !IsVirtual(job.destination)) {
      size_t count = 0;
      for (const auto &source : job.sources)
        runner.CheckExistingOutputs(source, ChildLocation(job.destination, LocationLeaf(source)),
                                    runner.Control(source), count);
    }
    for (auto &source : job.sources) {
      const size_t publishedBefore = runner.publishedEntries;
      std::string dst;
      try {
        runner.selectedSource = source;
        runner.selectedDirectory = false;
        runner.firstOmission.clear();
        runner.unverifiedDirectory.clear();
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
            if (expected->second.directory && runner.deletedEntries != deletedBefore) {
              ++runner.result.partialDeletions;
              runner.RecordOutcome({source, "", "partially deleted",
                                    "Deletion stopped. See individual paths for confirmed and unconfirmed removals."});
            }
            throw;
          }
        } else if (job.action == Action::Move || job.action == Action::Rename) {
          FileEntry current;
          try {
            current = runner.PrepareMove(source, dst, control);
          } catch (const std::exception &error) {
            runner.RecordOutcome({source, dst, "unprocessed", error.what()});
            throw;
          }
          runner.MoveNetworkEntry(source, dst, current, control);
        } else {
          const auto canonicalSource = TransferIdentity(source);
          const auto canonicalDestination = TransferIdentity(dst);
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
