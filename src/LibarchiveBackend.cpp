// SPDX-License-Identifier: MIT
#include "DynamicLibrary.h"
#include "ArchiveBackend.h"
#include "ArchiveTextLocale.h"
#include "ArchiveMetadata.h"
#include "ArchivePasswords.h"
#include "ArchiveVolumes.h"
#include <memory>
#include <algorithm>
#include <UTF8.h>
#include <archive.h>
#include <archive_entry.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace Listi {
namespace {
DynamicLibrary &Library() {
  // An optional system package may be installed while Listi is running.
  // Per-worker ownership avoids racing dlopen/replacement with another reader.
  thread_local std::unique_ptr<DynamicLibrary> lib;
  if (!lib || !lib->Available())
    lib.reset(new DynamicLibrary({"libarchive.so", "libarchive.so.13"}));
  return *lib;
}
struct Reader {
  DynamicLibrary &lib = Library();
  struct archive *handle = nullptr;
  int fd = -1;
  TransferControl control;
  std::exception_ptr failure;
  std::string path, identity;
  uint64_t passwordGeneration = ArchivePasswordGeneration();
  unsigned passwordAttempts = 0;
  ArchiveSecretPtr password;
  std::vector<char> buffer = std::vector<char>(131072);
  explicit Reader(const std::string &path, const TransferControl &c)
      : control(c), path(path) {
    if (!lib.Available())
      throw std::runtime_error(
          "Install libarchive from HaikuDepot to browse archives");
    fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
      throw std::runtime_error(path + ": " + strerror(errno));
    struct stat opened;
    if (fstat(fd, &opened) || !S_ISREG(opened.st_mode)) {
      close(fd);
      fd = -1;
      throw std::runtime_error("Archive input must be a regular file");
    }
    identity = ArchivePasswordIdentity(opened);
    handle = LIBCALL(lib, archive_read_new)();
    if (!handle) {
      close(fd);
      fd = -1;
      throw std::runtime_error("Could not allocate archive reader");
    }
    try {
      CheckOpenedArchiveVersion(path, identity, c);
      LIBCALL(lib, archive_read_set_passphrase_callback)
      (handle, this, &Passphrase);
      LIBCALL(lib, archive_read_support_filter_all)(handle);
      // Compressed single files must not be guessed as mtree (ordinary text
      // can look like an mtree manifest). Explicit tar containers still use
      // format detection; standalone compression exposes one decoded file.
      std::string lower = path;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char ch) { return std::tolower(ch); });
      auto dot = lower.find_last_of('.');
      auto extension = dot == std::string::npos ? "" : lower.substr(dot);
      auto stem = dot == std::string::npos ? lower : lower.substr(0, dot);
      bool compressed = extension == ".gz" || extension == ".bz2" ||
                        extension == ".xz" || extension == ".zst" ||
                        extension == ".lz" || extension == ".lzma" ||
                        extension == ".lz4" || extension == ".z";
      bool tar = stem.size() >= 4 && stem.substr(stem.size() - 4) == ".tar";
      if (!compressed || tar)
        LIBCALL(lib, archive_read_support_format_all)(handle);
      LIBCALL(lib, archive_read_support_format_raw)(handle);
      LIBCALL(lib, archive_read_set_seek_callback)(handle, &Seek);
      int result = LIBCALL(lib, archive_read_open)(handle, this, nullptr, &Read,
                                                   nullptr);
      Rethrow();
      if (result != ARCHIVE_OK)
        Error();
    } catch (...) {
      LIBCALL(lib, archive_read_free)(handle);
      handle = nullptr;
      close(fd);
      fd = -1;
      throw;
    }
  }
  ~Reader() {
    if (handle)
      LIBCALL(lib, archive_read_free)(handle);
    if (fd >= 0)
      close(fd);
  }
  static const char *Passphrase(struct archive *, void *data) {
    auto &self = *static_cast<Reader *>(data);
    try {
      self.control.Check();
      if (self.passwordAttempts >= 4)
        throw std::runtime_error(
            "Password not accepted. Reopen the archive to try again.");
      self.password = GetArchivePassword(self.path, self.identity,
                                         self.passwordAttempts++ != 0,
                                         self.passwordGeneration, self.control);
      struct stat current, named;
      if (fstat(self.fd, &current) || lstat(self.path.c_str(), &named) ||
          self.identity != ArchivePasswordIdentity(current) ||
          self.identity != ArchivePasswordIdentity(named))
        throw std::runtime_error(
            "Archive changed while entering its password. Open it again.");
      return self.password->Text();
    } catch (...) {
      self.failure = std::current_exception();
      return nullptr;
    }
  }
  static la_ssize_t Read(struct archive *, void *data, const void **out) {
    auto &self = *static_cast<Reader *>(data);
    try {
      self.control.Check();
      *out = self.buffer.data();
      ssize_t n;
      do {
        n = read(self.fd, self.buffer.data(), self.buffer.size());
      } while (n < 0 && errno == EINTR);
      if (n < 0)
        throw std::runtime_error(std::string("Archive read failed: ") +
                                 strerror(errno));
      return n;
    } catch (...) {
      self.failure = std::current_exception();
      return -1;
    }
  }
  static la_int64_t Seek(struct archive *, void *data, la_int64_t offset,
                         int whence) {
    auto &self = *static_cast<Reader *>(data);
    try {
      self.control.Check();
      off_t result = lseek(self.fd, offset, whence);
      if (result < 0)
        throw std::runtime_error(std::string("Archive seek failed: ") +
                                 strerror(errno));
      return result;
    } catch (...) {
      self.failure = std::current_exception();
      return -1;
    }
  }
  void Rethrow() {
    if (failure)
      std::rethrow_exception(failure);
  }
  [[noreturn]] void Error() {
    Rethrow();
    const char *s = LIBCALL(lib, archive_error_string)(handle);
    std::string error = s ? s : "Unsupported or damaged archive";
    if (error.find("unable to run program") != std::string::npos) {
      const char *package = error.find("zstd") != std::string::npos ? "zstd_bin"
                            : error.find("lz4") != std::string::npos ? "lz4"
                                                                     : nullptr;
      if (package)
        error =
            std::string("This compression needs the optional ") + package +
            " package. Install it from HaikuDepot, then open the archive again in Listi.";
    }
    if (error.find("encrypt") != std::string::npos ||
        error.find("Encrypt") != std::string::npos)
      error +=
          " This encryption is not supported by the installed archive engine. "
          "Install the optional p7zip package for encrypted 7z/RAR. ZIP header encryption needs another adapter.";
    throw std::runtime_error(error);
  }
  struct archive_entry *Next() {
    control.Check();
    struct archive_entry *entry = nullptr;
    int r = LIBCALL(lib, archive_read_next_header)(handle, &entry);
    Rethrow();
    if (r == ARCHIVE_EOF)
      return nullptr;
    if (r != ARCHIVE_OK)
      Error();
    const bool encrypted = LIBCALL(lib, archive_entry_is_encrypted)(entry) > 0;
    if (encrypted)
      ArchiveEditProtection::Observe(identity);
    if (encrypted &&
        (LIBCALL(lib, archive_format)(handle) & ARCHIVE_FORMAT_BASE_MASK) !=
            ARCHIVE_FORMAT_ZIP)
      throw std::runtime_error(
          "Encrypted 7z/RAR is not supported by this archive engine. "
          "Install the optional p7zip package from HaikuDepot, then reopen it in Listi.");
    return entry;
  }
  bool CompressedOrArchive() const {
    return (LIBCALL(lib, archive_format)(handle) != ARCHIVE_FORMAT_RAW &&
            LIBCALL(lib, archive_format)(handle) != ARCHIVE_FORMAT_EMPTY) ||
           LIBCALL(lib, archive_filter_code)(handle, 0) != ARCHIVE_FILTER_NONE;
  }
};
std::string EntryName(Reader &reader, struct archive_entry *entry,
                      const Location &location) {
  auto name =
      SafeArchivePath(LIBCALL(reader.lib, archive_entry_pathname)(entry));
  const int format = LIBCALL(reader.lib, archive_format)(reader.handle);
  // Keep Unicode extra fields and the unmarked UTF-8 names written by older
  // Listi releases. Only fall back to ZIP's original DOS encoding if the
  // archive engine cannot interpret the member name as UTF-8.
  if ((format & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_ZIP &&
      !name.empty() && !LIBCALL(reader.lib, archive_entry_pathname_utf8)(entry)) {
    int32 sourceSize = name.size(), size = sourceSize * 3, state = 0;
    std::string decoded(size, '\0');
    if (convert_to_utf8(B_MS_DOS_CONVERSION, name.data(), &sourceSize,
                        decoded.data(), &size, &state) != B_OK ||
        size_t(sourceSize) != name.size())
      throw std::runtime_error("Cannot decode ZIP entry name.");
    decoded.resize(size);
    return SafeArchivePath(decoded.c_str());
  }
  if (format == ARCHIVE_FORMAT_RAW) {
    name = LocationLeaf(location.archive);
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos)
      name.resize(dot);
    if (name.empty())
      name = "contents";
  }
  return name;
}
} // namespace
bool LibarchiveAvailable() { return Library().Available(); }
std::string LibarchiveOpen(const std::string &path, const TransferControl &c) {
  ArchiveTextLocale locale;
  if (IsVirtual(path))
    return "";
  bool expected = ArchiveExtension(path);
  if (!LibarchiveAvailable()) {
    if (expected)
      throw std::runtime_error(
          "Archive support is not installed. Install libarchive from HaikuDepot, "
          "then open this file again. Browsing and extraction stay in Listi.");
    return "";
  }
  try {
    Reader reader(path, c);
    reader.Next();
    if (!reader.CompressedOrArchive()) {
      if (expected)
        throw std::runtime_error(
            "The installed archive engine cannot read this format. "
            "It needs an additional Listi archive adapter; no external app was opened.");
      return "";
    }
    Location location;
    location.scheme = "archive";
    location.archive = path;
    location.path = "/";
    return location.URL();
  } catch (const Cancelled &) {
    throw;
  } catch (const std::exception &) {
    if (expected)
      throw;
    return "";
  }
}
std::vector<FileEntry> LibarchiveList(const Location &location,
                                      const TransferControl &c) {
  ArchiveMetadataBudget budget;
  return LibarchiveList(location, c, budget);
}
namespace {
// Each component is owned once under its immediate parent. An implicit
// folder can later be an explicit directory, but never an explicit file.
struct MemberPath {
  std::map<std::string, MemberPath, std::less<>> children;
  bool explicitEntry = false;
  bool file = false;

  void Insert(std::string_view path, bool directory,
              ArchiveMetadataBudget &budget) {
    MemberPath *node = this;
    for (size_t depth = 0;; ++depth) {
      const auto slash = path.find('/');
      const auto component = path.substr(0, slash);
      auto child = node->children.find(component);
      if (child == node->children.end()) {
        budget.Charge(sizeof(decltype(children)::value_type) +
                      ArchiveMetadataBudget::NodeOverhead +
                      2 * (component.size() + 1));
        child = node->children.try_emplace(std::string(component)).first;
      }
      node = &child->second;
      if (slash == std::string_view::npos)
        break;
      if (depth >= 64)
        throw std::runtime_error("Archive exceeds the safe folder depth");
      if (node->file)
        throw std::runtime_error("Archive has conflicting file and folder paths");
      path.remove_prefix(slash + 1);
    }
    if (node->explicitEntry)
      throw std::runtime_error(
          "Archive contains duplicate entry paths; nothing was extracted");
    if (!directory && !node->children.empty())
      throw std::runtime_error("Archive has conflicting file and folder paths");
    node->explicitEntry = true;
    node->file = !directory;
  }
};
} // namespace

std::vector<FileEntry> LibarchiveList(const Location &location,
                                     const TransferControl &c,
                                     ArchiveMetadataBudget &budget) {
  ArchiveTextLocale locale;
  Reader reader(location.archive, c);
  ArchiveListingChildren children(location, budget);
  size_t count = 0;
  MemberPath paths;
  while (auto entry = reader.Next()) {
    if (++count > 200000)
      throw std::runtime_error("Archive has too many entries for this view");
    const auto name = EntryName(reader, entry, location);
    if (name.empty())
      continue;
    const auto type = LIBCALL(reader.lib, archive_entry_filetype)(entry);
    paths.Insert(name, type == AE_IFDIR, budget);
    const auto rest = children.RelativeMember(name, type == AE_IFDIR);
    if (rest.empty())
      continue;
    const auto slash = rest.find('/');
    auto &item = children.Get(rest.substr(0, slash));
    bool implied = slash != std::string::npos;
    if (implied) {
      if (!item.directory)
        item.permissions = 0755;
      item.directory = true;
    } else if (type == AE_IFDIR) {
      item.directory = true;
      item.permissions = LIBCALL(reader.lib, archive_entry_perm)(entry) & 07777;
    } else {
      item.link = type == AE_IFLNK ||
                  LIBCALL(reader.lib, archive_entry_hardlink)(entry) != nullptr;
      item.size = LIBCALL(reader.lib, archive_entry_size)(entry);
      item.sizeKnown = LIBCALL(reader.lib, archive_entry_size_is_set)(entry);
      item.permissions = LIBCALL(reader.lib, archive_entry_perm)(entry) & 07777;
      if (type != AE_IFREG && !item.link)
        throw std::runtime_error("Archive contains a special device entry");
    }
    if (!implied) {
      if (LIBCALL(reader.lib, archive_entry_mtime_is_set)(entry))
        item.SetModification(LIBCALL(reader.lib, archive_entry_mtime)(entry),
                             LIBCALL(reader.lib, archive_entry_mtime_nsec)(entry));
    }
  }
  return children.Finish();
}
namespace {
// One open sequential reader per worker/job. Reuse avoids rescanning every
// preceding header for each file. Out-of-order requests rewind once, never
// silently miss entries. No reader or callback outlives the job scope.
thread_local std::unique_ptr<Reader> sequentialReader;
void VerifyReader(Reader &reader) {
  struct stat opened, named;
  reader.control.Check();
  CheckArchivePasswordGeneration(reader.passwordGeneration);
  if (fstat(reader.fd, &opened) || lstat(reader.path.c_str(), &named) ||
      ArchivePasswordIdentity(opened) != reader.identity ||
      ArchivePasswordIdentity(named) != reader.identity)
    throw std::runtime_error(
        "Archive changed during reading. Source entries were retained.");
}
}
void ClearLibarchiveReadCache() { sequentialReader.reset(); }
void LibarchiveRead(const Location &location, const DataSink &sink,
                    const TransferControl &c) {
  ArchiveTextLocale locale;
  const bool scoped = ArchiveListingScopeActive();
  std::unique_ptr<Reader> local;
  auto &slot = scoped ? sequentialReader : local;
  bool reused = false;
  try {
    c.Check();
    struct stat named;
    if (lstat(location.archive.c_str(), &named) || !S_ISREG(named.st_mode))
      throw std::runtime_error("Archive input is unavailable.");
    // Reuse only formats whose streaming state has been qualified. In
    // particular XAR's data/TOC cursor cannot safely serve this access pattern.
    int format = slot ? LIBCALL(slot->lib, archive_format)(slot->handle) &
                            ARCHIVE_FORMAT_BASE_MASK
                      : 0;
    bool sequential = format == ARCHIVE_FORMAT_ZIP ||
                      format == ARCHIVE_FORMAT_TAR ||
                      format == ARCHIVE_FORMAT_CPIO;
    if (slot && sequential && slot->path == location.archive &&
        slot->identity == ArchivePasswordIdentity(named) &&
        slot->passwordGeneration == ArchivePasswordGeneration()) {
      CheckOpenedArchiveVersion(location.archive, slot->identity, c);
      slot->control = c;
      reused = true;
    } else
      slot.reset(new Reader(location.archive, c));
    for (;;) {
      auto &reader = *slot;
      VerifyReader(reader);
      const std::string wanted = location.path.substr(1);
      while (auto entry = reader.Next()) {
        if (EntryName(reader, entry, location) != wanted)
          continue;
        if (LIBCALL(reader.lib, archive_entry_filetype)(entry) != AE_IFREG ||
            LIBCALL(reader.lib, archive_entry_hardlink)(entry))
          throw std::runtime_error(
              "Archive links and special entries are not extracted");
        std::vector<char> data(131072);
        for (;;) {
          c.Check();
          la_ssize_t n = LIBCALL(reader.lib, archive_read_data)(
              reader.handle, data.data(), data.size());
          reader.Rethrow();
          if (n < 0)
            reader.Error();
          if (!n) {
            VerifyReader(reader);
            RememberArchivePassword(reader.identity, reader.password,
                                    reader.passwordGeneration);
            // Callback closures often capture a stack frame belonging to the
            // current transfer. Clear them before retaining the reader.
            reader.control = {};
            return;
          }
          sink(data.data(), n);
          c.Tick(n);
        }
      }
      VerifyReader(reader);
      if (!reused)
        break;
      slot.reset(new Reader(location.archive, c));
      reused = false;
    }
    throw std::runtime_error("Archive entry no longer exists");
  } catch (...) {
    slot.reset();
    throw;
  }
}
} // namespace Listi
