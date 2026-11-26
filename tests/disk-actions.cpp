// SPDX-License-Identifier: MIT
#include "DiskActions.h"
#include "PaneModel.h"
#include <Application.h>
#include <Directory.h>
#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <Entry.h>
#include <Message.h>
#include <Path.h>
#include <Volume.h>
#include <VolumeRoster.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace Listi;

namespace {

DiskListing FindFixture(const char *devicePath) {
  DiskListing result;
  VisitDiskPartitions([&](const auto &disk) {
    if (disk.partition.devicePath != devicePath)
      return false;
    result = disk;
    return true;
  });
  assert(result.partition.id >= 0);
  BDiskDevice device;
  BPartition *partition = nullptr;
  assert(BDiskDeviceRoster().GetPartitionWithID(result.partition.id, &device,
                                              &partition) == B_OK);
  assert(device.IsFile() && partition == &device &&
         partition->ContentName() == "ListiMountQA");
  return result;
}

// Tracker may briefly open the volume in response to its mount notification.
// Only the fixture retries; the application reports B_BUSY to the user.
status_t UnmountWhenIdle(const DiskListing &disk, const BPath &path,
                         const struct stat &info) {
  status_t result;
  for (int attempt = 0; attempt < 50; ++attempt) {
    result = UnmountDisk(disk.partition, path.Path(), info.st_dev, info.st_ino);
    if (result != B_BUSY)
      return result;
    usleep(100000);
  }
  return B_BUSY;
}

void VerifyRoundTrip(const char *devicePath) {
  const auto first = FindFixture(devicePath);
  assert(first.mountedDevice < 0);
  auto stale = first.partition;
  ++stale.offset;
  assert(MountDisk(stale) == B_ENTRY_NOT_FOUND);
  assert(MountDisk(first.partition) == B_OK);

  const auto mounted = FindFixture(devicePath);
  assert(mounted.mountedDevice >= 0 && !mounted.protectedVolume);
  BDirectory root;
  BEntry entry;
  BPath path;
  assert(BVolume(mounted.mountedDevice).GetRootDirectory(&root) == B_OK);
  assert(root.GetEntry(&entry) == B_OK && entry.GetPath(&path) == B_OK);
  entry.Unset();
  root.Unset();
  struct stat info;
  assert(lstat(path.Path(), &info) == 0);
  assert(UnmountDisk(stale, path.Path(), info.st_dev, info.st_ino) ==
         B_ENTRY_NOT_FOUND);
  assert(UnmountDisk(mounted.partition, path.Path(), info.st_dev, info.st_ino + 1) ==
         B_ENTRY_NOT_FOUND);

  const std::string file = std::string(path.Path()) + "/disk-actions-test";
  int fd = open(file.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0 && write(fd, "test", 4) == 4);
  assert(UnmountDisk(mounted.partition, path.Path(), info.st_dev, info.st_ino) == B_BUSY);
  assert(FindFixture(devicePath).mountedDevice == info.st_dev);
  assert(close(fd) == 0);
  assert(UnmountWhenIdle(mounted, path, info) == B_OK);
  assert(FindFixture(devicePath).mountedDevice < 0);

  const auto second = FindFixture(devicePath);
  assert(MountDisk(second.partition) == B_OK);
  const auto remounted = FindFixture(devicePath);
  assert(remounted.mountedDevice >= 0);
  assert(BVolume(remounted.mountedDevice).GetRootDirectory(&root) == B_OK);
  assert(root.GetEntry(&entry) == B_OK && entry.GetPath(&path) == B_OK);
  entry.Unset();
  root.Unset();
  const std::string remountedFile = std::string(path.Path()) + "/disk-actions-test";
  fd = open(remountedFile.c_str(), O_RDONLY);
  char contents[5] = {};
  assert(fd >= 0 && read(fd, contents, 4) == 4 && std::string(contents) == "test");
  assert(close(fd) == 0 && unlink(remountedFile.c_str()) == 0);
  assert(lstat(path.Path(), &info) == 0);
  assert(UnmountWhenIdle(remounted, path, info) == B_OK);
  assert(FindFixture(devicePath).mountedDevice < 0);
  puts("File-backed BFS: stale, busy, unmount, remount and data verified");
}

} // namespace

int main() {
  BApplication app("application/x-vnd.Listi-DiskActionsTest");
  DiskPartition partition{42, "/dev/disk/test", "bfs", 100, 1024};
  BMessage message;
  partition.WriteTo(message);
  assert(DiskPartition::ReadFrom(message) == partition);
  assert(MountDisk({}) == B_ENTRY_NOT_FOUND);

  BVolume boot;
  assert(BVolumeRoster().GetBootVolume(&boot) == B_OK);
  assert(UnmountDisk({}, "/boot", boot.Device(), 1) == B_NOT_ALLOWED);
  assert(UnmountDisk({}, "/", boot.Device(), 1) == B_NOT_ALLOWED);

  PaneModel rows;
  auto disk = std::make_unique<PaneEntry>();
  disk->path = "@partition/42";
  disk->unmountedDisk = true;
  rows.Add(std::move(disk));
  auto mounted = std::make_unique<PaneEntry>();
  mounted->path = "/Mounted";
  mounted->directory = true;
  rows.Add(std::move(mounted));
  rows.Filter("");
  rows.SetSelection({"@partition/42"}, "@partition/42");
  assert(rows.OnlySelected()->unmountedDisk && rows.SelectedFiles().empty());
  rows.SetSelection({"@partition/42", "/Mounted"}, "/Mounted");
  assert(rows.SelectedFiles().empty());
  rows.SetSelection({"/Mounted"}, "/Mounted");
  assert(rows.SelectedFiles() == std::vector<std::string>{"/Mounted"});

  if (const char *devicePath = getenv("LISTI_TEST_DISK_DEVICE"))
    VerifyRoundTrip(devicePath);
  else
    puts("SKIP disk round-trip (set LISTI_TEST_DISK_DEVICE to an unmounted disposable file device named ListiMountQA)");
}
