// SPDX-License-Identifier: MIT
#include "BulkRename.h"
#include "JobSnapshot.h"
#include "NamedEntry.h"
#include "RecoveryInventory.h"
#include "Sha256.h"
#include <Directory.h>
#include <Path.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace Listi {
namespace {
bool SameNode(const struct stat &st, const NodeIdentity &identity) {
  return st.st_dev == static_cast<dev_t>(identity.device) &&
         st.st_ino == static_cast<ino_t>(identity.inode);
}

bool InspectSource(const Job &job, const std::string &source) {
  const auto before = Leaf(source);
  const auto original = job.expectedSources.find(source);
  struct stat st;
  if (before.empty() || before == "." || before == ".." ||
      Join(job.destinationFolder, before) != source ||
      original == job.expectedSources.end() || lstat(source.c_str(), &st) ||
      !SameNode(st, original->second))
    throw std::runtime_error("The selected entry changed or is unavailable.");
  if (S_ISDIR(st.st_mode) &&
      st.st_dev != static_cast<dev_t>(job.expectedDestination.device))
    throw std::runtime_error("Mounted volumes cannot be renamed.");
  return S_ISDIR(st.st_mode);
}

void InspectFolder(const Job &job) {
  struct stat st;
  if ((job.action != Action::BulkRename && job.action != Action::UndoBulkRename) ||
      job.destinationFolder.empty() ||
      job.destinationFolder.front() != '/' ||
      stat(job.destinationFolder.c_str(), &st) || !S_ISDIR(st.st_mode) ||
      !SameNode(st, job.expectedDestination))
    throw std::runtime_error("The selected folder changed or is unavailable.");
}

std::string NewName(const std::string &leaf, bool folder,
                    const BulkRenameOptions &options, size_t index) {
  std::string stem = leaf, extension;
  const auto dot = leaf.find_last_of('.');
  if (!folder && dot != std::string::npos && dot > 0 && dot + 1 < leaf.size()) {
    stem = leaf.substr(0, dot);
    extension = leaf.substr(dot);
  }
  if (!options.search.empty()) {
    size_t pos = 0;
    while ((pos = stem.find(options.search, pos)) != std::string::npos) {
      stem.replace(pos, options.search.size(), options.replacement);
      pos += options.replacement.size();
    }
  }
  std::string number;
  if (options.number) {
    number = std::to_string(uint64_t(options.start) + index);
    if (number.size() < options.digits)
      number.insert(0, options.digits - number.size(), '0');
    number.insert(0, " ");
  }
  return options.prefix + stem + options.suffix + number + extension;
}

// A target may be occupied only by the exact selected name that will move.
void CheckTargets(BulkRenamePreview &preview) {
  std::map<std::string, size_t> counts;
  std::set<std::string> leaving;
  for (const auto &row : preview.rows) {
    if (row.error.empty()) {
      ++counts[row.after];
      if (row.before != row.after)
        leaving.insert(row.before);
    }
  }
  std::map<std::string, size_t> needsVacant;
  std::deque<size_t> blocked;
  for (size_t i = 0; i < preview.rows.size(); ++i) {
    auto &row = preview.rows[i];
    if (row.error.empty() && row.before != row.after) {
      ++preview.changed;
      try {
        if (counts[row.after] > 1)
          throw std::runtime_error("More than one entry has this new name.");
        ValidateEntryLeaf(row.after);
        if (!leaving.count(row.after))
          ValidateNewEntryName(preview.job.destinationFolder, row.after);
        else
          needsVacant.emplace(row.after, i);
      } catch (const std::runtime_error &error) {
        row.error = error.what();
      }
    }
    if (!row.error.empty())
      blocked.push_back(i);
  }
  // A blocked rename will not release its original name. Follow the chain
  // backwards so that no dependent row is advertised as ready.
  while (!blocked.empty()) {
    const auto &occupied = preview.rows[blocked.front()].before;
    blocked.pop_front();
    ++preview.blocked;
    const auto dependent = needsVacant.find(occupied);
    if (dependent != needsVacant.end()) {
      preview.rows[dependent->second].error =
          "Another blocked entry cannot release this name.";
      blocked.push_back(dependent->second);
    }
  }
}

class FileDescriptor {
public:
  explicit FileDescriptor(int value) : fd(value) {}
  ~FileDescriptor() {
    if (fd >= 0)
      close(fd);
  }
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  const int fd;
};

struct Cancelled {};
void Check(const Hooks &hooks) {
  if (hooks.proceed && !hooks.proceed())
    throw Cancelled();
}

bool Stat(int parent, const std::string &leaf, struct stat &st) {
  return fstatat(parent, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0;
}

bool SameStamp(const struct stat &a, const struct stat &b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
         a.st_mode == b.st_mode && a.st_size == b.st_size &&
         a.st_uid == b.st_uid && a.st_gid == b.st_gid &&
         a.st_nlink == b.st_nlink &&
         a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
         a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
         a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
         a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}

template <class T> void HashValue(Sha256 &hash, T value) {
  hash.Update(&value, sizeof(value));
}

void HashStamp(Sha256 &hash, const struct stat &st, bool ctime) {
  HashValue(hash, st.st_dev);
  HashValue(hash, st.st_ino);
  HashValue(hash, st.st_mode);
  HashValue(hash, st.st_uid);
  HashValue(hash, st.st_gid);
  HashValue(hash, st.st_nlink);
  HashValue(hash, st.st_size);
  HashValue(hash, st.st_mtim.tv_sec);
  HashValue(hash, st.st_mtim.tv_nsec);
  if (ctime) {
    HashValue(hash, st.st_ctim.tv_sec);
    HashValue(hash, st.st_ctim.tv_nsec);
  }
}

std::string RootStamp(const struct stat &st) {
  Sha256 hash;
  HashStamp(hash, st, true);
  return hash.Finish();
}

std::vector<std::string> ChildNames(int parent) {
  const int copy = openat(parent, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (copy < 0)
    throw std::runtime_error("Cannot open folder for rename verification.");
  std::unique_ptr<DIR, decltype(&closedir)> dir(fdopendir(copy), closedir);
  if (!dir) {
    close(copy);
    throw std::runtime_error("Cannot list folder for rename verification.");
  }
  std::vector<std::string> names;
  for (;;) {
    errno = 0;
    const auto child = readdir(dir.get());
    if (!child) {
      if (errno)
        throw std::runtime_error("Cannot finish listing folder for rename verification.");
      break;
    }
    const std::string name(child->d_name);
    if (name != "." && name != "..")
      names.push_back(name);
    if (names.size() > 200000)
      throw std::runtime_error("Folder exceeds the rename verification limit.");
  }
  std::sort(names.begin(), names.end());
  return names;
}

void HashContents(int fd, Sha256 &hash, const Hooks &hooks,
                  const std::string &leaf, uint64_t &bytesLeft) {
  char buffer[65536];
  for (;;) {
    Check(hooks);
    const auto count = read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0)
      throw std::runtime_error("Cannot read contents for undo: " + leaf);
    if (!count)
      break;
    if (uint64_t(count) > bytesLeft)
      throw std::runtime_error("Batch exceeds the 512 MiB undo verification limit.");
    bytesLeft -= count;
    hash.Update(buffer, count);
  }
}

void FingerprintEntry(int parent, const std::string &leaf, Sha256 &hash,
                      const Hooks &hooks, dev_t device, unsigned depth,
                      size_t &remaining, uint64_t &bytesLeft) {
  Check(hooks);
  struct stat st;
  if (!remaining || depth > 64 || !Stat(parent, leaf, st) ||
      st.st_dev != device)
    throw std::runtime_error("Cannot verify contents for undo: " + leaf);
  --remaining;
  // A rename can change the root's ctime. A separate post-move root stamp
  // guards the final entry until the first inverse step.
  HashStamp(hash, st, depth != 0);
  if (S_ISREG(st.st_mode) && (st.st_size < 0 || uint64_t(st.st_size) > bytesLeft))
    throw std::runtime_error("Batch exceeds the 512 MiB undo verification limit.");
  if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) {
    FileDescriptor entry(openat(parent, leaf.c_str(),
                                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat opened;
    if (entry.fd < 0 || fstat(entry.fd, &opened) || !SameStamp(st, opened))
      throw std::runtime_error("Cannot verify contents for undo: " + leaf);
    if (S_ISDIR(st.st_mode)) {
      const auto children = ChildNames(entry.fd);
      HashValue(hash, children.size());
      for (const auto &name : children) {
        HashValue(hash, name.size());
        hash.Update(name.data(), name.size());
        FingerprintEntry(entry.fd, name, hash, hooks, device, depth + 1,
                         remaining, bytesLeft);
      }
    } else {
      HashContents(entry.fd, hash, hooks, leaf, bytesLeft);
    }
  }
  // Symlinks are never followed, and their text cannot change in place.
  struct stat after;
  if (!Stat(parent, leaf, after) || !SameStamp(st, after))
    throw std::runtime_error("Entry changed during undo verification: " + leaf);
}

std::string Fingerprint(int parent, const std::string &leaf,
                        const Hooks &hooks, const NodeIdentity &identity,
                        size_t &remaining, uint64_t &bytesLeft) {
  struct stat st;
  if (!Stat(parent, leaf, st) || !SameNode(st, identity))
    throw std::runtime_error("The renamed entry changed: " + leaf);
  Sha256 hash;
  FingerprintEntry(parent, leaf, hash, hooks, st.st_dev, 0, remaining,
                   bytesLeft);
  return hash.Finish();
}

void MoveName(int parent, const std::string &before, const std::string &after,
              const NodeIdentity &identity, const std::string &stamp = {}) {
  struct stat st;
  if (!Stat(parent, before, st) || !SameNode(st, identity) ||
      (!stamp.empty() && RootStamp(st) != stamp))
    throw std::runtime_error("Source changed: " + before);
  if (Stat(parent, after, st))
    throw std::runtime_error("Destination already exists: " + after);
  if (errno != ENOENT)
    throw std::runtime_error("Cannot check destination: " + after);
  // Haiku's BEntry::MoveTo(false) likewise checks then renames. A directory
  // descriptor pins both parents, but Haiku lacks an atomic no-replace flag.
  if (renameat(parent, before.c_str(), parent, after.c_str()))
    throw std::runtime_error(before + " -> " + after + ": " + strerror(errno));
}

std::string TemporaryName(int parent, const std::set<std::string> &reserved,
                          const char *prefix = ".listi-rename-") {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  for (int attempt = 0; attempt < 100; ++attempt) {
    auto name = prefix + std::to_string(getpid()) + "-" +
                std::to_string(nonce) + "-" + std::to_string(attempt);
    if (reserved.count(name))
      continue;
    struct stat st;
    if (Stat(parent, name, st))
      continue;
    if (errno == ENOENT)
      return name;
    break;
  }
  throw std::runtime_error("Cannot find a free temporary rename name.");
}

// This is an intent log, not permission to make mutations after a restart.
class RenameJournal {
public:
  RenameJournal(int parent, const std::set<std::string> &reserved)
      : parent(parent),
        name(TemporaryName(parent, reserved, ".listi-recovery-rename-")),
        file(openat(parent, name.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) {
    if (file.fd < 0)
      throw std::runtime_error("Cannot create bulk rename recovery record.");
  }

  void Record(const std::string &line) const {
    size_t at = 0;
    while (at < line.size()) {
      const auto n = write(file.fd, line.data() + at, line.size() - at);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        throw std::runtime_error("Cannot write bulk rename recovery record.");
      at += n;
    }
  }
  void Sync() const {
    if (fsync(file.fd) || fsync(parent))
      throw std::runtime_error("Cannot flush bulk rename recovery record or folder.");
  }
  void Intent(const NodeIdentity &identity, const std::string &from,
              const std::string &to) const {
    Record(Format("intent", identity, from, to));
    Sync();
  }
  struct stat Identity() const {
    struct stat st;
    if (fstat(file.fd, &st))
      throw std::runtime_error("Cannot identify bulk rename recovery journal.");
    return st;
  }
  void Finish() const {
    struct stat opened, named;
    if (fstat(file.fd, &opened) || !Stat(parent, name, named) ||
        opened.st_dev != named.st_dev || opened.st_ino != named.st_ino ||
        unlinkat(parent, name.c_str(), 0) || fsync(parent))
      throw std::runtime_error("Could not confirm removal of the bulk rename recovery record.");
  }
  static std::string Format(const char *kind, const NodeIdentity &identity,
                            const std::string &from, const std::string &to) {
    return std::string(kind) + " " + std::to_string(identity.device) + " " +
           std::to_string(identity.inode) + " " + Quote(from) + " " +
           Quote(to) + "\n";
  }

  const int parent;
  const std::string name;

private:
  const FileDescriptor file;

  static std::string Quote(const std::string &name) {
    static const char hex[] = "0123456789abcdef";
    std::string escaped = "\"";
    for (const unsigned char byte : name) {
      if (byte < ' ' || byte == 127 || byte == '\\' || byte == '"') {
        escaped += "\\x";
        escaped += hex[byte >> 4];
        escaped += hex[byte & 15];
      } else
        escaped += char(byte);
    }
    return escaped + '"';
  }
};

std::string CurrentFolder(const Job &job) {
  struct stat st;
  if (!stat(job.destinationFolder.c_str(), &st) &&
      SameNode(st, job.expectedDestination))
    return job.destinationFolder;
  const node_ref ref(job.expectedDestination.device,
                     job.expectedDestination.inode);
  BDirectory directory(&ref);
  BEntry entry;
  BPath path;
  if (directory.GetEntry(&entry) == B_OK && entry.GetPath(&path) == B_OK &&
      !stat(path.Path(), &st) && SameNode(st, job.expectedDestination))
    return path.Path();
  return {};
}

} // namespace

Job FreezeBulkRename(const std::string &folder,
                     const std::vector<std::string> &selection,
                     const std::map<std::string, NodeIdentity> &expected) {
  if (selection.size() < 2 || folder.empty() || folder.front() != '/')
    throw std::runtime_error(
        "Select at least two entries in one local folder.");
  Job job;
  job.action = Action::BulkRename;
  job.sources = selection;
  job.expectedSources = expected;
  std::set<std::string> unique;
  for (const auto &source : selection) {
    if (Join(folder, Leaf(source)) != source ||
        !unique.insert(source).second)
      throw std::runtime_error(
          "Select distinct entries in the current local folder.");
  }
  FreezeLocalJob(job, folder);
  InspectFolder(job);
  for (const auto &source : selection)
    InspectSource(job, source);
  return job;
}

BulkRenamePreview PreviewBulkRename(const Job &job,
                                    const BulkRenameOptions &options) {
  BulkRenamePreview preview;
  preview.job = job;
  preview.job.bulkNames.clear();
  try {
    InspectFolder(job);
    if (options.number && (options.digits < 1 || options.digits > 9))
      throw std::runtime_error("Number width must be from 1 to 9.");
  } catch (const std::runtime_error &error) {
    preview.error = error.what();
    return preview;
  }
  for (size_t i = 0; i < job.sources.size(); ++i) {
    const auto &source = job.sources[i];
    BulkRenameRow row{Leaf(source), "", ""};
    try {
      const auto folder = InspectSource(job, source);
      const auto override = options.overrides.find(source);
      row.after = override == options.overrides.end()
                      ? NewName(row.before, folder, options, i)
                      : override->second;
      preview.job.bulkNames.emplace(source, row.after);
    } catch (const std::runtime_error &error) {
      row.error = error.what();
    }
    preview.rows.push_back(std::move(row));
  }
  CheckTargets(preview);
  return preview;
}

Result RunBulkRename(const Job &job, const Hooks &hooks) {
  Result result;
  const bool undo = job.action == Action::UndoBulkRename;
  for (const auto &source : job.sources) {
    const auto target = job.bulkNames.find(source);
    std::string destination;
    if (target != job.bulkNames.end())
      destination = Join(job.destinationFolder, target->second);
    result.outcomes.push_back({source, destination, "unprocessed", ""});
  }

  BulkRenamePreview confirmed;
  confirmed.job = job;
  try {
    Check(hooks);
    InspectFolder(job);
    if (job.sources.size() < (undo ? 1u : 2u) ||
        job.bulkNames.size() != job.sources.size() ||
        job.expectedSources.size() != job.sources.size() ||
        (undo && job.bulkFingerprints.size() != job.sources.size()))
      throw std::runtime_error("The selected batch is incomplete.");
    std::set<std::string> unique;
    for (const auto &source : job.sources) {
      const auto name = job.bulkNames.find(source);
      if (name == job.bulkNames.end() || !unique.insert(source).second)
        throw std::runtime_error("The selected batch is incomplete.");
      BulkRenameRow row{Leaf(source), name->second, ""};
      try {
        InspectSource(job, source);
      } catch (const std::runtime_error &error) {
        row.error = error.what();
      }
      confirmed.rows.push_back(std::move(row));
    }
    CheckTargets(confirmed);
    if (!confirmed.CanApply()) {
      for (size_t i = 0; i < confirmed.rows.size(); ++i) {
        const auto &row = confirmed.rows[i];
        if (!row.error.empty()) {
          result.outcomes[i].status = "failed";
          result.outcomes[i].detail = row.error;
          result.errors.push_back(row.before + ": " + row.error);
        } else if (row.before != row.after) {
          ++result.skipped;
        }
      }
      if (result.errors.empty())
        result.errors.push_back("There are no changed names to apply.");
      return result;
    }
  } catch (const Cancelled &) {
    result.cancelled = true;
    return result;
  } catch (const std::runtime_error &error) {
    result.errors.push_back(error.what());
    return result;
  }

  FileDescriptor folder(open(job.destinationFolder.c_str(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  struct stat parent;
  if (folder.fd < 0 || fstat(folder.fd, &parent) ||
      !SameNode(parent, job.expectedDestination)) {
    result.errors.push_back("The selected folder changed or is unavailable.");
    return result;
  }

  struct Step { size_t row; std::string before, after; };
  std::vector<Step> history;
  history.reserve(confirmed.changed * 2);
  std::map<std::string, size_t> pending, incoming;
  std::set<std::string> reserved;
  std::vector<std::string> current;
  std::vector<const NodeIdentity *> identities;
  std::deque<size_t> ready;
  for (size_t i = 0; i < confirmed.rows.size(); ++i) {
    const auto &row = confirmed.rows[i];
    current.push_back(row.before);
    identities.push_back(&job.expectedSources.at(job.sources[i]));
    reserved.insert(row.before);
    reserved.insert(row.after);
    if (row.before != row.after) {
      pending.emplace(row.before, i);
      incoming.emplace(row.after, i);
    } else {
      result.outcomes[i].status = "skipped";
      result.outcomes[i].detail = "Name unchanged.";
    }
  }
  for (const auto &item : pending)
    if (!pending.count(confirmed.rows[item.second].after))
      ready.push_back(item.second);

  auto released = [&](const std::string &name) {
    const auto dependent = incoming.find(name);
    if (dependent != incoming.end())
      ready.push_back(dependent->second);
  };
  std::map<size_t, std::string> rootStamps;
  std::map<std::string, std::string> fingerprints;
  constexpr size_t kUndoEntryLimit = 200000;
  constexpr uint64_t kUndoByteLimit = 512ull * 1024 * 1024;
  size_t entriesLeft = kUndoEntryLimit;
  uint64_t bytesLeft = kUndoByteLimit;
  std::optional<RenameJournal> journal;
  std::optional<RecoveryRecord> recovery;
  auto move = [&](size_t i, const std::string &target) {
    Check(hooks);
    InspectFolder(job);
    Step step{i, current[i], target};
    const auto stamp = rootStamps.find(i);
    if (undo && Fingerprint(folder.fd, step.before, hooks, *identities[i],
                    entriesLeft, bytesLeft) !=
            job.bulkFingerprints.at(job.sources[i]).contents)
      throw std::runtime_error("Undo stopped: contents changed: " + job.sources[i]);
    journal->Intent(*identities[i], step.before, step.after);
    MoveName(folder.fd, step.before, step.after, *identities[i],
             stamp == rootStamps.end() ? "" : stamp->second);
    history.push_back(std::move(step));
    current[i] = target;
    struct stat st;
    if (!Stat(folder.fd, target, st) || !SameNode(st, *identities[i]))
      throw std::runtime_error("Cannot confirm the renamed entry: " + target);
    rootStamps[i] = RootStamp(st);
  };

  try {
    if (!undo) {
      // Freeze content before touching names, so edits during the batch cannot
      // become the baseline for undo. Unreadable entries do not block renaming.
      try {
        for (const auto &item : pending)
          fingerprints.emplace(item.first, Fingerprint(
              folder.fd, item.first, hooks, *identities[item.second],
              entriesLeft, bytesLeft));
      } catch (const std::runtime_error &) {
        fingerprints.clear();
      }
    } else {
      for (const auto &item : pending) {
        const size_t i = item.second;
        const auto &source = job.sources[i];
        const auto expected = job.bulkFingerprints.find(source);
        struct stat st;
        if (expected == job.bulkFingerprints.end() ||
            !Stat(folder.fd, item.first, st) ||
            RootStamp(st) != expected->second.rootStamp ||
            Fingerprint(folder.fd, item.first, hooks, *identities[i],
                        entriesLeft, bytesLeft) != expected->second.contents)
          throw std::runtime_error("Undo stopped: the renamed entry changed: " + source);
        rootStamps.emplace(i, expected->second.rootStamp);
      }
      // At most one row per cycle is verified a second time after staging.
      entriesLeft = kUndoEntryLimit * 2;
      bytesLeft = kUndoByteLimit * 2;
    }

    journal.emplace(folder.fd, reserved);
    journal->Record("Listi bulk rename recovery record v1. Review only. Do not execute.\n");
    journal->Record(RenameJournal::Format("folder", job.expectedDestination,
                                         job.destinationFolder, ""));
    for (const auto &item : pending) {
      const size_t i = item.second;
      journal->Record(RenameJournal::Format("plan", *identities[i],
          confirmed.rows[i].before, confirmed.rows[i].after));
    }
    journal->Sync();
    recovery.emplace("rename", Join(job.destinationFolder, journal->name),
                     journal->Identity());

    // Rename chains from the free end. In a cycle, free one original by moving
    // it to a temporary leaf, then finish that row after its target is free.
    while (!pending.empty() || !ready.empty()) {
      if (ready.empty()) {
        const auto anchor = pending.begin()->second;
        const auto original = confirmed.rows[anchor].before;
        const auto stage = TemporaryName(folder.fd, reserved);
        move(anchor, stage);
        pending.erase(original);
        released(original);
      }
      const size_t i = ready.front();
      ready.pop_front();
      const auto &row = confirmed.rows[i];
      const auto before = current[i];
      move(i, row.after);
      pending.erase(before);
      released(before);
    }
    Check(hooks);
    InspectFolder(job);
    for (size_t i = 0; i < confirmed.rows.size(); ++i) {
      const auto &row = confirmed.rows[i];
      if (row.before == row.after)
        continue;
      struct stat st;
      if (!Stat(folder.fd, current[i], st) || !SameNode(st, *identities[i]))
        throw std::runtime_error("Cannot confirm the final entry: " + current[i]);
    }
  } catch (const Cancelled &) {
    result.cancelled = true;
  } catch (const std::runtime_error &error) {
    result.errors.push_back(error.what());
  }

  if (result.cancelled || !result.errors.empty()) {
    // Ignore cancellation during recovery. Never replace an entry introduced
    // since the failed step, and continue restoring independent rows.
    for (auto step = history.rbegin(); step != history.rend(); ++step) {
      try {
        journal->Intent(*identities[step->row], step->after, step->before);
        MoveName(folder.fd, step->after, step->before, *identities[step->row]);
        current[step->row] = step->before;
      } catch (const std::runtime_error &error) {
        result.errors.push_back(std::string("Could not restore ") + step->before +
                                ": " + error.what());
        result.bulkRenameRecoveryNeeded = true;
      }
    }

    const auto actualFolder = CurrentFolder(job);
    using NodeKey = std::pair<int32_t, int64_t>;
    std::optional<std::map<NodeKey, std::string>> nearby;
    for (size_t i = 0; i < confirmed.rows.size(); ++i) {
      if (confirmed.rows[i].before == confirmed.rows[i].after)
        continue;
      auto &outcome = result.outcomes[i];
      struct stat st;
      std::string located;
      if (!actualFolder.empty()) {
        if (Stat(folder.fd, current[i], st) && SameNode(st, *identities[i])) {
          located = current[i];
        } else {
          // Another process may have renamed an entry between our steps.
          // Search this folder once; do not guess among hard-linked names.
          if (!nearby) {
            nearby.emplace();
            try {
              for (const auto &name : ChildNames(folder.fd)) {
                if (Stat(folder.fd, name, st)) {
                  const auto item = nearby->emplace(
                      NodeKey{int32_t(st.st_dev), int64_t(st.st_ino)}, name);
                  if (!item.second)
                    item.first->second.clear();
                }
              }
            } catch (const std::runtime_error &) {
              nearby->clear();
            }
          }
          const auto found = nearby->find(
              NodeKey{identities[i]->device, identities[i]->inode});
          if (found != nearby->end() && !found->second.empty() &&
              Stat(folder.fd, found->second, st) && SameNode(st, *identities[i]))
            located = found->second;
        }
      }
      outcome.destination = located.empty() ? "" : Join(actualFolder, located);
      if (located.empty()) {
        outcome.status = "unconfirmed";
        outcome.detail = "Last known name: " + current[i];
        result.bulkRenameRecoveryNeeded |= !history.empty();
      } else if (outcome.destination != outcome.source) {
        outcome.status = "retained";
        outcome.detail = "Entry remains at this location.";
        result.bulkRenameRecoveryNeeded = true;
      } else {
        outcome.status = "preserved";
        outcome.detail = "Original name retained or restored.";
        ++result.skipped;
      }
    }
    if (journal) {
      try {
        if (!result.bulkRenameRecoveryNeeded) {
          journal->Record("restored\n");
          journal->Sync();
          journal->Finish();
          if (recovery)
            recovery->Retire();
        }
      } catch (const std::runtime_error &error) {
        result.errors.push_back(error.what());
        result.bulkRenameRecoveryNeeded = true;
      }
      if (result.bulkRenameRecoveryNeeded)
        result.errors.push_back("Review retained rename record: " +
                                Join(actualFolder.empty() ? job.destinationFolder
                                                         : actualFolder,
                                     journal->name));
    }
    return result;
  }

  try {
    journal->Record("committed\n");
    journal->Sync();
    journal->Finish();
    recovery->Retire();
  } catch (const std::runtime_error &error) {
    result.bulkRenameRecoveryNeeded = true;
    result.errors.push_back(std::string("Names were changed. ") + error.what() +
                            " Review recovery record if present: " +
                            Join(job.destinationFolder, journal->name));
  }
  Job reverse;
  reverse.action = Action::UndoBulkRename;
  reverse.destinationFolder = job.destinationFolder;
  reverse.expectedDestination = job.expectedDestination;
  for (size_t i = 0; i < confirmed.rows.size(); ++i) {
    const auto &row = confirmed.rows[i];
    if (row.before == row.after)
      continue;
    auto &outcome = result.outcomes[i];
    outcome.status = "moved";
    ++result.completed;
    result.completedRoots.emplace_back(outcome.source, outcome.destination);
    if (!undo) {
      reverse.sources.push_back(outcome.destination);
      reverse.bulkNames.emplace(outcome.destination, row.before);
      reverse.expectedSources.emplace(outcome.destination, *identities[i]);
      if (const auto fingerprint = fingerprints.find(row.before);
          fingerprint != fingerprints.end())
        reverse.bulkFingerprints.emplace(
            outcome.destination, BulkRenameGuard{fingerprint->second,
                                                 rootStamps.at(i)});
    }
    if (hooks.fileDone)
      hooks.fileDone(outcome.source, outcome.destination);
  }
  if (!undo && reverse.bulkFingerprints.size() == reverse.sources.size())
    result.bulkRenameUndo = std::move(reverse);
  return result;
}
} // namespace Listi
