// SPDX-License-Identifier: MIT
#include "DirectoryFinalization.h"
#include "RemovableCopy.h"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#ifdef __HAIKU__
#include <Directory.h>
#include <Entry.h>
#include <Path.h>
#include <Volume.h>
#include <fs_info.h>
#include <strings.h>
#endif
#include <unistd.h>

namespace Listi {
PinnedDirectory::PinnedDirectory(const std::string &path, dev_t device,
                                 ino_t inode, bool follow)
    : path(path), device(device), inode(inode), follow(follow) {
  fd = open(path.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | (follow ? 0 : O_NOFOLLOW));
  struct stat opened;
  if (fd < 0 || fstat(fd, &opened) || !S_ISDIR(opened.st_mode) ||
      opened.st_dev != device || opened.st_ino != inode) {
    if (fd >= 0)
      close(fd);
    throw std::runtime_error(path + ": cannot pin destination directory.");
  }
}

PinnedDirectory::~PinnedDirectory() { close(fd); }

void PinnedDirectory::Verify() const {
  struct stat now;
  if ((follow ? stat(path.c_str(), &now) : lstat(path.c_str(), &now)) ||
      !S_ISDIR(now.st_mode) || now.st_dev != device || now.st_ino != inode)
    throw std::runtime_error(path + ": destination directory changed.");
}

bool PinnedDirectory::Flush(const std::function<void()> &checkpoint,
                            bool allowUnsupportedNtfs) const {
  if (checkpoint)
    checkpoint();
  Verify();
  int error;
  int closeError = 0;
  bool ntfs = false;
#ifdef __HAIKU__
  fs_info info{};
  if (fs_stat_dev(device, &info))
    throw std::runtime_error(path +
                             ": cannot identify volume for directory flush.");
  ntfs = strcasecmp(info.fsh_name, "ntfs") == 0;
  const bool removable = strncmp(info.device_name, "/dev/disk/", 10) == 0 &&
                         ((info.flags & B_FS_IS_REMOVABLE) ||
                          strncmp(info.device_name, "/dev/disk/usb/", 14) == 0);
  if (removable) {
    std::string mount;
    {
      BDirectory root;
      BEntry entry;
      BPath location;
      if (BVolume(device).GetRootDirectory(&root) == B_OK &&
          root.GetEntry(&entry) == B_OK && entry.GetPath(&location) == B_OK)
        mount = location.Path();
    }
    auto interruption = [&](const std::string &message) {
      return RemovableCopyInterrupted(path + ": " + message, mount);
    };
    struct stat backing;
    auto check = [&] {
      struct stat current;
      if (stat(info.device_name, &current) ||
          backing.st_dev != current.st_dev || backing.st_ino != current.st_ino)
        throw interruption(
            "Device changed during directory flush. Durability after restart is unknown.");
      if (checkpoint)
        checkpoint();
    };
    if (stat(info.device_name, &backing))
      throw interruption(
          "Device disconnected before directory flush. Durability after restart is unknown.");
    const int worker = dup(fd);
    if (worker < 0)
      throw std::runtime_error(
          path + ": cannot duplicate directory flush descriptor.");
    try {
      error = FlushRemovableDirectory(worker, check, closeError);
    } catch (const RemovableCopyInterrupted &failure) {
      throw interruption(failure.what());
    }
    if (error == EIO || error == ENODEV || error == ENXIO ||
        closeError == EIO || closeError == ENODEV || closeError == ENXIO)
      throw interruption(
          "Removable directory flush failed. Durability after restart is unknown.");
  } else
#endif
    error = fsync(fd) ? errno : 0;
  // Haiku's NTFS fsync calls file_cache_sync on a null directory cache.
  // EINVAL here is not a durability acknowledgment. Do not suppress a
  // separate close error, or relax any caller that requires a real flush.
  const bool unsupported = allowUnsupportedNtfs && ntfs && !closeError &&
                           (error == EINVAL || error == ENOTSUP);
  if ((error || closeError) && !unsupported)
    throw std::runtime_error(
        path + ": directory flush failed: " + strerror(error ? error : closeError) +
        ". Durability after restart is unknown; source retained.");
  Verify();
  return !unsupported;
}

void PinnedDirectory::FlushAfterCleanup(
    const std::function<void()> &checkpoint) const {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  Flush([&] {
    if (checkpoint)
      checkpoint();
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("Staging removal flush timed out.");
  });
}

void FinalizeDirectory(const PinnedDirectory &directory,
                       const PinnedDirectory &parent,
                       const std::function<void()> &checkpoint) {
  parent.Verify();
  directory.Flush(checkpoint);
  parent.Flush(checkpoint);
  directory.Verify();
}
} // namespace Listi
