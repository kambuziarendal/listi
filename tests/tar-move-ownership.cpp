// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "VirtualFile.h"
#include <Application.h>
#include <cstdio>
#include <fcntl.h>
#include <cstring>
#include <sys/stat.h>
#include <iostream>

namespace {
const char *changedArchive = nullptr;
bool substituted = false;
}
extern "C" int __real_renameat(int, const char *, int, const char *);
extern "C" int __wrap_renameat(int from, const char *oldName, int to, const char *newName) {
  const int status = __real_renameat(from, oldName, to, newName);
  if (!status && changedArchive && !substituted && !strcmp(oldName, "updated") &&
      !strcmp(newName, "target.tar"))
    substituted = __real_renameat(AT_FDCWD, changedArchive, to, newName) == 0;
  return status;
}

int main(int argc, char **argv) {
  using namespace Listi;
  BApplication app("application/x-vnd.Listi-tar-move-ownership");
  if (argc != 3 && argc != 4)
    return 2;
  const std::string input = argv[1];
  const bool local = Leaf(input) == "local-item" || Leaf(input) == "tree";
  struct stat destination;
  if (stat(argv[2], &destination))
    return 2;
  const bool expectRefusal = argc == 4 && !strcmp(argv[3], "unsupported");
  const bool directoryConflict = argc == 4 && !strcmp(argv[3], "directory-conflict");
  const bool timeMismatch = argc == 4 && !strcmp(argv[3], "time-mismatch");
  const auto target = S_ISDIR(destination.st_mode)
                          ? std::string(argv[2]) : ChildLocation(OpenArchive(argv[2]), "recv");
  std::vector<std::string> sources{input};
  if (!local) {
    const auto source = OpenArchive(input);
    sources = {ChildLocation(ChildLocation(source, "box"), "tree"), ChildLocation(source, "loose")};
  }
  if (argc == 4 && !expectRefusal && !directoryConflict && !timeMismatch)
    changedArchive = argv[3];
  auto result = Run(PrepareArchiveMove(Job{Action::Move, sources, target}), {});
  bool mismatch = false;
  for (const auto &error : result.errors) {
    std::cerr << error << std::endl;
    if (timeMismatch && error.find("Destination timestamp or ownership differs") != std::string::npos)
      return result.completed == 0 && result.completedRoots.empty() ? 0 : 1;
    if (directoryConflict && error.find("conflicting TAR metadata") != std::string::npos)
      return result.completed == 0 && result.completedRoots.empty() ? 0 : 1;
    if (expectRefusal && error.find("Cannot preserve selected permissions or ownership") != std::string::npos)
      return result.completed == 0 && result.completedRoots.empty() ? 0 : 1;
    mismatch |= error.find("ownership differs") != std::string::npos;
  }
  if (expectRefusal || directoryConflict || timeMismatch) return 1;
  if (changedArchive)
    return substituted && mismatch && result.completed == 0 && result.completedRoots.empty() ? 0 : 1;
  return result.completed == int(sources.size()) && result.errors.empty() ? 0 : 1;
}
