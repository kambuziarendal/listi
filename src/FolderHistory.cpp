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
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

namespace Listi {
namespace {
namespace fs = std::filesystem;
constexpr size_t kManifestLimit = 64 * 1024 * 1024;
constexpr off_t kAttributeLimit = 16 * 1024 * 1024;
constexpr char kBinding[] = "listi-history-v1";
std::mutex bindingLock;

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

std::vector<std::string> Names(int fd) {
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
    if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
      names.emplace_back(entry->d_name);
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

std::string Digest(int fd, int copy, const std::atomic_bool &cancel) {
  Sha256 sha;
  char bytes[64 * 1024];
  while (true) {
    CheckCancel(cancel);
    auto count = read(fd, bytes, sizeof(bytes));
    if (count < 0 && errno == EINTR) continue;
    Check(count >= 0, "Read file contents");
    if (!count) return sha.Finish();
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

void CollectAttributes(int fd, BMessage &entry, const std::atomic_bool &cancel) {
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
              const std::atomic_bool &cancel, unsigned depth) {
  CheckCancel(cancel);
  Require(depth <= 128, "History folder nesting exceeds 128 levels.");
  auto before = Stat(fd);
  Require(before.st_dev == device, "History does not cross mounted volumes.");
  Message(node.AddString("name", name.c_str()));
  Message(node.AddInt32("mode", before.st_mode));
  Message(node.AddInt64("sec", before.st_mtim.tv_sec));
  Message(node.AddInt32("ns", before.st_mtim.tv_nsec));
  CollectAttributes(fd, node, cancel);
  if (S_ISREG(before.st_mode)) {
    auto digest = Digest(fd, -1, cancel);
    auto object = "object-" + digest;
    int dir = store.Descriptor();
    struct stat stored{};
    if (fstatat(dir, object.c_str(), &stored, AT_SYMLINK_NOFOLLOW)) {
      Check(errno == ENOENT, "Check history object");
      Pending pending(dir);
      Check(lseek(fd, 0, SEEK_SET) == 0, "Rewind source file");
      Require(Digest(fd, pending.file.fd, cancel) == digest, "Source changed: " + name);
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
    target[length] = '\0';
    Message(node.AddString("target", target));
  } else if (S_ISDIR(before.st_mode)) {
    for (const auto &child : Names(fd)) {
      struct stat info{};
      Check(fstatat(fd, child.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0, child);
      Require(S_ISREG(info.st_mode) || S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode),
              "Unsupported entry: " + child);
      FD opened(openat(fd, child.c_str(), O_RDONLY | O_NOTRAVERSE | O_NONBLOCK | O_CLOEXEC), child);
      const auto identity = Stat(opened.fd);
      Require(identity.st_ino == info.st_ino && identity.st_dev == info.st_dev,
              "Source changed: " + child);
      BMessage entry;
      SaveNode(opened.fd, fd, child, device, store, entry, cancel, depth + 1);
      Message(node.AddMessage("child", &entry));
      Require(node.FlattenedSize() <= ssize_t(kManifestLimit), "History manifest exceeds 64 MiB.");
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
      std::string digest = child.GetString("digest", "");
      Require(digest.size() == 64 && Token(digest, "0123456789abcdef", 64), "Invalid history digest.");
      FD input(openat(store, ("object-" + digest).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC), name);
      Require(S_ISREG(Stat(input.fd).st_mode), "Invalid content object.");
      FD output(openat(fd, name.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600), name);
      Require(Digest(input.fd, output.fd, cancel) == digest, "History object is damaged: " + name);
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
  Require(state->Bound() || Names(state->store->Descriptor()).empty(),
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
  CheckCancel(cancel);
  auto &source = *state->root;
  auto &store = *state->store;
  int dir = store.Descriptor();
  source.Verify();
  {
    const std::lock_guard<std::mutex> lock(bindingLock);
    if (!state->Bound()) {
      Require(Names(dir).empty(), "History folder is no longer empty.");
      store.Verify();
      Pending marker(dir);
      auto binding = state->Binding();
      Write(marker.file.fd, binding.data(), binding.size());
      marker.Publish(kBinding, store, cancel);
      store.Flush();
    }
  }
  const auto point = "point-" + Unique();
  BMessage node;
  SaveNode(source.Descriptor(), -1, "", Stat(source.Descriptor()).st_dev,
           store, node, cancel, 0);
  source.Verify();
  auto size = node.FlattenedSize();
  Require(size > 0 && size <= ssize_t(kManifestLimit), "History manifest exceeds 64 MiB.");
  std::vector<char> data(size);
  Message(node.Flatten(data.data(), size));
  Pending pending(dir);
  Write(pending.file.fd, data.data(), data.size());
  store.Flush(); // Data and object names precede publication of the manifest.
  pending.Publish(point, store, cancel);
  try {
    store.Flush();
  } catch (const std::exception &e) {
    throw std::runtime_error(std::string("Point published; durability not confirmed: ") + e.what());
  }
}

void HistoryStore::Restore(const std::string &point, const std::string &destination,
                           const std::atomic_bool &cancel) const {
  auto root = Browse(point);
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
  Check(mkdirat(parent->Descriptor(), leaf.c_str(), 0700) == 0, "Create new copy at " + target.string());
  const auto marker = RestoreMarker(root);
  try {
    FD output(openat(parent->Descriptor(), leaf.c_str(),
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC), target);
    const auto identity = Stat(output.fd);
    PinnedDirectory copy(target, identity.st_dev, identity.st_ino);
    constexpr char incomplete[] = "Incomplete Listi restore. Do not use as a finished copy.";
    Check(fs_write_attr(output.fd, marker.c_str(), B_STRING_TYPE, 0,
                        incomplete, sizeof(incomplete)) == sizeof(incomplete),
          "Mark incomplete copy");
    copy.Flush();
    parent->Flush();

    RestoreNode(output.fd, state->store->Descriptor(), root, cancel, 0);
    CheckCancel(cancel);
    state->store->Verify();
    copy.Verify();
    parent->Flush();
    // Do not check cancellation after this commit point. The restored bytes,
    // metadata and parent directory were already flushed with the marker.
    Check(fs_remove_attr(output.fd, marker.c_str()) == 0, "Finish restored copy");
    if (fsync(output.fd) != 0)
      throw std::runtime_error(std::string("Copy complete; completion marker durability not confirmed: ") + strerror(errno));
  } catch (const std::exception &e) {
    throw std::runtime_error(std::string(e.what()) +
        " Check the new folder for an incomplete copy: " + target.string() +
        " (BFS attribute " + marker + ").");
  }
}
}
