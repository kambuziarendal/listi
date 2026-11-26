// SPDX-License-Identifier: MIT
#pragma once

#include <SupportDefs.h>
#include <functional>
#include <string>

class BMessage;
class BMessenger;

namespace Listi {

constexpr uint32 kMountDisk = 'dsmn', kUnmountDisk = 'dsun';
constexpr uint32 kDiskDeviceUpdate = 'DUPD', kDiskActionDone = 'dsdn';

// A snapshot of a Haiku disk-manager partition, not a filesystem path.
struct DiskPartition {
  int32 id = -1;
  std::string devicePath, type;
  int64 offset = -1, size = -1;
  // The raw device ID can survive a removable media change.
  std::string contentName;

  bool operator==(const DiskPartition &other) const;
  void WriteTo(BMessage &message) const;
  static DiskPartition ReadFrom(const BMessage &message);
};

struct DiskListing {
  DiskPartition partition;
  std::string name;
  int32 mountedDevice = -1;
  bool protectedVolume = false;
};

void VisitDiskPartitions(const std::function<bool(const DiskListing &)> &visit);
status_t WatchDiskPartitions(const BMessenger &target);
void StopWatchingDiskPartitions(const BMessenger &target);

// Resolve and verify the current target before changing it. Unmount never
// forces a busy volume or ejects its backing device.
status_t MountDisk(const DiskPartition &expected);
status_t UnmountDisk(const DiskPartition &expected, const std::string &path,
                     int64 device, int64 node);
std::string DiskActionError(bool mount, status_t status);

} // namespace Listi
