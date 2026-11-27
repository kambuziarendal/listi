// SPDX-License-Identifier: MIT
#pragma once
#include "FileOperation.h"
#include "VirtualFile.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace Listi {
// Installed-package qualification against tests/sftp-scaling-fixture.py.
// Only reads from the local fixture, and writes into a fresh temporary folder.
inline int SftpCopyTest(const std::string &fixture, const std::string &knownHosts,
                        const std::string &key) {
  namespace fs = std::filesystem;
  const bool wide = fixture == "wide";
  const int count = fixture == "64" ? 64 : 8;
  if (!wide && fixture != "8" && fixture != "64" && fixture != "fifo")
    return 2;
  const std::string server = "sftp://127.0.0.1:28657/";
  Connection auth;
  auth.username = "benchmark";
  auth.privateKey = key;
  auth.knownHosts = knownHosts;
  if (!SetConnection(server, auth))
    return 2;
  char scratch[] = "/boot/home/Listi-sftp-copy-XXXXXX";
  if (!mkdtemp(scratch))
    return 2;
  std::string problem;
  try {
    const auto folder = ChildLocation(server, fixture);
    const auto source = ChildLocation(folder, wide ? "item-0000" : "top");
    const auto start = std::chrono::steady_clock::now();
    const auto result = Run({Action::Copy, {source}, scratch}, {});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    std::cout << "seconds " << std::chrono::duration<double>(elapsed).count() << std::endl;
    if (result.completed != 1 || !result.errors.empty())
      throw std::runtime_error(result.errors.empty() ? "Incomplete transfer" : result.errors.front());
    const fs::path data = wide ? fs::path(scratch) : fs::path(scratch) / "top" / "inner";
    if (std::distance(fs::directory_iterator(data), fs::directory_iterator{}) != (wide ? 1 : count))
      throw std::runtime_error("Unexpected number of files in destination");
    for (int i = 0; i < (wide ? 1 : count); ++i) {
      const auto name = "item-" + std::to_string(10000 + i).substr(1);
      std::ifstream file(data / name, std::ios::binary);
      if (std::string(std::istreambuf_iterator<char>(file), {}) != name)
        throw std::runtime_error("Incorrect bytes in " + name);
    }
  } catch (const std::exception &error) {
    problem = error.what();
  }
  std::error_code ec;
  fs::remove_all(scratch, ec);
  if (!problem.empty() || ec) {
    std::cerr << (problem.empty() ? ec.message() : problem) << std::endl;
    return 1;
  }
  std::cout << "PASS verified downloaded names, bytes and job status" << std::endl;
  return 0;
}
}
