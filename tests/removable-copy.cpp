// SPDX-License-Identifier: MIT
// Park a staged-file flush without needing a particular USB driver failure.
#include "RemovableCopy.h"
#include "UnavailableVolume.h"
#ifdef __HAIKU__
#include <Errors.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
std::mutex mutex;
std::condition_variable changed;
bool entered = false, release = false, returned = false;
std::atomic<int> park{-1}, failRead{-1};
#ifdef __HAIKU__
constexpr int kSimulatedDiskError = B_DEV_NOT_READY;
#else
constexpr int kSimulatedDiskError = EIO;
#endif
bool stallWrite = false;
std::atomic<int> stallRead{-1};
std::atomic<int> writeCalls{0};
void Hold() {
  std::unique_lock<std::mutex> lock(mutex);
  entered = true;
  changed.notify_all();
  changed.wait(lock, [] { return release; });
  returned = true;
  changed.notify_all();
}

void Require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
  std::cout << "PASS " << message << std::endl;
}
}

extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  if (fd == park.load() && !stallWrite && stallRead < 0)
    Hold();
  return __real_fsync(fd);
}

extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" ssize_t __wrap_write(int fd, const void *bytes, size_t count) {
  if (fd == park.load() && stallWrite) {
    ++writeCalls;
    Hold();
    return __real_write(fd, bytes, std::min(count, size_t(1)));
  }
  return __real_write(fd, bytes, count);
}

extern "C" ssize_t __real_read(int, void *, size_t);
extern "C" ssize_t __wrap_read(int fd, void *bytes, size_t count) {
  if (fd == stallRead.load())
    Hold();
  if (fd == failRead.load()) {
    errno = kSimulatedDiskError;
    return -1;
  }
  return __real_read(fd, bytes, count);
}

int main() {
  char directory[] = "/tmp/Listi-usb-XXXXXX";
  if (!mkdtemp(directory))
    return 1;
  std::filesystem::path root(directory);
  const auto source = root / "source";
  const auto staging = root / "stage";
  std::string payload(350000, 'A');
  std::ofstream(source, std::ios::binary) << payload;
  auto copy = [&](const std::filesystem::path &path, bool stall, bool reading = false) {
    struct stat before;
    int in = open(source.c_str(), O_RDONLY);
    int out = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (in < 0 || out < 0 || fstat(in, &before))
      throw std::runtime_error("could not prepare descriptors");
    if (stall) {
      park = out;
      if (reading)
        stallRead = in;
    }
    size_t bytes = 0;
    Listi::WriteRemovableStagingFile(in, out, before, [&](size_t count) {
      bytes += count;
      std::lock_guard<std::mutex> lock(mutex);
      if (stall && entered)
        throw std::runtime_error("simulated media disconnect");
    });
    return bytes;
  };
  for (int fault : {0, 1, 2}) {
    const bool blockWrite = fault == 1, blockRead = fault == 2;
    stallWrite = blockWrite;
    entered = release = returned = false;
    bool stopped = false;
    try {
      copy(staging, true, blockRead);
    } catch (const Listi::RemovableCopyInterrupted &) {
      stopped = true;
    }
    Require(stopped, "blocked USB I/O returns control before the kernel call returns");
    Require(copy(root / "other", false) == payload.size(),
            "another transfer reports all bytes while the old I/O is still blocked");
    // Replace the *name*, not the inode: a late completion must not open,
    // rewrite or publish through that name.
    std::filesystem::rename(staging, root / "old-inode");
    std::ofstream(staging) << "new stage";
    {
      std::unique_lock<std::mutex> lock(mutex);
      release = true;
      changed.notify_all();
      Require(changed.wait_for(lock, 2s, [] { return returned; }),
              "stalled writer can unwind after the device returns");
    }
    for (int i = 0; i != 200 && fcntl(park, F_GETFD) != -1; ++i)
      std::this_thread::sleep_for(5ms);
    Require(fcntl(park, F_GETFD) == -1, "abandoned stage descriptor was closed");
    auto text = [](const std::filesystem::path &path) {
      std::ifstream input(path, std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(input), {});
    };
    Require(text(staging) == "new stage" && text(source) == payload &&
            text(root / "other") == payload,
            "late return does not affect the new inode, source or independent copy");
    if (blockWrite)
      Require(writeCalls == 1, "short write returning after cancellation is never retried");
    std::filesystem::remove(staging);
    std::filesystem::remove(root / "other");
    std::filesystem::remove(root / "old-inode");
    park = -1;
    stallRead = -1;
  }
  int in = open(source.c_str(), O_RDONLY);
  int out = open((root / "cancel").c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
  struct stat before;
  Require(in >= 0 && out >= 0 && fstat(in, &before) == 0, "prepare connected cancellation");
  struct Cancel {};
  bool cancelled = false;
  try {
    Listi::WriteRemovableStagingFile(in, out, before, [](size_t count) {
      if (count)
        throw Cancel();
    });
  } catch (const Cancel &) {
    cancelled = true;
  }
  Require(cancelled && fcntl(out, F_GETFD) == -1 &&
              std::filesystem::file_size(root / "cancel") < payload.size(),
          "cancellation at a chunk boundary stops writes and closes staging before return");
  in = open(source.c_str(), O_RDONLY);
  out = open((root / "read-error").c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
  Require(in >= 0 && out >= 0 && fstat(in, &before) == 0, "prepare read failure");
  failRead = in;
  bool interrupted = false;
  try {
    Listi::WriteRemovableStagingFile(in, out, before, [](size_t) {});
  } catch (const Listi::RemovableCopyInterrupted &) {
    interrupted = true;
  }
  failRead = -1;
  Require(interrupted && fcntl(out, F_GETFD) == -1 &&
              std::filesystem::file_size(root / "read-error") == 0,
          "removable read failure suspends the endpoint before devfs notices");
  Require(!Listi::IsRemovableIOError(ENOSPC) && !Listi::IsRemovableIOError(EACCES),
          "ordinary space and permission errors do not suspend a volume");
  Listi::UnavailableVolumes volumes;
  volumes.Block("/USB Disk/", 123);
  Require(volumes.Contains("/USB Disk") && volumes.Contains("//USB Disk/./other") &&
              volumes.Contains("/USB Disk/../boot/home") &&
              volumes.Contains("archive:///USB%20Disk/test.zip!/") &&
              volumes.ContainsDevice(123) && !volumes.ContainsDevice(124) &&
              !volumes.Contains("/USB Disk backup") && !volumes.Contains("/boot/home") &&
              !volumes.Contains("sftp://example/USB%20Disk"),
          "a detached volume covers alternate spellings, descendants and archive backings, but not independent paths");
  const auto alias = root / "alias";
  std::filesystem::create_directory_symlink("/USB Disk", alias);
  Require(volumes.ContainsResolved((alias / "subdirectory").string()) &&
              !volumes.ContainsResolved(root.string()),
          "a local symlink to a detached volume is recognized without querying its target");
  std::filesystem::remove_all(root);
}
