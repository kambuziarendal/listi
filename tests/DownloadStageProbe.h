// SPDX-License-Identifier: MIT
#pragma once
#include "FileOperation.h"
#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace DownloadStageProbe {
constexpr auto smallName = "early-conflict.bin", largeName = "large-conflict.bin";
constexpr size_t largeSize = 1024 * 1024;
static int stageFd = -1;
static bool measuring = false, full = false;
static size_t opens = 0, writes = 0, bytes = 0;

inline std::string Read(const std::string &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}

// Same three cases against a native ZIP reader and the fault-suite network
// provider. Only the second leaf's private stage is subject to ENOSPC.
template<class Execute>
void Exercise(const Listi::Job &job, Execute execute, const char *provider) {
  const auto small = job.destination + "/" + smallName;
  const auto large = job.destination + "/" + largeName;
  std::ofstream(small) << "old small";
  std::ofstream(large) << "old large";
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (attempt == 1 && unlink(large.c_str()))
      throw std::runtime_error("Cannot remove large destination fixture");
    full = attempt != 2;
    bytes = writes = opens = 0;
    int questions = 0;
    Listi::Hooks h;
    h.conflict = [&](const std::string &path) {
      if (path != small)
        throw std::runtime_error("Wrong path in first conflict");
      ++questions;
      return Listi::Decision::SkipAll;
    };
    measuring = true;
    const auto result = execute(job, h);
    measuring = false;
    if (questions != 1 || Read(small) != "old small" || result.cancelled)
      throw std::runtime_error("Skip All did not preserve the first conflict");
    if (attempt == 0) {
      const auto skipped = std::count_if(result.outcomes.begin(), result.outcomes.end(),
          [&](const auto &entry) { return entry.status == "skipped" && entry.source == job.sources[1] &&
                                         entry.destination == large; });
      if (opens || writes || bytes || result.skipped != 2 || result.completed ||
          !result.errors.empty() || skipped != 1 || Read(large) != "old large")
        throw std::runtime_error("Existing second target should skip without a stage or write");
    } else if (attempt == 1) {
      if (!opens || !writes || bytes || result.skipped != 1 || result.completed ||
          result.errors.size() != 1 || access(large.c_str(), F_OK) == 0)
        throw std::runtime_error("Vacant second target must still attempt staging and report ENOSPC");
    } else if (bytes != largeSize || result.skipped != 1 || result.completed != 1 ||
               !result.errors.empty() || Read(large) != std::string(largeSize, 'b')) {
      throw std::runtime_error("Vacant second target retry must publish complete payload");
    }
    printf("PASS %s Skip All: second target %s, stage %s, "
           "write attempts %zu, bytes staged %zu, skipped %d, errors %zu\n",
           provider, attempt ? "absent" : "exists", full ? "full" : "writable",
           writes, bytes, result.skipped, result.errors.size());
  }
  unlink(large.c_str());
  unlink(small.c_str());
}
}

extern "C" int __real_openat(int, const char *, int, ...);
extern "C" int __wrap_openat(int directory, const char *name, int flags, ...) {
  if (!(flags & O_CREAT))
    return __real_openat(directory, name, flags);
  va_list args;
  va_start(args, flags);
  const auto mode = va_arg(args, int);
  va_end(args);
  const int fd = __real_openat(directory, name, flags, mode);
  using namespace DownloadStageProbe;
  if (fd >= 0 && measuring && (flags & O_EXCL) && !strcmp(name, largeName)) {
    // The scratch tree is directly under /tmp; the fixture destination and
    // local publisher are not. Do not attribute publication I/O to staging.
    struct stat temporary, parent;
    if (stat("/tmp", &temporary) == 0 && fstatat(directory, "..", &parent, 0) == 0 &&
        temporary.st_dev == parent.st_dev && temporary.st_ino == parent.st_ino) {
      stageFd = fd;
      ++opens;
    }
  }
  return fd;
}
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" ssize_t __wrap_write(int fd, const void *data, size_t size) {
  using namespace DownloadStageProbe;
  if (fd != stageFd || !size)
    return __real_write(fd, data, size);
  ++writes;
  if (full) {
    errno = ENOSPC;
    return -1;
  }
  const auto written = __real_write(fd, data, size);
  if (written > 0)
    bytes += written;
  return written;
}
extern "C" int __real_close(int);
extern "C" int __wrap_close(int fd) {
  if (fd == DownloadStageProbe::stageFd)
    DownloadStageProbe::stageFd = -1;
  return __real_close(fd);
}
