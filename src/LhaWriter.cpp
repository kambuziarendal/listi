// SPDX-License-Identifier: MIT
#include "LhaWriter.h"
#include "StagingSpace.h"
#include "ArchiveBackend.h"
#include "ArchiveCompressor.h"
#include "ArchiveTextLocale.h"
#include "DynamicLibrary.h"
#include "LocalMoveTree.h"

#include <archive.h>
#include <archive_entry.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace Listi {
namespace {
namespace fs = std::filesystem;
constexpr const char *LhaTool = "/boot/system/bin/lha";

void Require(bool condition, const std::string &error) {
  if (!condition)
    throw std::runtime_error(error);
}

struct FD {
  int value;
  ~FD() {
    if (value >= 0)
      close(value);
  }
};

struct Scratch {
  std::string path;
  Scratch() {
    char pattern[] = "/tmp/listi-lha-XXXXXX";
    Require(mkdtemp(pattern), "Cannot create LHA staging folder.");
    path = pattern;
  }
  ~Scratch() {
    std::error_code error;
    for (fs::recursive_directory_iterator it(path, error), end;
         !error && it != end; it.increment(error)) {
      if (fs::is_directory(it->symlink_status(error)))
        chmod(it->path().c_str(), 0700);
    }
    fs::remove_all(path, error);
  }
};

ssize_t ReadChunk(int fd, void *data, size_t length) {
  ssize_t result;
  do {
    result = read(fd, data, length);
  } while (result < 0 && errno == EINTR);
  Require(result >= 0, "Cannot read LHA verification data.");
  return result;
}

void Verify(const std::string &working, const std::string &archivePath,
            const TransferControl &control) {
  std::map<std::string, struct stat> expected;
  for (const auto &item : fs::recursive_directory_iterator(working)) {
    control.Check();
    auto path = item.path().string();
    struct stat entry;
    Require(!lstat(path.c_str(), &entry) &&
                (S_ISREG(entry.st_mode) || S_ISDIR(entry.st_mode)),
            "LHA staging contains a link or unsupported file.");
    auto name = path.substr(working.size() + 1);
    Require(SafeArchivePath(name.c_str()) == name,
            "LHA staging contains an unsupported name.");
    expected.emplace(name, entry);
  }

  ArchiveTextLocale locale;
  DynamicLibrary lib{"libarchive.so", "libarchive.so.13"};
  auto freeArchive = LIBCALL(lib, archive_read_free);
  std::unique_ptr<archive, decltype(freeArchive)> reader(
      LIBCALL(lib, archive_read_new)(), freeArchive);
  Require(reader &&
              LIBCALL(lib, archive_read_support_format_lha)(reader.get()) ==
                  ARCHIVE_OK &&
              LIBCALL(lib, archive_read_open_filename)(
                  reader.get(), archivePath.c_str(), 65536) == ARCHIVE_OK,
          "Cannot reopen LHA output for verification.");

  auto next = LIBCALL(lib, archive_read_next_header);
  auto readData = LIBCALL(lib, archive_read_data);
  auto pathname = LIBCALL(lib, archive_entry_pathname);
  auto mode = LIBCALL(lib, archive_entry_mode);
  auto modified = LIBCALL(lib, archive_entry_mtime);
  auto memberSize = LIBCALL(lib, archive_entry_size);
  auto hardlink = LIBCALL(lib, archive_entry_hardlink);
  std::vector<char> decoded(65536), original(decoded.size());
  for (;;) {
    control.Check();
    archive_entry *entry = nullptr;
    const auto status = next(reader.get(), &entry);
    if (status == ARCHIVE_EOF)
      break;
    if (status != ARCHIVE_OK) {
      const char *error = LIBCALL(lib, archive_error_string)(reader.get());
      throw std::runtime_error(
          std::string("Cannot read back the LHA archive header: ") +
          (error ? error : "Unknown error"));
    }
    const auto name = SafeArchivePath(pathname(entry));
    const auto found = expected.find(name);
    Require(found != expected.end(), "LHA output has a different member set.");
    const auto &source = found->second;
    Require(
        !hardlink(entry) &&
            (mode(entry) & (S_IFMT | 0777)) ==
                (source.st_mode & (S_IFMT | 0777)) &&
            modified(entry) == source.st_mtim.tv_sec,
        "LHA could not preserve the member type, permissions or timestamp: " +
            name);
    if (S_ISREG(source.st_mode)) {
      Require(memberSize(entry) == source.st_size,
              "LHA output has an incorrect member size: " + name);
      FD input{open(Join(working, name).c_str(),
                    O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
      Require(input.value >= 0, "Cannot reopen an LHA verification source.");
      for (;;) {
        control.Check();
        const auto decodedSize =
            readData(reader.get(), decoded.data(), decoded.size());
        Require(decodedSize >= 0, "Cannot decode LHA output: " + name);
        size_t offset = 0;
        while (offset < size_t(decodedSize)) {
          const auto count =
              ReadChunk(input.value, original.data(), decodedSize - offset);
          Require(count > 0 && memcmp(decoded.data() + offset, original.data(),
                                      count) == 0,
                  "LHA output does not match the source bytes: " + name);
          offset += count;
        }
        if (!decodedSize) {
          Require(ReadChunk(input.value, original.data(), 1) == 0,
                  "LHA output ended before the source: " + name);
          break;
        }
      }
    }
    expected.erase(found);
  }
  Require(expected.empty(), "LHA output is missing archive members.");
}

void CopyOutput(const std::string &path, int outputFd,
                const TransferControl &control) {
  FD input{open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
  Require(input.value >= 0, "Cannot open the verified LHA output.");
  std::vector<char> bytes(65536);
  for (;;) {
    control.Check();
    auto count = ReadChunk(input.value, bytes.data(), bytes.size());
    if (!count)
      break;
    for (ssize_t offset = 0; offset < count;) {
      auto amount = WriteStaging(outputFd, bytes.data() + offset, count - offset);
      if (amount < 0 && errno == EINTR)
        continue;
      Require(amount > 0, "Cannot write the verified LHA output.");
      offset += amount;
    }
  }
  control.Check();
}
} // namespace

bool LhaCanCreate() {
  return access(LhaTool, X_OK) == 0 && LibarchiveAvailable();
}

void LhaCreate(int outputFd, const std::vector<std::string> &sources, int level,
               const Hooks &hooks) {
  Require(LhaCanCreate(),
          "Install the optional lha and libarchive packages from HaikuDepot.");
  Require(!sources.empty(), "LHA creation requires at least one entry.");
  Require(level >= 0 && level <= 9, "Invalid LHA compression level.");
  TransferControl control{hooks.proceed, {}};
  control.Check();

  Scratch scratch;
  const std::string canonical = fs::canonical(scratch.path).string();
  for (const auto &source : sources) {
    const std::string root = fs::canonical(source).string();
    Require(root != "/" && canonical.compare(0, root.size() + 1, root + "/"),
            "Cannot archive a folder that contains the LHA staging folder.");
  }
  LocalMoveTree frozen(sources, hooks, false);
  Job stage;
  stage.sources = sources;
  stage.destination = Join(scratch.path, "members");
  Require(mkdir(stage.destination.c_str(), 0700) == 0,
          "Cannot create LHA member staging folder.");

  std::set<std::string> roots;
  const auto archivePath = Join(scratch.path, "output.lzh");
  std::vector<std::string> args{LhaTool, level ? "cq2o7" : "cq2z",
                                "--system-kanji-code=utf8",
                                "--archive-kanji-code=utf8", archivePath};
  for (const auto &source : sources) {
    const auto name = Leaf(source);
    Require(!name.empty() && SafeArchivePath(name.c_str()) == name &&
                roots.insert(name).second,
            "LHA source names are invalid or not unique.");
    args.emplace_back("./" + name);
  }
  Hooks copying;
  copying.proceed = hooks.proceed;
  copying.progress = hooks.progress;
  const auto copied = Run(stage, copying);
  if (copied.cancelled)
    throw Cancelled();
  Require(copied.errors.empty(),
          copied.errors.empty() ? "" : copied.errors.front());
  Require(copied.completed == int(sources.size()) && !copied.skipped,
          "Could not stage all LHA sources.");

  // Check the staged tree before the external tool can follow any path in it.
  LocalMoveTree staged({stage.destination}, hooks, false);
  frozen.Verify();
  RunArchiveCompressor("LHA", stage.destination, std::move(args), outputFd,
                       control);
  staged.Verify();
  Verify(stage.destination, archivePath, control);
  CopyOutput(archivePath, outputFd, control);
  frozen.Verify();
}
} // namespace Listi
