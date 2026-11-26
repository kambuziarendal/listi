// SPDX-License-Identifier: MIT
#include "DiskActions.h"
#include "UnavailableVolume.h"
#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <DiskDeviceVisitor.h>
#include <Directory.h>
#include <Message.h>
#include <Path.h>
#include <Volume.h>
#include <VolumeRoster.h>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#include <tuple>

namespace Listi {
namespace {

std::mutex diskAction;

DiskPartition Identify(BPartition &partition) {
  BPath path;
  if (!partition.ContainsFileSystem() || partition.GetPath(&path) != B_OK ||
      !partition.ContentType())
    return {};
  const char *name = partition.RawContentName();
  return {partition.ID(), path.Path(), partition.ContentType(),
          partition.Offset(), partition.Size(), name ? name : ""};
}

// Fail closed if the boot volume cannot be identified. The synthetic root
// and package filesystem mounts are not ordinary unmount targets either.
bool ProtectedVolume(dev_t device, const char *path) {
  BVolume boot;
  return BVolumeRoster().GetBootVolume(&boot) != B_OK ||
         boot.Device() == device || !strcmp(path, "/") ||
         !strcmp(path, "/boot") || !strcmp(path, "/system") ||
         !strcmp(path, "/boot/system") ||
         !strncmp(path, "/boot/system/", sizeof("/boot/system/") - 1);
}

} // namespace

bool DiskPartition::operator==(const DiskPartition &other) const {
  return std::tie(id, devicePath, type, offset, size, contentName) ==
         std::tie(other.id, other.devicePath, other.type, other.offset, other.size,
                  other.contentName);
}

void DiskPartition::WriteTo(BMessage &message) const {
  message.AddInt32("partition", id);
  message.AddString("partitionPath", devicePath.c_str());
  message.AddString("partitionType", type.c_str());
  message.AddInt64("partitionOffset", offset);
  message.AddInt64("partitionSize", size);
  message.AddString("partitionContentName", contentName.c_str());
}

DiskPartition DiskPartition::ReadFrom(const BMessage &message) {
  return {message.GetInt32("partition", -1),
          message.GetString("partitionPath", ""),
          message.GetString("partitionType", ""),
          message.GetInt64("partitionOffset", -1),
          message.GetInt64("partitionSize", -1),
          message.GetString("partitionContentName", "")};
}

void VisitDiskPartitions(const std::function<bool(const DiskListing &)> &visit) {
  class Visitor : public BDiskDeviceVisitor {
  public:
    explicit Visitor(const std::function<bool(const DiskListing &)> &callback)
        : callback(callback) {}

    bool Visit(BDiskDevice *device) override { return Visit(device, 0); }
    bool Visit(BPartition *partition, int32) override {
      DiskListing listing;
      listing.partition = Identify(*partition);
      if (listing.partition.id < 0)
        return false;
      if (partition->IsMounted()) {
        BVolume volume;
        BPath path;
        if (partition->GetVolume(&volume) != B_OK ||
            InterruptedVolumes().ContainsDevice(volume.Device()) ||
            partition->GetMountPoint(&path) != B_OK)
          return false;
        listing.mountedDevice = volume.Device();
        listing.protectedVolume = ProtectedVolume(volume.Device(), path.Path());
      } else {
        BString name = partition->ContentName();
        if (name.IsEmpty())
          name = partition->Name();
        if (name.IsEmpty())
          name = listing.partition.devicePath.c_str();
        listing.name = name.String();
      }
      return callback(listing);
    }

  private:
    const std::function<bool(const DiskListing &)> &callback;
  } visitor(visit);
  BDiskDeviceRoster().VisitEachMountablePartition(&visitor);
}

status_t WatchDiskPartitions(const BMessenger &target) {
  // VFS broadcasts unmount before clearing the partition's mounted state.
  // The disk manager publishes a second event once its inventory is current.
  return BDiskDeviceRoster().StartWatching(
      target, B_DEVICE_REQUEST_MOUNTING | B_DEVICE_REQUEST_PARTITION |
                  B_DEVICE_REQUEST_DEVICE | B_DEVICE_REQUEST_DEVICE_LIST);
}

void StopWatchingDiskPartitions(const BMessenger &target) {
  BDiskDeviceRoster().StopWatching(target);
}

status_t MountDisk(const DiskPartition &expected) {
  std::unique_lock<std::mutex> lock(diskAction, std::try_to_lock);
  if (!lock.owns_lock())
    return B_BUSY;
  BDiskDevice disk;
  BPartition *partition = nullptr;
  if (expected.id < 0 ||
      BDiskDeviceRoster().GetPartitionWithID(expected.id, &disk, &partition) != B_OK ||
      !partition || !(Identify(*partition) == expected) || partition->IsMounted())
    return B_ENTRY_NOT_FOUND;
  if (partition->IsBusy())
    return B_BUSY;
  return partition->Mount();
}

status_t UnmountDisk(const DiskPartition &expected, const std::string &path,
                     int64 device, int64 node) {
  std::unique_lock<std::mutex> lock(diskAction, std::try_to_lock);
  if (!lock.owns_lock())
    return B_BUSY;
  if (device < 0 || node < 0 || path.empty())
    return B_ENTRY_NOT_FOUND;
  if (ProtectedVolume(device, path.c_str()))
    return B_NOT_ALLOWED;
  if (InterruptedVolumes().ContainsDevice(device) ||
      InterruptedVolumes().Contains(path))
    return B_DEV_NOT_READY;
  BDiskDevice disk;
  BPartition *partition = nullptr;
  BVolume volume;
  BPath actual;
  struct stat named;
  if (expected.id < 0 ||
      BDiskDeviceRoster().GetPartitionWithID(expected.id, &disk, &partition) != B_OK ||
      !partition || !(Identify(*partition) == expected) ||
      partition->GetVolume(&volume) != B_OK || volume.Device() != device ||
      partition->GetMountPoint(&actual) != B_OK || path != actual.Path() ||
      lstat(path.c_str(), &named) || named.st_dev != device || named.st_ino != node)
    return B_ENTRY_NOT_FOUND;
  if (partition->IsBusy())
    return B_BUSY;
  return partition->Unmount(0);
}

std::string DiskActionError(bool mount, status_t status) {
  std::string text = mount ? "Cannot mount: " : "Cannot unmount: ";
  if (status == B_BUSY)
    return text + "the disk is busy. Close files and let transfers finish, then try again.";
  if (status == B_ENTRY_NOT_FOUND)
    return text + "the disk changed. Refresh Disks and select it again.";
  if (status == B_NOT_ALLOWED && !mount)
    return text + "system or boot volumes are protected, or permission was denied.";
  return text + strerror(status);
}

} // namespace Listi
