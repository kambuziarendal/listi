// SPDX-License-Identifier: MIT
// Linked only by the opt-in benchmark. No instrumentation in shipped binaries.
#include "FileOperation.h"
#include <FindDirectory.h>
#include <Path.h>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
struct Metric {
  long calls = 0;
  long long us = 0;
  void Add(Clock::time_point start) {
    ++calls;
    us += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
  }
};
static Metric journalFile, directory, dataFile, renameCalls;
static long long journalBytes = 0;
static thread_local int journalFd = -1;
static thread_local bool measure = false;

extern "C" int __real_mkstemp(char *);
extern "C" int __wrap_mkstemp(char *name) {
  int fd = __real_mkstemp(name);
  if (measure && fd >= 0 && std::string(name).find("/Listi-recovery/record-") != std::string::npos)
    journalFd = fd;
  return fd;
}
extern "C" int __real_close(int);
extern "C" int __wrap_close(int fd) {
  if (fd == journalFd) journalFd = -1;
  return __real_close(fd);
}
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" ssize_t __wrap_write(int fd, const void *data, size_t bytes) {
  ssize_t n = __real_write(fd, data, bytes);
  if (measure && fd == journalFd && n > 0) journalBytes += n;
  return n;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  if (!measure) return __real_fsync(fd);
  struct stat st;
  bool dir = fstat(fd, &st) == 0 && S_ISDIR(st.st_mode);
  auto start = Clock::now();
  int result = __real_fsync(fd);
  (dir ? directory : fd == journalFd ? journalFile : dataFile).Add(start);
  return result;
}
extern "C" int __real_renameat(int, const char *, int, const char *);
extern "C" int __wrap_renameat(int a, const char *b, int c, const char *d) {
  auto start = Clock::now();
  int result = __real_renameat(a, b, c, d);
  if (measure) renameCalls.Add(start);
  return result;
}

int main(int argc, char **argv) try {
  if (argc != 4) throw std::runtime_error("usage: copy volume-dir dataset mode");
  const std::string dataset = argv[2], mode = argv[3];
  const int count = dataset == "one" || dataset == "medium" ? 1 :
                    dataset == "tiny" ? 256 : 32;
  const size_t bytes = dataset == "medium" ? 8 * 1024 * 1024 : dataset == "tiny" ? 64 : 4096;
  std::string pattern = fs::canonical(argv[1]).string() + "/listi-timing-XXXXXX";
  if (!mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
  const fs::path root(pattern), in = root / "in", out = root / "out";
  fs::create_directories(in);
  fs::create_directories(out);
  std::string payload(bytes, 'x');
  std::vector<fs::path> names;
  for (int i = 0; i < count; ++i) {
    fs::path name = std::to_string(i);
    if (dataset == "nested") name = fs::path(std::to_string(i % 4)) / "deep" / name;
    names.push_back(name);
    fs::create_directories((in / name).parent_path());
    if (!(std::ofstream(in / name) << payload)) throw std::runtime_error("source write failed");
    if (mode == "overwrite") {
      fs::create_directories((out / "in" / name).parent_path());
      if (!(std::ofstream(out / "in" / name) << "original")) throw std::runtime_error("target setup failed");
    }
  }
  bool stop = false;
  Listi::Hooks hooks;
  hooks.conflict = [](const std::string &) { return Listi::Decision::Replace; };
  hooks.proceed = [&] { return !stop; };
  hooks.progress = [&](const std::string &, uint64_t n) { if (mode == "cancel" && n) stop = true; };
  auto start = Clock::now();
  auto cpu = std::clock();
  measure = true;
  auto result = Listi::Run({Listi::Action::Copy, {in.string()}, out.string()}, hooks);
  measure = false;
  auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
  double cpuUs = 1e6 * (std::clock() - cpu) / CLOCKS_PER_SEC;
  if (mode == "cancel" ? !result.cancelled || result.completed != 0 :
      !result.errors.empty() || result.completed != 1) throw std::runtime_error("unexpected copy result");
  for (const auto &name : names) {
    std::ifstream input(mode == "cancel" ? in / name : out / "in" / name, std::ios::binary);
    if (std::string(std::istreambuf_iterator<char>(input), {}) != payload)
      throw std::runtime_error("unexpected bytes in fixture");
  }
  std::cout << "{\"wall_us\":" << elapsed << ",\"cpu_us\":" << cpuUs
            << ",\"journal_bytes\":" << journalBytes << ",\"renames_at\":" << renameCalls.calls
            << ",\"rename_us\":" << renameCalls.us << ",\"journal_fsync\":" << journalFile.calls
            << ",\"journal_fsync_us\":" << journalFile.us << ",\"dir_fsync\":" << directory.calls
            << ",\"dir_fsync_us\":" << directory.us << ",\"data_fsync\":" << dataFile.calls
            << ",\"data_fsync_us\":" << dataFile.us << "}\n";
  fs::remove_all(root);
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
