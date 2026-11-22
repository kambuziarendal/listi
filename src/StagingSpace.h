// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
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
  const auto needed = growth / block + (growth % block != 0);
  if (free < reserve || needed > free - reserve) {
    errno = ENOSPC;
    return false;
  }
  return true;
}

inline ssize_t WriteStaging(int fd, const void *data, size_t size) {
  if (!StagingSpaceAvailable(fd, size))
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
