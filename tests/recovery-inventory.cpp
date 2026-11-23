// SPDX-License-Identifier: MIT
#include "RecoveryInventory.h"
#include "ArchiveEditor.h"
#include "RecoveryReviewWindow.h"
#include "ReadWorker.h"
#include <Application.h>
#include <Button.h>
#include <OS.h>
#include <TextView.h>
#include <Window.h>
#include <atomic>
#include <cerrno>
#include <string_view>
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

static std::string gatedPath;
static std::atomic<bool> gateArmed{false}, entered{false}, releaseProbe{false}, returned{false};
extern "C" int __real_lstat(const char *, struct stat *);
extern "C" int __wrap_lstat(const char *path, struct stat *st) {
  if (gateArmed && gatedPath == path) {
    entered = true;
    while (!releaseProbe)
      snooze(1000);
    const int result = __real_lstat(path, st);
    returned = true;
    return result;
  }
  return __real_lstat(path, st);
}

static std::atomic<bool> archiveGate{false};
extern "C" int __real_access(const char *, int);
extern "C" int __wrap_access(const char *path, int mode) {
  constexpr std::string_view suffix("/Listi_archive_edits");
  const std::string_view name(path);
  if (archiveGate && name.size() >= suffix.size() &&
      name.substr(name.size() - suffix.size()) == suffix) {
    entered = true;
    while (!releaseProbe)
      snooze(1000);
    errno = ENOENT;
    return -1;
  }
  return __real_access(path, mode);
}

static void CheckReviewWindows(const fs::path &inventory, const fs::path &stage) {
  BApplication app("application/x-vnd.Listi-recovery-review-test");
  gatedPath = stage;
  gateArmed = true;
  auto openJournal = [&]() {
    const auto deadline = system_time() + 2000000;
    auto *window = OpenRecoveryReview(BRect(0, 0, 640, 480), inventory);
    while (system_time() < deadline) {
      if (window->LockWithTimeout(20000) != B_OK)
        continue;
      auto *text = dynamic_cast<BTextView *>(window->FindView("recovery records"));
      if (text && std::string(text->Text()).find(gatedPath) != std::string::npos &&
          std::string(text->Text()).find("Identity not checked") != std::string::npos)
        return window; // caller holds the lock
      window->Unlock();
      snooze(10000);
    }
    throw std::runtime_error("Recovery journal not displayed within two seconds");
  };
  auto *first = openJournal();
  Require(!entered, "opening review displays journal without probing the staging volume");
  auto *check = dynamic_cast<BButton *>(first->FindView("check paths"));
  Require(check && check->IsEnabled() && check->Invoke() == B_OK,
          "user can request optional local identity checks");
  first->Unlock();
  auto deadline = system_time() + 2000000;
  while (!entered && system_time() < deadline)
    snooze(1000);
  Require(entered, "controlled stage lstat is pending");
  auto *second = openJournal();
  second->Unlock();
  deadline = system_time() + 2000000;
  Require(first->LockWithTimeout(1000000) == B_OK, "blocked review looper remains available");
  first->Quit();
  Require(system_time() < deadline && !releaseProbe, "blocked review closes before probe returns");
  releaseProbe = true;
  deadline = system_time() + 2000000;
  while (!returned && system_time() < deadline)
    snooze(1000);
  Require(returned && second->LockWithTimeout(1000000) == B_OK, "late probe returned after close");
  auto *text = dynamic_cast<BTextView *>(second->FindView("recovery records"));
  Require(text && std::string(text->Text()).find("Identity not checked") != std::string::npos,
          "second review retains its own journal-only snapshot");
  second->Quit();
  Require(DrainReadWorkers(2000000), "closed review readers exit after I/O returns");
  gateArmed = false;

  entered = releaseProbe = false;
  archiveGate = true;
  ResumeArchiveEditors("");
  deadline = system_time() + 2000000;
  while (!entered && system_time() < deadline)
    snooze(1000);
  Require(entered, "archive recovery listing is blocked in controlled storage read");
  BWindow *archive = nullptr;
  for (int32 i = 0; i < app.CountWindows(); ++i) {
    auto *candidate = app.WindowAt(i);
    if (candidate && candidate->Title() && std::string(candidate->Title()) == "Listi: Resume archive edits")
      archive = candidate;
  }
  deadline = system_time() + 2000000;
  Require(archive && archive->LockWithTimeout(1000000) == B_OK && archive->QuitRequested(),
          "archive recovery accepts close while its listing is blocked");
  archive->Quit();
  Require(system_time() < deadline && !releaseProbe, "archive recovery closed before read returned");
  releaseProbe = true;
  Require(DrainReadWorkers(2000000), "late archive listing retires after its window closes");
  archiveGate = false;
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
  auto report = ReviewRecovery(inventory, RecoveryReview::CheckLocalPaths);
  Require(report.find(stage) != std::string::npos &&
              report.find("identity matches") != std::string::npos &&
              fs::is_regular_file(stage / "data"),
          "fresh process reviews retained stage without modifying it");
  CheckReviewWindows(inventory, stage);
  const auto moved = root / "moved";
  fs::rename(stage, moved);
  fs::create_directory(stage);
  std::ofstream(stage / "unrelated") << "unrelated replacement";
  report = ReviewRecovery(inventory, RecoveryReview::CheckLocalPaths);
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
  Require(ReviewRecovery(inventory, RecoveryReview::CheckLocalPaths).find("Bulk rename journal") == std::string::npos &&
              fs::is_regular_file(journal),
          "successful operation retires only its private record");
  fs::remove_all(root);
  return 0;
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
