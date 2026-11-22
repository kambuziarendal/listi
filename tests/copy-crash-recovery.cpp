// SPDX-License-Identifier: MIT
// Kill the operation process at actual transaction syscalls. Its parent never
// resumes the operation. Read only the record for this unique fixture source.
#include "FileOperation.h"
#include "MessageStore.h"
#include "RecoveryInventory.h"
#include <FindDirectory.h>
#include <Path.h>
#include <cerrno>
#include <cstdarg>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace Listi;
namespace {
enum Boundary { Intent, Created, Writing, Data, Ready, Publishing, Renamed,
                Published, Retired, JournalFull, PublishFull, PrepareFull, RetireFull, Invalid = -1 };
int at = Invalid, records = 0, targetParent = -1;
const char *names[] = {"intent", "created", "writing", "data", "ready", "publishing",
                       "renamed", "published", "retired", "journal-full", "publish-full", "prepare-full", "retire-full"};
void Stop(int point) { if (point == at) _exit(41); }
void Require(bool value, const char *text) {
  if (!value) throw std::runtime_error(text);
}
std::string Bytes(const fs::path &file) {
  std::ifstream input(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}
}

extern "C" int __real_openat(int, const char *, int, ...);
extern "C" int __wrap_openat(int parent, const char *leaf, int flags, ...) {
  if (!(flags & O_CREAT))
    return __real_openat(parent, leaf, flags);
  va_list args;
  va_start(args, flags);
  const auto mode = va_arg(args, int);
  va_end(args);
  const int fd = __real_openat(parent, leaf, flags, mode);
  if (fd >= 0 && std::string(leaf).rfind(".listi-copy-", 0) == 0)
    Stop(Created);
  return fd;
}
extern "C" int __real_renameat(int, const char *, int, const char *);
extern "C" int __wrap_renameat(int from, const char *oldLeaf, int to, const char *leaf) {
  const bool journal = std::string(leaf).rfind("record-", 0) == 0;
  if (journal && records == 2) Stop(Ready); // Verified stage, still Writing on disk.
  if ((journal && (at == JournalFull || (at == PrepareFull && records == 2))) ||
      (at == PublishFull && std::string(leaf) == "payload")) {
    errno = ENOSPC;
    return -1;
  }
  int status = __real_renameat(from, oldLeaf, to, leaf);
  if (status == 0 && journal && at != Invalid) {
    constexpr int phase[] = {Intent, Writing, Publishing};
    if (records < 3) Stop(phase[records++]);
  } else if (status == 0 && std::string(leaf) == "payload") {
    targetParent = to;
    Stop(Renamed);
  }
  return status;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  const int status = __real_fsync(fd);
  if (!status && fd == targetParent) Stop(Published);
  return status;
}
extern "C" int __real_unlinkat(int, const char *, int);
extern "C" int __wrap_unlinkat(int fd, const char *leaf, int flags) {
  if (at == RetireFull && targetParent >= 0 && std::string(leaf).rfind("record-", 0) == 0) {
    errno = ENOSPC;
    return -1;
  }
  const int status = __real_unlinkat(fd, leaf, flags);
  if (!status && std::string(leaf).rfind("record-", 0) == 0)
    Stop(Retired);
  return status;
}

int main() try {
  char temp[] = "/tmp/Listi-crash-XXXXXX";
  Require(mkdtemp(temp), "Cannot create fixture");
  const auto root = fs::canonical(temp);
  const auto dest = root / "out", source = root / "payload", target = dest / "payload";
  const auto inventory = root / "review";
  fs::create_directory(dest);
  Require(!mkdir(inventory.c_str(), 0700), "Cannot create review folder");
  std::string payload(300000, 'x');
  std::ofstream(source) << payload;
  BPath settings;
  Require(find_directory(B_USER_SETTINGS_DIRECTORY, &settings) == B_OK &&
          settings.Append("Listi-recovery") == B_OK, "No recovery settings");
  for (int boundary = Intent; boundary <= RetireFull; ++boundary) {
    std::ofstream(target) << "original";
    const auto child = fork();
    if (!child) {
      at = boundary;
      Hooks hooks;
      hooks.conflict = [](const std::string &) { return Decision::Replace; };
      hooks.progress = [](const std::string &, uint64_t bytes) { if (bytes) Stop(Data); };
      auto result = Run({Action::Copy, {source}, dest}, hooks);
      _exit((at >= JournalFull) &&
            !result.completed && !result.errors.empty() ? 41 : 2);
    }
    int status;
    Require(child > 0 && waitpid(child, &status, 0) == child &&
            WIFEXITED(status) && WEXITSTATUS(status) == 41, names[boundary]);
    const bool published = (boundary >= Renamed && boundary <= Retired) || boundary == RetireFull;
    Require(Bytes(source) == payload && Bytes(target) == (published ? payload : "original"),
            "Source or destination bytes wrong after interruption");
    unsigned found = 0;
    for (const auto &entry : fs::directory_iterator(settings.Path())) {
      if (entry.path().filename().string().size() != 13 ||
          !fs::is_regular_file(entry.symlink_status())) continue;
      BMessage record;
      try { record = LoadMessageStore(entry.path()); } catch (...) { continue; }
      if (record.GetString("source", "") != source.string()) continue;
      ++found;
      auto own = inventory / entry.path().filename();
      fs::copy_file(entry.path(), own);
      const auto report = ReviewRecovery(inventory);
      Require(report == ReviewRecovery(inventory) && report.find(source) != std::string::npos &&
              report.find(target) != std::string::npos, "Restart review is not idempotent");
      if (boundary == Renamed || boundary == Published || boundary == RetireFull || boundary == Publishing)
        Require(report.find("Uncertain publication") != std::string::npos, "Lost rename mislabelled");
      if (boundary == Ready)
        Require(report.find("not published") != std::string::npos, "Ready stage falsely marked published");
      const auto stage = fs::path(record.GetString("path", ""));
      Require(stage.parent_path() == dest, "Unexpected fixture stage");
      if (fs::exists(stage)) {
        fs::rename(stage, root / "moved");
        fs::create_symlink(source, stage);
        Require(ReviewRecovery(inventory).find("identity changed") != std::string::npos &&
                fs::is_symlink(stage) && Bytes(stage) == payload, "Substituted entry not retained");
        fs::remove(stage);
        fs::remove(root / "moved");
      }
      fs::remove(own);
      fs::remove(entry.path());
    }
    Require(found == (boundary < Retired || boundary == RetireFull ? 1u : 0u), "Wrong number of outstanding records");
    std::cout << "PASS restart boundary " << names[boundary] << std::endl;
  }
  fs::remove_all(root);
  return 0;
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
