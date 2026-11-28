// SPDX-License-Identifier: MIT
#include "StagingSpace.h"
#include "InspectorDocument.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace Inspector {
namespace {
constexpr char kUtf8Bom[] = "\xef\xbb\xbf";
constexpr size_t kUtf8BomSize = sizeof(kUtf8Bom) - 1;

struct FD {
  int fd;
  explicit FD(int value) : fd(value) {}
  ~FD() {
    if (fd >= 0)
      close(fd);
  }
  FD(const FD &) = delete;
  FD &operator=(const FD &) = delete;
};

void Fail(const char *action) {
  throw std::runtime_error(std::string(action) + ": " + strerror(errno));
}

bool SameNode(const struct stat &a, const struct stat &b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

struct Lock {
  BFile &file;
  explicit Lock(BFile &f) : file(f) {
    if (file.Lock() != B_OK)
      throw std::runtime_error("This file is busy. Try again when it is free.");
  }
  ~Lock() { file.Unlock(); }
};

std::string Bytes(int fd) {
  struct stat st;
  if (fstat(fd, &st))
    Fail("Inspect text file");
  if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
      uint64_t(st.st_size) > kDocumentLimit)
    throw std::runtime_error(
        "Editing requires a complete file of 256 KiB or less.");
  std::string bytes(static_cast<size_t>(st.st_size), '\0');
  size_t offset = 0;
  while (offset < bytes.size()) {
    ssize_t n = pread(fd, bytes.data() + offset, bytes.size() - offset, offset);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      throw std::runtime_error("Could not read the complete text file.");
    offset += n;
  }
  struct stat after;
  if (fstat(fd, &after) || st.st_size != after.st_size ||
      st.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      st.st_mtim.tv_nsec != after.st_mtim.tv_nsec)
    throw std::runtime_error("The file changed while being read.");
  return bytes;
}

void WriteBytes(int fd, const std::string &bytes, size_t &written) {
  while (written < bytes.size()) {
    ssize_t n =
        pwrite(fd, bytes.data() + written, bytes.size() - written, written);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      throw std::runtime_error("Could not write the complete text file.");
    written += n;
  }
}

void Sync(int fd) {
  if (fsync(fd))
    Fail("Sync text file");
}

// Private original-byte backup. The containing directory is pinned and its
// new entry synced before the first source write. Never remove another node
// that has since acquired the temporary name.
class Recovery {
public:
  Recovery(const std::string &path, const std::string &bytes)
      : directory(path.substr(0, path.rfind('/') + 1)),
        parent(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)),
        file(-1) {
    if (parent.fd < 0)
      Fail("Open text recovery directory");
    const auto tick =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string prefix = ".Listi-text-recovery-" +
                               std::to_string(getpid()) + "-" +
                               std::to_string(tick) + "-";
    for (int i = 0; i < 100; ++i) {
      leaf = prefix + std::to_string(i);
      const auto name = path.substr(directory.size());
      if (leaf.size() + name.size() + 1 < B_FILE_NAME_LENGTH)
        leaf += "-" + name;
      file.fd =
          openat(parent.fd, leaf.c_str(),
                 O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (file.fd >= 0 || errno != EEXIST)
        break;
    }
    if (file.fd < 0)
      Fail("Create text recovery file");
    try {
      size_t written = 0;
      if (!Listi::StagingSpaceAvailable(file.fd, bytes.size()))
        Fail("Text recovery volume has insufficient free space");
      WriteBytes(file.fd, bytes, written);
      Sync(file.fd);
      Sync(parent.fd);
      if (Bytes(file.fd) != bytes || !AtPath())
        throw std::runtime_error("Cannot verify the text recovery file.");
    } catch (...) {
      Remove();
      throw;
    }
  }

  ~Recovery() {
    if (!keep)
      Remove();
  }

  bool AtPath() const {
    struct stat opened, named;
    return !fstat(file.fd, &opened) && !lstat(Path().c_str(), &named) &&
           SameNode(opened, named);
  }
  std::string Path() const { return directory + leaf; }
  bool keep = false;

private:
  std::string directory, leaf;
  FD parent, file;
  void Remove() {
    struct stat opened, named;
    if (!fstat(file.fd, &opened) &&
        !fstatat(parent.fd, leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW) &&
        SameNode(opened, named))
      unlinkat(parent.fd, leaf.c_str(), 0);
  }
};

} // namespace

void Document::Guard() const {
  struct stat opened, named;
  if (source.GetStat(&opened) != B_OK || lstat(path.c_str(), &named) ||
      !S_ISREG(named.st_mode) || !SameNode(opened, named))
    throw std::runtime_error(
        "The selected file moved or was replaced. Reopen it before editing.");
  // Edits may clear privilege bits at the filesystem level. Do not silently
  // change those bits or attempt to reinstate special privileges.
  if (!(named.st_mode & 0222) || (named.st_mode & (S_ISUID | S_ISGID)) ||
      access(path.c_str(), W_OK))
    throw std::runtime_error(
        "This file is read-only or has protected permissions.");
}

Document::Document(const std::string &selected) : path(selected) {
  struct stat named;
  if (path.empty() || path[0] != '/' || path.find('\0') != std::string::npos ||
      lstat(path.c_str(), &named) || !S_ISREG(named.st_mode))
    throw std::runtime_error(
        "Select a local regular text file, not a link or a virtual entry.");
  if (source.SetTo(path.c_str(),
                   O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) != B_OK)
    throw std::runtime_error("Cannot open this file for editing.");
  Lock lock(source);
  Guard();
  struct stat opened;
  if (source.GetStat(&opened) != B_OK || !SameNode(named, opened))
    throw std::runtime_error("The file changed while opening it for editing.");
  FD pinned(source.Dup());
  original = Bytes(pinned.fd);
  ValidateText(original);
  bom = original.compare(0, kUtf8BomSize, kUtf8Bom) == 0;
  bool lf = false;
  text.reserve(original.size());
  for (size_t i = bom ? kUtf8BomSize : 0; i < original.size(); ++i) {
    if (original[i] == '\r') {
      if (++i == original.size() || original[i] != '\n')
        throw std::runtime_error("Classic CR line endings are preview-only.");
      crlf = true;
    } else if (original[i] == '\n')
      lf = true;
    text.push_back(original[i]);
  }
  if (crlf && lf)
    throw std::runtime_error("Mixed line endings are preview-only.");
  Guard();
}

size_t Document::EncodedTextBytes(std::string_view part) const {
  return part.size() + (crlf ? std::count(part.begin(), part.end(), '\n') : 0);
}

void Document::Save(const std::string &edited) {
  if (EncodedTextBytes(edited) > TextBudget() || edited.find('\r') != std::string::npos)
    throw std::runtime_error("Use LF editor text within the 256 KiB limit.");
  ValidateText(edited);
  // In a document without a BOM, a leading U+FEFF would be decoded as a
  // marker on reopen, silently removing the first editor character.
  if (!bom && edited.compare(0, kUtf8BomSize, kUtf8Bom) == 0)
    throw std::runtime_error(
        "A file without a BOM cannot start with the U+FEFF character.");
  std::string encoded = bom ? kUtf8Bom : "";
  for (char byte : edited) {
    if (crlf && byte == '\n')
      encoded += '\r';
    encoded += byte;
  }
  if (encoded.size() > kDocumentLimit)
    throw std::runtime_error("Encoded text exceeds the 256 KiB editing limit.");
  // Prepare allocations before committing. A successful save only swaps strings.
  std::string newText = edited;
  Lock lock(source);
  FD pinned(source.Dup());
  Guard();
  if (Bytes(pinned.fd) != original)
    throw std::runtime_error(
        "File contents changed since opening. Your edit was not written.");
  if (original == encoded)
    return;

  Recovery recovery(path, original);
  Guard();
  if (Bytes(pinned.fd) != original || !recovery.AtPath())
    throw std::runtime_error(
        "The file or recovery copy changed before saving.");
  size_t written = 0;
  bool resized = false;
  try {
    WriteBytes(pinned.fd, encoded, written);
    if (ftruncate(pinned.fd, encoded.size()))
      Fail("Resize text file");
    resized = true;
    Sync(pinned.fd);
    Guard();
    if (Bytes(pinned.fd) != encoded)
      throw std::runtime_error("The file changed while saving.");
  } catch (const std::exception &failure) {
    // BNode::Lock blocks competing opens, not independently opened writers.
    // Only roll back a byte sequence our writes could have produced.
    recovery.keep = true;
    try {
      std::string expected = original;
      expected.replace(0, written, encoded, 0, written);
      if (resized)
        expected.resize(encoded.size());
      if (Bytes(pinned.fd) == expected) {
        size_t restored = 0;
        WriteBytes(pinned.fd, original, restored);
        if (!ftruncate(pinned.fd, original.size())) {
          Sync(pinned.fd);
          // Keep recovery if the original name no longer reaches the original node.
          Guard();
          recovery.keep = Bytes(pinned.fd) != original;
        }
      }
    } catch (const std::exception &) {
      // The synced original is the recovery path if restoration cannot be verified.
    }
    if (recovery.keep)
      throw std::runtime_error(
          std::string(failure.what()) +
          " Inspect before retrying. Original bytes: " + recovery.Path());
    throw;
  }
  original.swap(encoded);
  text.swap(newText);
}
}
