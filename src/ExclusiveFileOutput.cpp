// SPDX-License-Identifier: MIT
#include "ExclusiveFileOutput.h"
#include "NamedEntry.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace Listi {
namespace {
[[noreturn]] void Fail(const std::string &operation) {
  throw std::runtime_error(operation + ": " + strerror(errno));
}

bool SameOutput(const struct stat &entry, const struct stat &expected) {
  return entry.st_dev == expected.st_dev && entry.st_ino == expected.st_ino;
}
}

void WriteOutputBytes(int fd, const char *data, size_t size) {
  for (size_t offset = 0; offset < size;) {
    const ssize_t n = write(fd, data + offset, size - offset);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      Fail("Write output file");
    if (n == 0)
      throw std::runtime_error("Writing the output file made no progress.");
    offset += n;
  }
}

void SaveExclusiveFile(int folder, const std::string &name, mode_t mode,
                       const std::function<void(int)> &write,
                       const std::function<void()> &verifyFolder) {
  ValidateEntryLeaf(name);
  const char *leaf = name.c_str();
  // BFS has neither hard links nor atomic no-replace rename.
  const int fd = openat(folder, leaf,
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        mode);
  if (fd < 0) {
    if (errno == EEXIST)
      throw std::runtime_error(name + ": already exists. No file was replaced.");
    Fail("Create " + name);
  }
  struct stat identity;
  if (fstat(fd, &identity)) {
    const int error = errno;
    close(fd);
    // Without the new file's identity, removing this name is not safe.
    throw std::runtime_error(name + ": could not identify new file: " +
                             strerror(error) + ". Check for an empty file.");
  }
  bool closeAttempted = false;
  try {
    write(fd);
    if (fsync(fd))
      Fail("Flush " + name);
    closeAttempted = true;
    if (close(fd))
      Fail("Close " + name);
  } catch (...) {
    // Keep the created inode pinned while checking whether it can be removed.
    // A close error is already a close attempt; never close that fd again.
    struct stat current;
    const int inspectError =
        fstatat(folder, leaf, &current, AT_SYMLINK_NOFOLLOW) ? errno : 0;
    const bool same = !inspectError && SameOutput(current, identity);
    const int cleanupError = same && unlinkat(folder, leaf, 0) ? errno : 0;
    if (!closeAttempted)
      close(fd);
    if (cleanupError) {
      errno = cleanupError;
      Fail("Could not remove incomplete output " + name);
    }
    if (!same && inspectError != 0 && inspectError != ENOENT) {
      errno = inspectError;
      Fail("Could not inspect incomplete output " + name);
    }
    if (same && fsync(folder))
      Fail("Could not flush removal of incomplete output " + name);
    throw;
  }

  // Retain completed bytes even if their name changed or directory sync
  // fails. In those cases we cannot confirm a durable save at the given path.
  if (fsync(folder))
    Fail(name + ": bytes written, but the destination folder could not be "
                "flushed; check before retrying");
  verifyFolder();
  struct stat current;
  if (fstatat(folder, leaf, &current, AT_SYMLINK_NOFOLLOW) ||
      !SameOutput(current, identity))
    throw std::runtime_error(name + ": saved file was moved or replaced.");
}
}
