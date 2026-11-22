// SPDX-License-Identifier: MIT
// Park a staged-file flush without needing a particular USB driver failure.
#include "RemovableCopy.h"
#include <algorithm>
#include <atomic>
#include <chrono>
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
std::atomic<int> park{-1};
bool stallWrite = false;
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
  if (fd == park.load() && !stallWrite)
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

int main() {
  char directory[] = "/tmp/Listi-usb-XXXXXX";
  if (!mkdtemp(directory))
    return 1;
  std::filesystem::path root(directory);
  const auto source = root / "source";
  const auto staging = root / "stage";
  std::string payload(350000, 'A');
  std::ofstream(source, std::ios::binary) << payload;
  auto copy = [&](const std::filesystem::path &path, bool stall) {
    struct stat before;
    int in = open(source.c_str(), O_RDONLY);
    int out = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (in < 0 || out < 0 || fstat(in, &before))
      throw std::runtime_error("could not prepare descriptors");
    if (stall)
      park = out;
    size_t bytes = 0;
    Listi::WriteRemovableStagingFile(in, out, before, [&](size_t count) {
      bytes += count;
      std::lock_guard<std::mutex> lock(mutex);
      if (stall && entered)
        throw std::runtime_error("simulated media disconnect");
    });
    return bytes;
  };
  for (bool blockWrite : {false, true}) {
    stallWrite = blockWrite;
    entered = release = returned = false;
    bool stopped = false;
    try {
      copy(staging, true);
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
  std::filesystem::remove_all(root);
}
