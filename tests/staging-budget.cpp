// SPDX-License-Identifier: MIT
// This opt-in case deliberately approaches the limits of a disposable BFS.
#include "StagingSpace.h"
#include "ArchiveCompressor.h"
#include <fstream>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
using namespace Listi;

int main() try {
  const auto *volume = getenv("LISTI_TEST_VOLUME");
  struct stat st, local;
  fs_info info;
  if (!volume || stat(volume, &st) || stat("/tmp", &local) ||
      st.st_dev == local.st_dev || fs_stat_dev(st.st_dev, &info) ||
      info.total_blocks * uint64_t(info.block_size) > 128ull * 1024 * 1024)
    throw std::runtime_error("Select a disposable distinct BFS volume of at most 128 MiB.");
  std::string directory = std::string(volume) + "/Listi-staging-budget-XXXXXX";
  if (!mkdtemp(directory.data()))
    return 2;
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
  } cleanup{directory};
  const int a = open((cleanup.path / "one").c_str(), O_CREAT | O_WRONLY | O_EXCL, 0600);
  const int b = open((cleanup.path / "two").c_str(), O_CREAT | O_WRONLY | O_EXCL, 0600);
  if (a < 0 || b < 0)
    return 2;
  // Hold one write at the admission boundary while another thread takes
  // its own reservation. A third volume must be independent.
  std::atomic<bool> entered{false}, done{false};
  std::thread other([&] {
    PendingStagingWrite pending(st.st_dev, 65536);
    entered = true;
    while (!done) std::this_thread::yield();
  });
  while (!entered) std::this_thread::yield();
  bool simultaneous;
  {
    PendingStagingWrite next(st.st_dev, 32768), independent(local.st_dev, 4096);
    simultaneous = next.total >= 98304 && independent.total == 4096;
  }
  done = true;
  other.join();
  if (!simultaneous) throw std::runtime_error("Concurrent admission failed");
  std::cout << "PASS same-volume in-flight writes accounted; other volume independent\n";
  const size_t block = info.block_size;
  std::vector<char> bytes(block, 'B');
  uint64_t written = 0;
  for (int fd = a;; fd = fd == a ? b : a) {
    const auto count = WriteStaging(fd, bytes.data(), bytes.size());
    if (count < 0 && errno == ENOSPC)
      break;
    if (count != ssize_t(block))
      throw std::runtime_error("Could not write a complete staged block.");
    written += count;
  }
  if (fsync(a) || fsync(b) || fs_stat_dev(st.st_dev, &info))
    return 2;
  const auto containerReserve =
      std::max<uint64_t>(1, std::min<uint64_t>(info.total_blocks / 400, 16 * 1024 * 1024 / block));
  // BFS also consumes metadata blocks; the next write cannot be attempted
  // after the guard reaches reserve, but allow one block of allocator overhead.
  const auto finalAttempt = WriteStaging(b, bytes.data(), bytes.size());
  std::cout << "Written " << written << " bytes, reserve " << containerReserve
            << " blocks, free " << info.free_blocks << ", next write "
            << finalAttempt << ", errno " << errno << '\n';
  if (!written || uint64_t(info.free_blocks) + 1 < containerReserve ||
      finalAttempt >= 0 || errno != ENOSPC)
    throw std::runtime_error("The combined output did not stop at the shared reserve.");

  // Keep both stages open: closing them can release preallocated BFS extents
  // and remove the low-space condition before the child even starts.
  // A separate child owns the next growth. Use the real codec process
  // monitor with a disposable writer and verify that it refuses exhausted
  // scratch, not just the data stream controlled by Listi.
  const auto old = cleanup.path / "existing";
  std::ofstream(old) << "original";
  std::string message;
  try {
    RunArchiveCompressor("pressure fixture", directory,
        {"/bin/dd", "if=/dev/zero", "of=extra", "bs=4096", "count=1024"},
        -1, {});
  } catch (const std::exception &error) { message = error.what(); }
  close(a);
  close(b);
  std::ifstream input(old);
  std::string original;
  input >> original;
  if (message.find("reserve") == std::string::npos || original != "original")
    throw std::runtime_error("External allocation did not stop codec safely: " + message);
  std::cout << "PASS external writer consuming disposable scratch stops codec; original unchanged\n";
  std::cout << "PASS " << written << " bytes written across two stages; "
            << info.free_blocks << " volume blocks remain reserved\n";
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
