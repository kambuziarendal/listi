// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "VirtualFile.h"
#include <Application.h>
#include <cerrno>
#include <fcntl.h>
#include <fs_attr.h>
#include <iostream>
#include <unistd.h>

static bool denyOwner = false;
extern "C" int __real_fchown(int, uid_t, gid_t);
extern "C" int __wrap_fchown(int fd, uid_t owner, gid_t group) {
  if (denyOwner) {
    errno = EPERM;
    return -1;
  }
  return __real_fchown(fd, owner, group);
}

int main(int argc, char **argv) {
  BApplication app("application/x-vnd.Listi-tar-edit-ownership");
  if (argc != 2 && argc != 3)
    return 2;
  using namespace Listi;
  if (argc == 3 && std::string(argv[2]) == "read-only") {
    try {
      PrepareArchiveEditTarget(argv[1]);
    } catch (const std::exception &error) {
      return std::string(error.what()).find("read-only") != std::string::npos ? 0 : 1;
    }
    return 1;
  }
  if (argc == 3) {
    const std::string operation(argv[2]);
    denyOwner = operation == "deny-owner";
    const auto oldPath = ChildLocation(OpenArchive(argv[1]), "old-name");
    const auto newPath = ChildLocation(OpenArchive(argv[1]), "new-name");
    auto result = Run(PrepareArchiveMutation(Job{Action::Rename, {oldPath}, newPath}), {});
    if (operation == "rename")
      return result.errors.empty() && result.completed == 1 ? 0 : 1;
    const auto reason = denyOwner ? "preserve archive owner" : "extended member metadata";
    for (const auto &error : result.errors)
      if (error.find(reason) != std::string::npos)
        return result.completed == 0 ? 0 : 1;
    return 1;
  }
  const char *name = "Listi:empty-outer-attribute";
  const auto isEmpty = [&](int fd) {
    attr_info info{};
    return fs_stat_attr(fd, name, &info) == 0 && info.type == B_RAW_TYPE && info.size == 0;
  };
  const int original = open(argv[1], O_RDWR);
  static const char seed[16384] = {};
  if (original < 0 || fs_write_attr(original, name, B_RAW_TYPE, 0, seed, sizeof(seed)) != sizeof(seed))
    return 2;
  int attr = fs_fopen_attr(original, name, B_RAW_TYPE, O_WRONLY | O_TRUNC);
  if (attr < 0 || fs_close_attr(attr) || !isEmpty(original) || close(original))
    return 2;
  const auto member = ChildLocation(OpenArchive(argv[1]), "remove");
  auto edit = PrepareArchiveEditJob(argv[1], {}, {member});
  edit.archiveRenames.emplace("old-name", "new-name");
  auto result = Run(edit, {});
  for (const auto &error : result.errors)
    std::cerr << error << std::endl;
  const int published = open(argv[1], O_RDONLY);
  const bool preserved = published >= 0 && isEmpty(published);
  if (published >= 0)
    close(published);
  return result.completed == 1 && result.errors.empty() && preserved ? 0 : 1;
}
