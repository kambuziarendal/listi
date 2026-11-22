// SPDX-License-Identifier: MIT
#include "RecoveryInventory.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace Listi;

namespace {
void Require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
  std::cout << "PASS " << message << '\n';
}
}

int main() try {
  char temp[] = "/tmp/Listi-inventory-XXXXXX";
  if (!mkdtemp(temp))
    return 2;
  const fs::path root = fs::canonical(temp);
  const auto stage = root / ".Listi-archive-stage-fixture";
  const auto inventory = root / "private-inventory";
  const auto child = fork();
  if (!child) {
    struct stat st;
    if (mkdir(stage.c_str(), 0700) || lstat(stage.c_str(), &st))
      _exit(22);
    RecoveryRecord record("archive", stage, st, inventory);
    std::ofstream(stage / "data") << "owned staging";
    _exit(21);
  }
  int status;
  Require(child > 0 && waitpid(child, &status, 0) == child &&
              WIFEXITED(status) && WEXITSTATUS(status) == 21,
          "stage owner died after writing a durable inventory record");
  auto report = ReviewRecovery(inventory);
  Require(report.find(stage) != std::string::npos &&
              report.find("identity matches") != std::string::npos &&
              fs::is_regular_file(stage / "data"),
          "fresh process reviews retained stage without modifying it");
  const auto moved = root / "moved";
  fs::rename(stage, moved);
  fs::create_directory(stage);
  std::ofstream(stage / "unrelated") << "unrelated replacement";
  report = ReviewRecovery(inventory);
  Require(report.find("identity changed") != std::string::npos &&
              fs::is_regular_file(stage / "unrelated") &&
              fs::is_regular_file(moved / "data"),
          "same-named replacement is not identified or removed");
  const auto journal = root / ".listi-recovery-rename-fixture";
  std::ofstream(journal) << "manual review";
  struct stat st;
  if (lstat(journal.c_str(), &st))
    return 2;
  RecoveryRecord completed("rename", journal, st, inventory);
  completed.Retire();
  Require(ReviewRecovery(inventory).find("Bulk rename journal") == std::string::npos &&
              fs::is_regular_file(journal),
          "successful operation retires only its private record");
  fs::remove_all(root);
  return 0;
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
