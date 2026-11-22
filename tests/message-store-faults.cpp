// SPDX-License-Identifier: MIT
#include "MessageStore.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
enum class Fault { None, Write, Sync, Close, Rename, DirectorySync };
Fault fault = Fault::None;
int stagedFd = -1;
std::string destination;

int Fail() {
  errno = EIO;
  return -1;
}
} // namespace

extern "C" int __real_mkstemp(char *);
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" int __real_fsync(int);
extern "C" int __real_close(int);
extern "C" int __real_renameat(int, const char *, int, const char *);

extern "C" int __wrap_mkstemp(char *path) {
  int fd = __real_mkstemp(path);
  if (std::string(path).find(destination + ".tmp.") == 0)
    stagedFd = fd;
  return fd;
}
extern "C" ssize_t __wrap_write(int fd, const void *data, size_t size) {
  return fd == stagedFd && fault == Fault::Write ? Fail()
                                                 : __real_write(fd, data, size);
}
extern "C" int __wrap_fsync(int fd) {
  return (fd == stagedFd ? fault == Fault::Sync : fault == Fault::DirectorySync)
             ? Fail()
             : __real_fsync(fd);
}
extern "C" int __wrap_close(int fd) {
  int status = __real_close(fd);
  if (fd == stagedFd) {
    stagedFd = -1;
    if (fault == Fault::Close)
      return Fail();
  }
  return status;
}
extern "C" int __wrap_renameat(int from, const char *oldPath, int to,
                                const char *newPath) {
  return fault == Fault::Rename && std::string(newPath) == "settings"
             ? Fail()
             : __real_renameat(from, oldPath, to, newPath);
}

int main() {
  char root[] = "/tmp/listi-store-XXXXXX";
  if (!mkdtemp(root))
    return 2;
  destination = std::string(root) + "/settings";
  BMessage original('orig'), update('next');
  Listi::SaveMessageStore(destination, original);
  for (auto failure :
       {Fault::Write, Fault::Sync, Fault::Close, Fault::Rename}) {
    fault = failure;
    try {
      Listi::SaveMessageStore(destination, update);
      std::cerr << "Expected publication to fail at " << int(failure) << '\n';
      return 1;
    } catch (const std::runtime_error &) {
    }
    fault = Fault::None;
    if (Listi::LoadMessageStore(destination).what != original.what)
      return 1;
  }
  fault = Fault::DirectorySync;
  try {
    Listi::SaveMessageStore(destination, update);
    return 1;
  } catch (const std::runtime_error &error) {
    if (std::string(error.what()).find("were replaced") == std::string::npos ||
        Listi::LoadMessageStore(destination).what != update.what)
      return 1;
  }
  fault = Fault::None;
  Listi::SaveMessageStore(destination, update);
  if (Listi::LoadMessageStore(destination).what != update.what ||
      unlink(destination.c_str()) != 0 || rmdir(root) != 0)
    return 1; // Also rejects leftover temporary files.
  std::cout
      << "PASS settings preserve old bytes before rename, report post-rename flush failure, and retry\n";
  return 0;
}
