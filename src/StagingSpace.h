// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __HAIKU__
#include <fs_info.h>
#else
#include <sys/statvfs.h>
#endif

namespace Listi {
// Sample the shared volume, not just the current file: nested stages and
// other jobs consume the same available blocks. Keep a small reserve relative
// to volume capacity (0.25%, capped at 16 MiB, at least one volume block).
inline bool StagingSpaceAvailable(int fd, uint64_t growth) {
  uint64_t blocks, free, block;
#ifdef __HAIKU__
  struct stat st;
  fs_info volume;
  if (fstat(fd, &st) || fs_stat_dev(st.st_dev, &volume))
    return false;
  blocks = volume.total_blocks;
  free = std::max<off_t>(0, volume.free_blocks);
  block = volume.block_size;
#else
  struct statvfs volume;
  if (fstatvfs(fd, &volume))
    return false;
  blocks = volume.f_blocks;
  free = volume.f_bavail;
  block = volume.f_frsize;
#endif
  if (block == 0) {
    errno = EIO;
    return false;
  }
  const auto reserve = std::max<uint64_t>(
      1, std::min<uint64_t>(blocks / 400, 16 * 1024 * 1024 / block));
  auto needed = growth / block + (growth % block != 0);
#ifdef __HAIKU__
  // BFS may preallocate beyond the byte count, and allocate extent metadata.
  // Allow another 256 KiB for those allocations on a growing write.
  if (growth && needed < std::numeric_limits<uint64_t>::max())
    needed += std::min<uint64_t>(std::numeric_limits<uint64_t>::max() - needed,
                                 std::max<uint64_t>(1, 256 * 1024 / block));
#endif
  if (free < reserve || needed > free - reserve) {
    errno = ENOSPC;
    return false;
  }
  return true;
}

// Include writes already admitted by this process but not yet visible in the
// filesystem free-block counter. No lock is held across filesystem calls: an
// unplugged disk must not stall an unrelated volume.
class PendingStagingWrite {
public:
  PendingStagingWrite(dev_t volume, size_t bytes) : volume(volume), bytes(bytes) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto *item = head; item; item = item->next) {
      if (item->volume != volume) continue;
      Add(item->bytes);
#ifdef __HAIKU__
      Add(256 * 1024); // Additional concurrent BFS allocation allowance.
#endif
    }
    Add(bytes);
    next = head;
    head = this;
  }
  ~PendingStagingWrite() {
    std::lock_guard<std::mutex> lock(mutex);
    auto **link = &head;
    while (*link != this)
      link = &(*link)->next;
    *link = next;
  }
  PendingStagingWrite(const PendingStagingWrite &) = delete;
  uint64_t total = 0;
private:
  void Add(uint64_t value) {
    total += std::min(value, std::numeric_limits<uint64_t>::max() - total);
  }
  dev_t volume;
  uint64_t bytes;
  PendingStagingWrite *next;
  static inline std::mutex mutex;
  static inline PendingStagingWrite *head = nullptr;
};

inline ssize_t WriteStaging(int fd, const void *data, size_t size) {
  struct stat st;
  if (fstat(fd, &st))
    return -1;
  PendingStagingWrite pending(st.st_dev, size);
  if (!StagingSpaceAvailable(fd, pending.total))
    return -1;
  return write(fd, data, size);
}

inline bool StagingGrowthAvailable(int fd, uint64_t end) {
  struct stat st;
  return !fstat(fd, &st) && st.st_size >= 0 &&
         StagingSpaceAvailable(fd, end > uint64_t(st.st_size)
                                       ? end - uint64_t(st.st_size) : 0);
}
} // namespace Listi
