// SPDX-License-Identifier: MIT
#include "MessageStore.h"
#include <DataIO.h>
#include "StagingSpace.h"
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
namespace Listi {
namespace {
constexpr size_t MaxFile = 1024 * 1024;
void Fail() { throw std::runtime_error(strerror(errno)); }
struct DirectoryFd {
  DirectoryFd(const std::string &path, bool noFollow)
      : fd(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | (noFollow ? O_NOFOLLOW : 0))) {
    if (fd < 0)
      Fail();
  }
  ~DirectoryFd() { close(fd); }
  const int fd;
};
}
BMessage LoadMessageStore(const std::string &file, bool noFollow) {
  int fd = open(file.c_str(), O_RDONLY | (noFollow ? O_NOFOLLOW : 0) | O_NONBLOCK);
  if (fd < 0) {
    if (errno == ENOENT) {
      BMessage absent;
      absent.what = 'none';
      return absent;
    }
    Fail();
  }
  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
      st.st_size > off_t(MaxFile)) {
    close(fd);
    throw std::runtime_error("Invalid settings file.");
  }
  std::vector<char> bytes(st.st_size);
  size_t at = 0;
  while (at < bytes.size()) {
    ssize_t n = read(fd, bytes.data() + at, bytes.size() - at);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      close(fd);
      throw std::runtime_error("Could not read settings.");
    }
    at += n;
  }
  close(fd);
  BMemoryIO input(bytes.data(), bytes.size());
  BMessage message;
  if (message.Unflatten(&input) != B_OK)
    throw std::runtime_error("Could not decode settings.");
  if (message.what == 'none')
    throw std::runtime_error("Invalid settings message.");
  return message;
}
void SaveMessageStore(const std::string &file, const BMessage &message,
                      const struct stat *expected, struct stat *stored,
                      const struct stat *inventory) {
  ssize_t size = message.FlattenedSize();
  if (size <= 0 || size > ssize_t(MaxFile))
    throw std::runtime_error("Settings are too large.");
  std::vector<char> bytes(size);
  if (message.Flatten(bytes.data(), size) != B_OK)
    throw std::runtime_error("Could not encode settings.");
  const auto slash = file.rfind('/');
  const std::string folder = slash == std::string::npos ? "." :
      slash == 0 ? "/" : file.substr(0, slash);
  const std::string target = file.substr(slash + 1);
  DirectoryFd parent(folder, inventory != nullptr);
  if (inventory) {
    struct stat st;
    if (fstat(parent.fd, &st) || st.st_dev != inventory->st_dev ||
        st.st_ino != inventory->st_ino)
      throw std::runtime_error("Recovery inventory directory changed.");
  }
  struct stat original;
  const bool existed = !fstatat(parent.fd, target.c_str(), &original, AT_SYMLINK_NOFOLLOW);
  if (!existed && errno != ENOENT)
    Fail();
  std::string pattern = file + ".tmp.XXXXXX";
  std::vector<char> temp(pattern.begin(), pattern.end());
  temp.push_back(0);
  int fd = mkstemp(temp.data());
  if (fd < 0)
    Fail();
  const std::string leaf = std::string(temp.data()).substr(slash + 1);
  struct stat opened, named;
  const bool identified = fstat(fd, &opened) == 0;
  auto sameStage = [&] {
    return identified &&
           !fstatat(parent.fd, leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW) &&
           opened.st_dev == named.st_dev && opened.st_ino == named.st_ino;
  };
  try {
    if (!sameStage())
      throw std::runtime_error("Settings staging directory changed.");
    size_t at = 0;
    while (at < bytes.size()) {
      ssize_t n = WriteStaging(fd, bytes.data() + at, bytes.size() - at);
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0)
        Fail();
      if (!n)
        throw std::runtime_error("Could not write settings.");
      at += n;
    }
    if (fsync(fd))
      Fail();
    if (close(fd)) {
      fd = -1;
      Fail();
    }
    fd = -1;
    struct stat savedParent, namedParent;
    if (!sameStage() || fstat(parent.fd, &savedParent) ||
        stat(folder.c_str(), &namedParent) || savedParent.st_dev != namedParent.st_dev ||
        savedParent.st_ino != namedParent.st_ino)
      throw std::runtime_error("Settings directory or temporary file changed.");
    const bool exists = !fstatat(parent.fd, target.c_str(), &named, AT_SYMLINK_NOFOLLOW);
    if ((!exists && errno != ENOENT) || existed != exists ||
        (existed && (original.st_dev != named.st_dev || original.st_ino != named.st_ino ||
                     original.st_ctim.tv_sec != named.st_ctim.tv_sec ||
                     original.st_ctim.tv_nsec != named.st_ctim.tv_nsec)))
      throw std::runtime_error("Settings file changed; replacement refused.");
    if (expected && (!exists || !S_ISREG(named.st_mode) || expected->st_dev != named.st_dev ||
                     expected->st_ino != named.st_ino || expected->st_crtime != named.st_crtime))
      throw std::runtime_error("Recovery record changed; replacement refused.");
    if (renameat(parent.fd, leaf.c_str(), parent.fd, target.c_str()))
      Fail();
  } catch (...) {
    if (fd >= 0)
      close(fd);
    if (sameStage())
      unlinkat(parent.fd, leaf.c_str(), 0);
    throw;
  }
  if (stored)
    *stored = opened;
  if (fsync(parent.fd))
    throw SettingsReplacedError(
        std::string("Settings were replaced, but their directory could not be flushed: ") +
        strerror(errno));
}
} // namespace Listi
