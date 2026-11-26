// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "VirtualFile.h"
#include <Application.h>
#include <cstdio>
#include <fcntl.h>
#include <cstring>
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
  const auto source = OpenArchive(argv[1]);
  const auto target = ChildLocation(OpenArchive(argv[2]), "recv");
  auto tree = ChildLocation(ChildLocation(source, "box"), "tree");
  auto loose = ChildLocation(source, "loose");
  if (argc == 4)
    changedArchive = argv[3];
  auto result = Run(PrepareArchiveMove(Job{Action::Move, {tree, loose}, target}), {});
  bool mismatch = false;
  for (const auto &error : result.errors) {
    std::cerr << error << std::endl;
    mismatch |= error.find("ownership differs") != std::string::npos;
  }
  if (changedArchive)
    return substituted && mismatch && result.completed == 0 && result.completedRoots.empty() ? 0 : 1;
  return result.completed == 2 && result.errors.empty() ? 0 : 1;
}
