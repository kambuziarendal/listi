// SPDX-License-Identifier: MIT
#include "FolderHistory.h"
#include "DirectoryFinalization.h"
#include "Sha256.h"

#include <OS.h>
#include <DataIO.h>
#include <dirent.h>
#include <fcntl.h>
#include <fs_attr.h>
#include <fs_info.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sys/file.h>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace Listi {
namespace {
namespace fs = std::filesystem;
constexpr size_t kManifestLimit = 64 * 1024 * 1024;
constexpr off_t kAttributeLimit = 16 * 1024 * 1024;
constexpr char kBinding[] = "listi-history-v1";
constexpr uint64 kMiB = 1024 * 1024;
constexpr uint64 kAutomaticStoreBytes = 512 * kMiB;
constexpr uint64 kAutomaticFreeBytes = 512 * kMiB;
constexpr unsigned kAutomaticPoints = 24;
constexpr unsigned kAutomaticStoreEntries = 8192;
constexpr unsigned kAutomaticNodes = 2048;
constexpr uint64 kAutomaticReadBytes = 256 * kMiB;
constexpr size_t kAutomaticManifestBytes = 4 * kMiB;

void Require(bool ok, const std::string &what) {
  if (!ok) throw std::runtime_error(what);
}
void Check(bool ok, const std::string &what) {
  if (!ok) throw std::runtime_error(what + ": " + strerror(errno));
}
void Message(status_t status) {
  Require(status == B_OK, "Cannot encode history metadata.");
}
void CheckCancel(const std::atomic_bool &cancel) {
  Require(!cancel, "Canceled.");
}

struct AutomaticBudget {
  unsigned nodes = kAutomaticNodes;
  unsigned points = 0;
  uint64 read = kAutomaticReadBytes;
  uint64 write = 0;
  uint64 metadata = kAutomaticManifestBytes;
  std::string latest;
  const bigtime_t deadline = system_time() + 90 * 1000000LL;

  void CheckTime(const std::atomic_bool &cancel) const {
    CheckCancel(cancel);
    Require(system_time() < deadline, "Automatic history reached its time limit.");
  }
  void Use(uint64 &budget, uint64 bytes) {
    Require(bytes <= budget, "Automatic history reached its size limit.");
    budget -= bytes;
  }
};

struct FD {
  explicit FD(int fd, const std::string &name) : fd(fd) { Check(fd >= 0, name); }
  ~FD() { close(fd); }
  FD(const FD &) = delete;
  int fd;
};
struct Directory {
  explicit Directory(DIR *stream, const char *operation) : stream(stream) {
    Check(stream != nullptr, operation);
  }
  ~Directory() { closedir(stream); }
  DIR *stream;
};

struct stat Stat(int fd) {
  struct stat info{};
  Check(fstat(fd, &info) == 0, "Read node information");
  return info;
}

std::vector<std::string> Names(int fd, size_t limit = SIZE_MAX, bool firstOnly = false) {
  int scan = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  Check(scan >= 0, "Open directory");
  DIR *stream = fdopendir(scan);
  if (!stream) close(scan);
  Directory dir(stream, "List directory");
  std::vector<std::string> names;
  while (true) {
    errno = 0;
    auto *entry = readdir(dir.stream);
    if (!entry) {
      Check(!errno, "Read directory");
      break;
    }
    if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
      Require(names.size() < limit, "Automatic history reached its folder entry limit.");
      names.emplace_back(entry->d_name);
      if (firstOnly) break;
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::unique_ptr<PinnedDirectory> Pin(const std::string &path) {
  struct stat info{};
  Check(lstat(path.c_str(), &info) == 0, path);
  fs_info volume{};
  Require(fs_stat_dev(info.st_dev, &volume) == 0 &&
      !(volume.flags & B_FS_IS_SHARED) && strcmp(volume.fsh_name, "bfs") == 0,
      "Folder history requires a local BFS folder: " + path);
  return std::make_unique<PinnedDirectory>(path, info.st_dev, info.st_ino);
}

bool Within(const fs::path &path, const fs::path &base) {
  auto relative = path.lexically_relative(base);
  return !relative.empty() && *relative.begin() != "..";
}
void Separate(const fs::path &a, const fs::path &b) {
  Require(!Within(a, b) && !Within(b, a),
          "Source, history and restored copy must be separate.");
}

void Write(int fd, const void *data, size_t size) {
  const auto *buffer = static_cast<const char *>(data);
  while (size) {
    auto wrote = write(fd, buffer, size);
    if (wrote < 0 && errno == EINTR) continue;
    Check(wrote > 0, "Write history data");
    buffer += wrote;
    size -= wrote;
  }
}

std::string Read(int fd, size_t limit) {
  auto bytes = Stat(fd).st_size;
  Require(bytes >= 0 && uint64(bytes) <= limit, "History metadata exceeds the size limit.");
  std::string data(size_t(bytes), '\0');
  size_t offset = 0;
  while (offset < data.size()) {
    auto count = read(fd, data.data() + offset, data.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    Require(count > 0, "Incomplete history metadata.");
    offset += count;
  }
  return data;
}

std::string Unique() {
  static std::atomic_uint64_t sequence{0};
  return std::to_string(real_time_clock_usecs()) + "-"
      + std::to_string(getpid()) + "-" + std::to_string(sequence++);
}

struct Pending {
  explicit Pending(int dir)
      : dir(dir), name(".pending-" + Unique()),
        file(openat(dir, name.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600), name) {}
  ~Pending() { if (!name.empty()) unlinkat(dir, name.c_str(), 0); }
  void Publish(const std::string &target, const PinnedDirectory &store,
               const std::atomic_bool &cancel) {
    Check(fsync(file.fd) == 0, "Flush history file");
    CheckCancel(cancel);
    store.Verify();
    Check(renameat(dir, name.c_str(), dir, target.c_str()) == 0, "Publish history file");
    name.clear();
  }
  int dir;
  std::string name;
  FD file;
};

std::string Digest(int fd, int copy, const std::atomic_bool &cancel,
                   AutomaticBudget *budget = nullptr) {
  Sha256 sha;
  char bytes[64 * 1024];
  while (true) {
    CheckCancel(cancel);
    if (budget) budget->CheckTime(cancel);
    auto count = read(fd, bytes, sizeof(bytes));
    if (count < 0 && errno == EINTR) continue;
    Check(count >= 0, "Read file contents");
    if (!count) return sha.Finish();
    if (budget) {
      budget->Use(budget->read, count);
      if (copy >= 0) budget->Use(budget->write, count);
    }
    sha.Update(bytes, count);
    if (copy >= 0) Write(copy, bytes, count);
  }
}

bool Token(const std::string &token, const char *chars, size_t limit) {
  return !token.empty() && token.size() <= limit &&
      token.find_first_not_of(chars) == std::string::npos;
}
bool Point(const std::string &name) {
  return name.compare(0, 6, "point-") == 0 &&
      Token(name.substr(6), "0123456789-", 80);
}

void CollectAttributes(int fd, BMessage &entry, const std::atomic_bool &cancel,
                       AutomaticBudget *budget) {
  DIR *attrs = fs_fopen_attr_dir(fd);
  Check(attrs != nullptr, "Open BFS attributes");
  try {
    while (true) {
      CheckCancel(cancel);
      errno = 0;
      auto *name = fs_read_attr_dir(attrs);
      if (!name) {
        Check(!errno, "List BFS attributes");
        break;
      }
      attr_info info{};
      Check(fs_stat_attr(fd, name->d_name, &info) == 0, "Read BFS attribute information");
      Require(info.size >= 0 && info.size <= kAttributeLimit, "BFS attribute exceeds the size limit.");
      if (budget) {
        budget->CheckTime(cancel);
        budget->Use(budget->read, info.size + 1);
        budget->Use(budget->metadata, info.size + strlen(name->d_name) + 512);
        Require(entry.FlattenedSize() + info.size < ssize_t(kAutomaticManifestBytes),
                "Automatic history metadata exceeds 4 MiB.");
      }
      std::vector<char> bytes(size_t(info.size) + 1);
      Check(fs_read_attr(fd, name->d_name, info.type, 0, bytes.data(), info.size)
          == info.size, "Read BFS attribute");
      BMessage attribute;
      Message(attribute.AddString("name", name->d_name));
      Message(attribute.AddUInt32("type", info.type));
      if (info.size)
        Message(attribute.AddData("value", B_RAW_TYPE, bytes.data(), info.size, false));
      Message(entry.AddMessage("attr", &attribute));
    }
  } catch (...) {
    fs_close_attr_dir(attrs);
    throw;
  }
  fs_close_attr_dir(attrs);
}

void ApplyAttributes(int fd, const BMessage &entry, const std::atomic_bool &cancel) {
  BMessage attr;
  for (int32 i = 0; entry.FindMessage("attr", i, &attr) == B_OK; ++i) {
    CheckCancel(cancel);
    const char *name = attr.GetString("name", "");
    const void *bytes = "";
    ssize_t length = 0;
    attr.FindData("value", B_RAW_TYPE, &bytes, &length);
    Require(*name && length >= 0 && length <= kAttributeLimit, "Invalid history attribute.");
    Check(fs_write_attr(fd, name, attr.GetUInt32("type", 0), 0, bytes, length)
        == length, "Restore BFS attribute");
  }
}

void SaveNode(int fd, int parent, const std::string &name, dev_t device,
              const PinnedDirectory &store, BMessage &node,
              const std::atomic_bool &cancel, unsigned depth,
              AutomaticBudget *budget) {
  CheckCancel(cancel);
  if (budget) {
    budget->CheckTime(cancel);
    Require(budget->nodes > 0, "Automatic history reached its source entry limit.");
    --budget->nodes;
    budget->Use(budget->metadata, name.size() + 512);
  }
  Require(depth <= 128, "History folder nesting exceeds 128 levels.");
  auto before = Stat(fd);
  Require(before.st_dev == device, "History does not cross mounted volumes.");
  Message(node.AddString("name", name.c_str()));
  Message(node.AddInt32("mode", before.st_mode));
  Message(node.AddInt64("sec", before.st_mtim.tv_sec));
  Message(node.AddInt32("ns", before.st_mtim.tv_nsec));
  CollectAttributes(fd, node, cancel, budget);
  if (S_ISREG(before.st_mode)) {
    if (budget)
      Require(uint64(before.st_size) <= budget->read, "Automatic history reached its read limit.");
    auto digest = Digest(fd, -1, cancel, budget);
    auto object = "object-" + digest;
    int dir = store.Descriptor();
    struct stat stored{};
    if (fstatat(dir, object.c_str(), &stored, AT_SYMLINK_NOFOLLOW)) {
      Check(errno == ENOENT, "Check history object");
      if (budget)
        Require(uint64(before.st_size) <= std::min(budget->read, budget->write),
                "Automatic history reached its copy limit.");
      Pending pending(dir);
      Check(lseek(fd, 0, SEEK_SET) == 0, "Rewind source file");
      Require(Digest(fd, pending.file.fd, cancel, budget) == digest, "Source changed: " + name);
      pending.Publish(object, store, cancel);
    } else {
      Require(S_ISREG(stored.st_mode) && stored.st_size == before.st_size,
              "Invalid existing history object: " + name);
    }
    Message(node.AddString("digest", digest.c_str()));
  } else if (S_ISLNK(before.st_mode)) {
    char target[B_PATH_NAME_LENGTH];
    auto length = readlinkat(parent, name.c_str(), target, sizeof(target));
    Check(length >= 0, "Read symbolic link");
    Require(size_t(length) < sizeof(target), "Symbolic link target is too long.");
    if (budget) budget->Use(budget->metadata, length);
    target[length] = '\0';
    Message(node.AddString("target", target));
  } else if (S_ISDIR(before.st_mode)) {
    for (const auto &child : Names(fd, budget ? budget->nodes : SIZE_MAX)) {
      struct stat info{};
      Check(fstatat(fd, child.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0, child);
      Require(S_ISREG(info.st_mode) || S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode),
              "Unsupported entry: " + child);
      FD opened(openat(fd, child.c_str(), O_RDONLY | O_NOTRAVERSE | O_NONBLOCK | O_CLOEXEC), child);
      const auto identity = Stat(opened.fd);
      Require(identity.st_ino == info.st_ino && identity.st_dev == info.st_dev,
              "Source changed: " + child);
      BMessage entry;
      SaveNode(opened.fd, fd, child, device, store, entry, cancel, depth + 1, budget);
      Message(node.AddMessage("child", &entry));
      Require(node.FlattenedSize() <= ssize_t(budget ? kAutomaticManifestBytes : kManifestLimit),
              budget ? "Automatic history metadata exceeds 4 MiB." :
                       "History manifest exceeds 64 MiB.");
    }
  } else {
    throw std::runtime_error("Unsupported entry: " + name);
  }
  auto after = Stat(fd);
  Require(before.st_size == after.st_size && before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
      before.st_ctim.tv_nsec == after.st_ctim.tv_nsec &&
      before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
      before.st_mtim.tv_nsec == after.st_mtim.tv_nsec, "Source changed: " + name);
  if (parent >= 0) {
    struct stat current{};
    Require(!fstatat(parent, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) &&
        after.st_ino == current.st_ino && after.st_dev == current.st_dev,
        "Source entry changed: " + name);
  }
}

// Use an attribute on the exclusively created inode, rather than an
// auxiliary pathname that could be replaced before we remove it. Existing
// snapshots may have any root attribute name, including the preferred one.
std::string RestoreMarker(const BMessage &entry) {
  const std::string prefix = "Listi:RestoreIncomplete";
  std::set<std::string> names;
  BMessage attr;
  for (int32 i = 0; entry.FindMessage("attr", i, &attr) == B_OK; ++i)
    names.insert(attr.GetString("name", ""));
  auto name = prefix;
  for (size_t i = 1; names.count(name); ++i)
    name = prefix + "-" + std::to_string(i);
  return name;
}

bool Leaf(const std::string &name) {
  return !name.empty() && name != "." && name != ".." &&
      name.find('/') == std::string::npos;
}

void RestoreBytes(int output, int store, const BMessage &entry, const std::atomic_bool &cancel) {
  std::string digest = entry.GetString("digest", "");
  Require(Token(digest, "0123456789abcdef", 64) && digest.size() == 64, "Invalid history digest.");
  FD input(openat(store, ("object-" + digest).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC), "Open saved file");
  Require(S_ISREG(Stat(input.fd).st_mode), "Invalid content object.");
  Require(Digest(input.fd, output, cancel) == digest, "Saved file content is damaged.");
}

BMessage FindRegularFile(BMessage folder, const fs::path &relative) {
  Require(!relative.empty() && relative.is_relative(), "Select a file in this version.");
  for (const auto &part : relative) {
    Require(Leaf(part) && S_ISDIR(folder.GetInt32("mode", 0)), "Invalid saved path.");
    BMessage child;
    bool found = false;
    for (int32 i = 0; folder.FindMessage("child", i, &child) == B_OK; ++i) {
      if (part != child.GetString("name", "")) continue;
      folder = child;
      found = true;
      break;
    }
    Require(found, "File is not in this version.");
  }
  Require(S_ISREG(folder.GetInt32("mode", 0)) && !folder.HasMessage("child"), "Select a regular file.");
  return folder;
}

void RestoreNode(int fd, int store, const BMessage &entry, const std::atomic_bool &cancel,
                 unsigned depth) {
  CheckCancel(cancel);
  Require(depth <= 128, "History folder nesting exceeds 128 levels.");
  BMessage child;
  for (int32 i = 0; entry.FindMessage("child", i, &child) == B_OK; ++i) {
    CheckCancel(cancel);
    std::string name = child.GetString("name", "");
    Require(Leaf(name), "Invalid history entry name.");
    mode_t mode = child.GetInt32("mode", 0);
    if (S_ISDIR(mode)) {
      Check(mkdirat(fd, name.c_str(), 0700) == 0, "Create " + name);
      FD opened(openat(fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC), name);
      RestoreNode(opened.fd, store, child, cancel, depth + 1);
    } else if (S_ISREG(mode)) {
      FD output(openat(fd, name.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600), name);
      RestoreBytes(output.fd, store, child, cancel);
      RestoreNode(output.fd, store, child, cancel, depth + 1);
    } else if (S_ISLNK(mode)) {
      Check(symlinkat(child.GetString("target", ""), fd, name.c_str()) == 0, "Create link " + name);
      FD link(openat(fd, name.c_str(), O_RDONLY | O_NOTRAVERSE | O_CLOEXEC), name);
      ApplyAttributes(link.fd, child, cancel);
    } else {
      throw std::runtime_error("Unknown history entry type.");
    }
  }
  ApplyAttributes(fd, entry, cancel);
  timespec times[] = {{0, UTIME_OMIT}, {entry.GetInt64("sec", 0), entry.GetInt32("ns", 0)}};
  Check(futimens(fd, times) == 0, "Restore modification time");
  Check(fchmod(fd, entry.GetInt32("mode", 0) & 0777) == 0, "Restore permissions");
  Check(fsync(fd) == 0, "Flush restored file or folder");
}
}

struct HistoryStore::State {
  std::string source, folder;
  std::unique_ptr<PinnedDirectory> store, root;

  std::string Binding() const { return std::string(kBinding) + "\n" + source; }
  bool Bound() const {
    store->Verify();
    int fd = openat(store->Descriptor(), kBinding, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) return false;
    FD marker(fd, "Open history binding");
    Require(S_ISREG(Stat(marker.fd).st_mode) && Read(marker.fd, B_PATH_NAME_LENGTH + 64) == Binding(),
            "History belongs to a different source folder.");
    return true;
  }
};

HistoryStore::HistoryStore(const std::string &source, const std::string &folder)
    : state(std::make_unique<State>()) {
  Require(fs::path(folder).is_absolute(), "Enter an absolute path to an existing history folder.");
  const fs::path chosen(folder);
  state->source = fs::canonical(source);
  state->folder = fs::canonical(chosen);
  Separate(state->source, state->folder);
  state->root = Pin(state->source);
  state->store = Pin(chosen);
  Require(fs::canonical(chosen) == state->folder, "History folder changed while connecting.");
  Require(state->Bound() || Names(state->store->Descriptor(), 1, true).empty(),
          "Choose an empty folder or an existing Listi history folder.");
}
HistoryStore::~HistoryStore() = default;

BMessage HistoryStore::Points() const {
  BMessage points;
  if (state->Bound()) {
    auto names = Names(state->store->Descriptor());
    for (auto it = names.rbegin(); it != names.rend(); ++it)
      if (Point(*it)) Message(points.AddString("point", it->c_str()));
  }
  state->store->Verify();
  return points;
}

BMessage HistoryStore::Browse(const std::string &point) const {
  Require(Point(point) && state->Bound(), "Select a saved history point.");
  FD file(openat(state->store->Descriptor(), point.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC),
          "Open history point");
  Require(S_ISREG(Stat(file.fd).st_mode), "Invalid point file.");
  std::string buffer = Read(file.fd, kManifestLimit);
  BMemoryIO stream(buffer.data(), buffer.size());
  BMessage node;
  Require(node.Unflatten(&stream) == B_OK && S_ISDIR(node.GetInt32("mode", 0)), "Invalid history point.");
  state->store->Verify();
  return node;
}

void HistoryStore::Save(const std::atomic_bool &cancel) const {
  SaveInternal(cancel, false);
}

BMessage HistoryStore::FolderIdentity() const {
  BMessage identity;
  for (const auto *folder : {state->root.get(), state->store.get()}) {
    folder->Verify();
    const auto info = Stat(folder->Descriptor());
    Message(identity.AddInt64("node", info.st_dev));
    Message(identity.AddInt64("node", info.st_ino));
    Message(identity.AddInt64("node", info.st_crtim.tv_sec));
    Message(identity.AddInt64("node", info.st_crtim.tv_nsec));
  }
  Message(identity.AddString("path", state->source.c_str()));
  Message(identity.AddString("path", state->folder.c_str()));
  return identity;
}

bool HistoryStore::SaveAutomatically(const std::atomic_bool &cancel,
                                                     const BMessage &approvedFolders) const {
  auto current = FolderIdentity();
  Require(current.HasSameData(approvedFolders),
          "A selected folder was replaced or remounted. Disable and confirm the folders again.");
  return SaveInternal(cancel, true);
}

bool HistoryStore::SaveInternal(const std::atomic_bool &cancel, bool automatic) const {
  CheckCancel(cancel);
  auto &source = *state->root;
  auto &store = *state->store;
  int dir = store.Descriptor();
  source.Verify();
  // Take a fresh descriptor: simultaneous saves can share a HistoryStore.
  FD lock(openat(dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC), "Open history writer lock");
  Require(flock(lock.fd, LOCK_EX | LOCK_NB) == 0, "Another history writer is active or the store cannot be locked.");
  AutomaticBudget limits;
  if (automatic) {
    uint64 stored = kAutomaticStoreBytes;
    for (const auto &name : Names(dir, kAutomaticStoreEntries)) {
      limits.CheckTime(cancel);
      struct stat item{};
      Check(fstatat(dir, name.c_str(), &item, AT_SYMLINK_NOFOLLOW) == 0, "Check history size");
      Require(S_ISREG(item.st_mode) && item.st_size >= 0, "Use a dedicated automatic history folder.");
      // Count allocation/metadata conservatively, also for small or abandoned files.
      limits.Use(stored, uint64(item.st_size) + 4096);
      if (Point(name)) {
        ++limits.points;
        limits.latest = name;
      }
    }
    Require(limits.points < kAutomaticPoints, "Automatic history has 24 saved points. No points were removed.");
    fs_info volume{};
    const auto device = Stat(dir).st_dev;
    Check(fs_stat_dev(device, &volume) == 0, "Read history free space");
    const uint64 available = uint64(volume.free_blocks) * volume.block_size;
    Require(available > kAutomaticFreeBytes, "Automatic history needs 512 MiB of free space in reserve.");
    limits.write = std::min(stored, available - kAutomaticFreeBytes);
    // Reserve directory and filesystem metadata for new objects, binding and point.
    limits.Use(limits.write, (kAutomaticNodes + 2) * 4096);
  }
  if (!state->Bound()) {
    Require(Names(dir, 1, true).empty(),
            "History folder is no longer empty.");
    store.Verify();
    Pending marker(dir);
    auto binding = state->Binding();
    Write(marker.file.fd, binding.data(), binding.size());
    marker.Publish(kBinding, store, cancel);
    store.Flush();
  }
  const auto point = "point-" + Unique();
  BMessage node;
  SaveNode(source.Descriptor(), -1, "", Stat(source.Descriptor()).st_dev,
           store, node, cancel, 0, automatic ? &limits : nullptr);
  source.Verify();
  auto size = node.FlattenedSize();
  Require(size > 0 && size <= ssize_t(kManifestLimit), "History manifest exceeds 64 MiB.");
  std::vector<char> data(size);
  Message(node.Flatten(data.data(), size));
  if (automatic) {
    limits.CheckTime(cancel);
    if (!limits.latest.empty()) {
      FD latest(openat(dir, limits.latest.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC),
                "Open latest history point");
      if (Stat(latest.fd).st_size == size &&
          Read(latest.fd, kAutomaticManifestBytes) == std::string_view(data.data(), data.size()))
        return false;
    }
    limits.Use(limits.write, size);
  }
  Pending pending(dir);
  Write(pending.file.fd, data.data(), data.size());
  store.Flush(); // Data and object names precede publication of the manifest.
  pending.Publish(point, store, cancel);
  try {
    store.Flush();
  } catch (const std::exception &e) {
    throw std::runtime_error(std::string("Point published; durability not confirmed: ") + e.what());
  }
  return true;
}

void HistoryStore::Restore(const std::string &point, const std::string &destination,
                           const std::atomic_bool &cancel, const std::string &selectedFile) const {
  auto root = Browse(point);
  const bool single = !selectedFile.empty();
  if (single) root = FindRegularFile(root, selectedFile);
  fs::path entered(destination);
  Require(entered.is_absolute() && Leaf(entered.filename()), "Enter an absolute path with a new folder name.");
  auto parentPath = fs::canonical(entered.parent_path());
  auto leaf = entered.filename().string();
  auto target = parentPath / leaf;
  Separate(target, state->source);
  Separate(target, state->folder);
  auto parent = Pin(parentPath);
  CheckCancel(cancel);
  state->store->Verify();
  parent->Verify();
  if (!single) Check(mkdirat(parent->Descriptor(), leaf.c_str(), 0700) == 0, "Create new copy at " + target.string());
  const auto marker = RestoreMarker(root);
  const int flags = single ? O_CREAT | O_EXCL | O_RDWR : O_RDONLY | O_DIRECTORY;
  FD output(openat(parent->Descriptor(), leaf.c_str(), flags | O_NOFOLLOW | O_CLOEXEC, 0600),
            "Open new copy at " + target.string());
  bool finished = false;
  try {
    const auto identity = Stat(output.fd);
    std::unique_ptr<PinnedDirectory> directory;
    if (!single) directory = std::make_unique<PinnedDirectory>(target, identity.st_dev, identity.st_ino);
    auto verifyCopy = [&] {
      parent->Verify();
      struct stat named{};
      Require(!fstatat(parent->Descriptor(), leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW) &&
              named.st_dev == identity.st_dev && named.st_ino == identity.st_ino,
              "New copy was replaced during restore.");
    };
    auto flushCopy = [&] {
      if (directory) directory->Flush();
      else {
        verifyCopy();
        Check(fsync(output.fd) == 0, "Flush restored file");
        verifyCopy();
      }
    };
    constexpr char incomplete[] = "Incomplete Listi restore. Do not use as a finished copy.";
    Check(fs_write_attr(output.fd, marker.c_str(), B_STRING_TYPE, 0,
                        incomplete, sizeof(incomplete)) == sizeof(incomplete),
          "Mark incomplete copy");
    flushCopy();
    parent->Flush();

    if (single) RestoreBytes(output.fd, state->store->Descriptor(), root, cancel);
    RestoreNode(output.fd, state->store->Descriptor(), root, cancel, 0);
    CheckCancel(cancel);
    state->store->Verify();
    verifyCopy();
    parent->Flush();
    // Do not check cancellation after this commit point. The restored bytes,
    // metadata and parent directory were already flushed with the marker.
    Check(fs_remove_attr(output.fd, marker.c_str()) == 0, "Finish restored copy");
    finished = true;
    flushCopy();
  } catch (const std::exception &e) {
    if (finished)
      throw std::runtime_error("Copy complete; marker removal not confirmed: " +
                               target.string() + ". " + e.what());
    throw std::runtime_error("Restore incomplete: " + std::string(e.what()) +
        " New copy: " + target.string() + " (BFS attribute " + marker + ").");
  }
}
}
