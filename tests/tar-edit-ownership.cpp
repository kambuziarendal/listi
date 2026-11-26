// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "VirtualFile.h"
#include <Application.h>
#include <iostream>

int main(int argc, char **argv) {
  BApplication app("application/x-vnd.Listi-tar-edit-ownership");
  if (argc != 2)
    return 2;
  using namespace Listi;
  const auto member = ChildLocation(OpenArchive(argv[1]), "remove");
  auto result = Run(PrepareArchiveEditJob(argv[1], {}, {member}), {});
  for (const auto &error : result.errors)
    std::cerr << error << std::endl;
  return result.completed == 1 && result.errors.empty() ? 0 : 1;
}
