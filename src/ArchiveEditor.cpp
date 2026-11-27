// SPDX-License-Identifier: MIT
#include "ArchiveEditor.h"
#include "ArchiveJobs.h"
#include "ArchiveJobChecks.h"
#include "ArchiveVolumes.h"
#include "JobSnapshot.h"
#include "MessageStore.h"
#include "StagingSpace.h"
#include "VirtualFile.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <set>
#include <stdexcept>
#include <unistd.h>
namespace Listi {
namespace {
void Require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct FD {
  int n;
  explicit FD(int fd) : n(fd) {}
  ~FD() {
    if (n >= 0)
      close(n);
  }
};
void Check(const Hooks &h) {
  if (h.proceed && !h.proceed())
    throw Cancelled();
}
bool Identity(const struct stat &a, const struct stat &b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
bool Stable(const struct stat &a, const struct stat &b) {
  return Identity(a, b) && a.st_mode == b.st_mode && a.st_size == b.st_size &&
         a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
         a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
         a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
         a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
struct stat PrivateDirectory(const std::string &path) {
  struct stat s;
  Require(!lstat(path.c_str(), &s) && S_ISDIR(s.st_mode) &&
              s.st_uid == getuid() && !(s.st_mode & 0077),
          "Archive edit folder is unavailable, replaced or not private.");
  return s;
}
void FlushDirectory(const std::string &path) {
  FD fd(open(path.c_str(), O_RDONLY));
  Require(fd.n >= 0 && !fsync(fd.n),
          "Cannot flush archive edit recovery folder.");
}
struct stat Regular(const std::string &path) {
  struct stat s;
  Require(!lstat(path.c_str(), &s) && S_ISREG(s.st_mode) && s.st_nlink == 1 &&
              s.st_uid == getuid(),
          "Working copy must remain an ordinary private file, not a link.");
  return s;
}
void Write(int fd, const char *bytes, size_t size) {
  while (size) {
    auto n = WriteStaging(fd, bytes, size);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && errno == ENOSPC)
      throw std::runtime_error("Not enough free space for the archive editing copy.");
    Require(n > 0, "Cannot write archive editing copy.");
    bytes += n;
    size -= n;
  }
}
void Snapshot(const std::string &from, const std::string &to,
              const Hooks &hooks, unsigned mode = 0600) {
  auto before = Regular(from);
  FD input(open(from.c_str(), O_RDONLY | O_NOFOLLOW)),
      output(open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
  Require(input.n >= 0 && output.n >= 0,
          "Cannot open archive editing snapshot.");
  struct stat opened;
  Require(!fstat(input.n, &opened) && Stable(before, opened),
          "Editor is still saving. Try again shortly.");
  std::vector<char> bytes(65536);
  int64_t total = 0;
  for (;;) {
    Check(hooks);
    auto n = read(input.n, bytes.data(), bytes.size());
    if (n < 0 && errno == EINTR)
      continue;
    Require(n >= 0, "Cannot read working copy.");
    if (!n)
      break;
    Require(n <= before.st_size - total,
            "Editor changed the working copy while it was being saved. Try again.");
    Write(output.n, bytes.data(), n);
    total += n;
  }
  struct stat after;
  Require(
      !fstat(input.n, &after) && Stable(before, after) &&
          Stable(before, Regular(from)) && total == before.st_size,
      "Editor changed the working copy while it was being saved. Try again.");
  Require(!fchmod(output.n, mode & 0777),
          "Cannot preserve edited member permissions.");
  struct timespec times[2] = {{0, UTIME_OMIT}, before.st_mtim};
  Require(!futimens(output.n, times) && !fsync(output.n),
          "Cannot flush edited file snapshot.");
}
bool Equal(const std::string &a, const std::string &b, const Hooks &hooks) {
  auto x = Regular(a), y = Regular(b);
  if (x.st_size != y.st_size)
    return false;
  FD left(open(a.c_str(), O_RDONLY | O_NOFOLLOW)),
      right(open(b.c_str(), O_RDONLY | O_NOFOLLOW));
  Require(left.n >= 0 && right.n >= 0,
          "Cannot compare archive working copies.");
  std::vector<char> l(65536), r(65536);
  bool equal = true;
  for (;;) {
    Check(hooks);
    auto n = read(left.n, l.data(), l.size());
    if (n < 0 && errno == EINTR)
      continue;
    Require(n >= 0, "Cannot read archive working copy.");
    if (!n)
      break;
    size_t have = 0;
    while (have < size_t(n)) {
      auto got = read(right.n, r.data() + have, n - have);
      if (got < 0 && errno == EINTR)
        continue;
      Require(got > 0, "Saved snapshot changed while comparing.");
      have += got;
    }
    if (memcmp(l.data(), r.data(), n)) {
      equal = false;
      break;
    }
  }
  Require(Stable(x, Regular(a)) && Stable(y, Regular(b)),
          "Editor is still saving. Try again shortly.");
  return equal;
}
} // namespace
std::vector<ArchiveEditRecord> ListArchiveEdits(const std::string &root) {
  if (access(root.c_str(), F_OK) && errno == ENOENT)
    return {};
  PrivateDirectory(root);
  DIR *raw = opendir(root.c_str());
  Require(raw, "Cannot list archive edit sessions.");
  std::unique_ptr<DIR, int (*)(DIR *)> dir(raw, closedir);
  std::vector<ArchiveEditRecord> records;
  std::set<std::pair<std::string, std::string>> retired;
  for (;;) {
    errno = 0;
    auto e = readdir(raw);
    if (!e) {
      Require(!errno, "Cannot finish listing archive edit sessions.");
      break;
    }
    if (strncmp(e->d_name, "edit-", 5))
      continue;
    const auto path = Join(root, e->d_name);
    try {
      PrivateDirectory(path);
      struct stat record;
      auto journal = Join(path, "session");
      const bool inactive = lstat(journal.c_str(), &record) && errno == ENOENT;
      if (inactive) {
        // A retired journal can itself supersede an earlier interrupted edit.
        journal = Join(path, "previous-session");
        if (lstat(journal.c_str(), &record) && errno == ENOENT)
          continue; // Never published.
      }
      Regular(journal);
      auto m = LoadMessageStore(journal);
      Require(m.GetInt32("version", 0) == 1,
              "Unsupported archive edit recovery record.");
      const std::string member = m.GetString("member", "");
      Require(ParseLocation(member).Archive(), "Invalid archive edit recovery record.");
      const std::string previous = m.GetString("predecessor", "");
      if (!previous.empty() && previous != e->d_name)
        retired.emplace(member, previous);
      if (!inactive)
        records.push_back({path, member, {}});
    } catch (const std::runtime_error &e) {
      // Preserve this copy and report its location without hiding other edits.
      records.push_back({path, {}, e.what()});
    }
  }
  // A crash after publishing a replacement but before retiring its predecessor
  // can leave both journals on disk. The replacement is authoritative.
  records.erase(std::remove_if(records.begin(), records.end(), [&](const auto &r) {
                  return retired.count({r.member, LocationLeaf(r.directory)}) != 0;
                }), records.end());
  std::sort(records.begin(), records.end(),
            [](const auto &a, const auto &b) { return a.member < b.member; });
  return records;
}
void ArchiveEditSession::ValidateDirectory() const {
  Require(Identity(directoryIdentity, PrivateDirectory(directory)) &&
              Identity(workIdentity, PrivateDirectory(Join(directory, "work"))),
          "Archive working folder was replaced. No archive update was made.");
}
void ArchiveEditSession::RequireActive() const {
  for (const auto &record : ListArchiveEdits(ParentLocation(directory)))
    if (record.member == member && record.directory == directory)
      return;
  throw std::runtime_error("This edit is no longer active. Working copy was retained.");
}
void ArchiveEditSession::ValidateArchive() const {
  for (const auto &entry : target.expectedArchiveSets)
    Require(
        FindArchiveVolumes(entry.first).Fingerprint() == entry.second,
        "Archive changed outside this editing session. Your working copy is "
        "kept. Reconcile it with the current archive before continuing.");
}
void ArchiveEditSession::Persist() {
  ValidateDirectory();
  BMessage m;
  m.AddInt32("version", 1);
  m.AddString("member", member.c_str());
  m.AddInt32("permissions", permissions);
  if (!predecessor.empty())
    m.AddString("predecessor", predecessor.c_str());
  auto job = EncodeJob(target, 0);
  m.AddMessage("target", &job);
  SaveMessageStore(Join(directory, "session"), m);
  FlushDirectory(directory);
}
std::shared_ptr<ArchiveEditSession>
ArchiveEditSession::Resume(const std::string &directory) {
  auto s = std::shared_ptr<ArchiveEditSession>(new ArchiveEditSession);
  s->directory = directory;
  s->directoryIdentity = PrivateDirectory(directory);
  s->workIdentity = PrivateDirectory(Join(directory, "work"));
  Regular(Join(directory, "session"));
  auto m = LoadMessageStore(Join(directory, "session"));
  Require(m.GetInt32("version", 0) == 1,
          "Invalid archive edit recovery record.");
  s->member = m.GetString("member", "");
  s->predecessor = m.GetString("predecessor", "");
  auto location = ParseLocation(s->member);
  Require(location.Archive() && location.path != "/",
          "Invalid archive edit member.");
  s->permissions = m.GetInt32("permissions", 0644) & 0777;
  s->working = Join(Join(directory, "work"), LocationLeaf(s->member));
  BMessage encoded;
  Require(m.FindMessage("target", &encoded) == B_OK,
          "Archive edit recovery target is missing.");
  s->target = DecodeJob(encoded);
  Require(s->target.action == Action::EditArchive &&
              s->target.destination == location.archive &&
              s->target.archivePath ==
                  ParseLocation(ParentLocation(s->member)).path.substr(1) &&
              s->target.expectedArchiveSets.count(location.archive),
          "Archive edit recovery target is invalid.");
  Require(s->target.sources.empty() && s->target.archiveRemove.empty() &&
              s->target.archiveFolders.empty() &&
              s->target.archiveRenames.empty(),
          "Archive edit recovery record has unexpected changes.");
  s->ValidateDirectory();
  Regular(s->working);
  Regular(Join(directory, "saved"));
  return s;
}
std::shared_ptr<ArchiveEditSession>
ArchiveEditSession::Open(const std::string &member, const std::string &root,
                         const Hooks &hooks, const Job &shown) {
  const auto location = ParseLocation(member);
  ArchiveJobChecks::VerifyArchiveSets(shown);
  Require(location.Archive() && location.path != "/",
          "Choose one regular member inside a writable local archive.");
  if (mkdir(root.c_str(), 0700))
    Require(errno == EEXIST, "Cannot create private archive editing folder.");
  PrivateDirectory(root);
  const auto records = ListArchiveEdits(root);
  for (const auto &record : records)
    if (record.member == member) {
      auto retained = Resume(record.directory);
      ArchiveJobChecks::RequireArchiveVersions(shown, retained->target);
      ArchiveJobChecks::VerifyArchiveSets(shown);
      return retained;
    }
  Require(records.size() < 256, "Too many retained archive edits. Keep needed files and remove old edit folders.");
  return Create(member, root, {}, hooks, shown);
}
std::shared_ptr<ArchiveEditSession>
ArchiveEditSession::Create(const std::string &member, const std::string &root,
                           const std::string &predecessor, const Hooks &hooks, const Job &shown) {
  const auto location = ParseLocation(member);
  auto s = std::shared_ptr<ArchiveEditSession>(new ArchiveEditSession);
  s->member = member;
  s->predecessor = predecessor;
  s->target = PrepareArchiveEditTarget(
      location.archive, ParseLocation(ParentLocation(member)).path, hooks);
  ArchiveJobChecks::RequireArchiveVersions(shown, s->target);
  TransferControl control;
  control.proceed = [&] {
    Check(hooks);
    s->ValidateArchive();
    return true;
  };
  FileEntry entry;
  Require(StatFile(member, entry, control) && !entry.directory && !entry.link,
          "Only ordinary archive files can be edited.");
  Require(!entry.sizeKnown || entry.size >= 0, "Archive member has an invalid size.");
  const uint64_t limit = entry.sizeKnown ? uint64_t(entry.size)
                                         : std::numeric_limits<uint64_t>::max();
  s->permissions = entry.permissions & 0777;
  std::string pattern = Join(root, "edit-XXXXXX");
  std::vector<char> name(pattern.begin(), pattern.end());
  name.push_back(0);
  char *made = mkdtemp(name.data());
  Require(made, "Cannot allocate private archive edit folder.");
  s->directory = made;
  s->directoryIdentity = PrivateDirectory(s->directory);
  const auto work = Join(s->directory, "work");
  const auto leaf = LocationLeaf(member);
  FD folder(open(made, O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
  FD workFolder(-1);
  try {
    Require(folder.n >= 0 && !mkdirat(folder.n, "work", 0700),
            "Cannot create editor working folder.");
    workFolder.n = openat(folder.n, "work", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    Require(workFolder.n >= 0, "Cannot open editor working folder.");
    s->workIdentity = PrivateDirectory(work);
    s->working = Join(work, leaf);
    FD output(openat(workFolder.n, leaf.c_str(),
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
    Require(output.n >= 0, "Cannot create editor working file.");
    uint64_t extracted = 0;
    ReadFile(member, [&](const char *b, size_t n) {
      Require(n <= limit - extracted, "Archive member exceeded its declared size.");
      Write(output.n, b, n);
      extracted += n;
    }, control);
    Require(!entry.sizeKnown || extracted == limit,
            "Archive member did not match its declared size.");
    Require(!fsync(output.n), "Cannot flush editor working file.");
    s->ValidateArchive();
    Snapshot(s->working, Join(s->directory, "saved"), hooks);
    FlushDirectory(work);
    // Make the parent entry durable before publishing a discoverable journal.
    FlushDirectory(root);
    s->Persist();
  } catch (...) {
    // No editor has seen these files yet. Preserve a published journal (e.g.
    // directory fsync failed), but do not strand unfinished, decrypted data.
    struct stat now;
    if (folder.n >= 0 && fstatat(folder.n, "session", &now, AT_SYMLINK_NOFOLLOW)
                             && errno == ENOENT) {
      if (workFolder.n >= 0)
        unlinkat(workFolder.n, leaf.c_str(), 0);
      unlinkat(folder.n, "work", AT_REMOVEDIR);
      unlinkat(folder.n, "saved", 0);
    }
    if (!lstat(made, &now) && Identity(s->directoryIdentity, now))
      rmdir(made); // Only removes an empty, unpublished session.
    throw;
  }
  return s;
}
std::shared_ptr<ArchiveEditSession>
ArchiveEditSession::StartFresh(const Hooks &hooks) {
  ValidateDirectory();
  Check(hooks);
  RequireActive();
  const auto journal = Join(directory, "session"),
             previous = Join(directory, "previous-session");
  Require(
      access(previous.c_str(), F_OK) && errno == ENOENT,
      "This edit has already been retired. Reopen the current member instead.");
  // Do not hide the only recoverable draft while extracting a replacement.
  // Publish the complete replacement first. Its journal identifies the old
  // one even when a shutdown interrupts the final housekeeping rename.
  auto fresh = Create(member, ParentLocation(directory), LocationLeaf(directory), hooks);
  // Retirement is only housekeeping now; a failure cannot undo publication.
  (void)rename(journal.c_str(), previous.c_str());
  return fresh;
}
bool ArchiveEditSession::ChangedBytes(const Hooks &hooks) {
  ValidateDirectory();
  ValidateArchive();
  return !Equal(working, Join(directory, "saved"), hooks);
}
ArchiveEditSession::Observation ArchiveEditSession::Observe(const Hooks &hooks) {
  ValidateDirectory();
  ValidateArchive();
  struct stat now;
  if (lstat(working.c_str(), &now) && errno == ENOENT) {
    haveObserved = false;
    stable = 0;
    return Waiting;
  }
  now = Regular(working);
  if (!haveObserved || !Stable(observed, now)) {
    observed = now;
    haveObserved = true;
    stable = 0;
    checked = false;
    return Waiting;
  }
  if (++stable < 2)
    return Waiting;
  if (checked)
    return Unchanged;
  if (ChangedBytes(hooks))
    return Changed;
  checked = true;
  return Unchanged;
}
void ArchiveEditSession::FinishSaveJournal() {
  try {
    ValidateDirectory();
    RequireActive();
    ValidateArchive();
    FlushDirectory(directory);
    Persist();
  } catch (const std::exception &e) {
    throw std::runtime_error(
        std::string("Archive was saved, but recovery metadata could not be "
                    "updated. Working copy kept: ") +
        e.what());
  }
  journalPending = false;
  haveObserved = false;
  checked = false;
  stable = 0;
}
bool ArchiveEditSession::Save(const Hooks &hooks) {
  Check(hooks);
  if (journalPending)
    FinishSaveJournal();
  if (!ChangedBytes(hooks))
    return false;
  RequireActive();
  // Snapshot a stable editor save. Later editor writes never become part of
  // this archive transaction, and are detected by the next observation cycle.
  std::string pattern = Join(directory, "save-XXXXXX");
  std::vector<char> name(pattern.begin(), pattern.end());
  name.push_back(0);
  auto made = mkdtemp(name.data());
  Require(made, "Cannot allocate archive save snapshot.");
  std::string folder = made, candidate = Join(folder, LocationLeaf(member));
  struct Cleanup {
    std::string file, folder;
    ~Cleanup() {
      unlink(file.c_str());
      rmdir(folder.c_str());
    }
  } cleanup{candidate, folder};
  Snapshot(working, candidate, hooks, permissions);
  ValidateDirectory();
  ValidateArchive();
  auto edit = target;
  SetArchiveEditAdditions(edit, {candidate});
  Hooks internal = hooks;
  // Saving this one selected member explicitly replaces its previous version.
  internal.conflict = [](const std::string &) { return Decision::Replace; };
  auto result = Run(edit, internal);
  if (result.cancelled)
    throw Cancelled();
  Require(result.errors.empty() && result.completed == 1 &&
              !result.publishedArchiveVersion.empty(),
          result.errors.empty()
              ? "Could not save the archive. Working copy was retained."
              : result.errors.front());
  const auto current = FindArchiveVolumes(target.destination);
  Require(current.Fingerprint() == result.publishedArchiveVersion,
          "Archive changed after this save was published. Working copy kept. "
          "Reconcile it with the current archive before continuing.");
  auto published = target;
  const auto &file = current.files.front();
  published.expectedSources.at(target.destination) = {int32_t(file.device), int64_t(file.inode)};
  published.expectedArchiveSets.at(target.destination) = result.publishedArchiveVersion;
  // The journal certifies both the archive fingerprint and the saved bytes.
  // Install and flush the baseline first. An interruption must leave a stale
  // fingerprint that refuses writeback, never a current one with old bytes.
  Require(!chmod(candidate.c_str(), 0600) &&
              !rename(candidate.c_str(), Join(directory, "saved").c_str()),
          "Archive was saved, but the local recovery baseline could not be "
          "updated. Working copy was retained.");
  target = std::move(published);
  journalPending = true;
  FinishSaveJournal();
  return true;
}
} // namespace Listi
