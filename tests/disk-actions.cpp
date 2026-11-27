// SPDX-License-Identifier: MIT
#include "DiskActions.h"
#include "Pane.h"
#include "WrappedLabel.h"
#include <Application.h>
#include <Directory.h>
#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <Entry.h>
#include <Looper.h>
#include <Message.h>
#include <Window.h>
#include <atomic>
#include <cstring>
#include <OS.h>
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
std::atomic<int32> failOffsetFor{-1};
std::atomic<bool> failedOffset{false};
}

extern "C" status_t __real__ZN8BMessage8AddInt64EPKcl(BMessage *, const char *, int64);
extern "C" status_t __wrap__ZN8BMessage8AddInt64EPKcl(BMessage *m, const char *field, int64 value) {
  const auto id = failOffsetFor.load();
  if (id >= 0 && strcmp(field, "partitionOffset") == 0 && m->GetInt32("partition", -1) == id) {
    failedOffset = true;
    return B_NO_MEMORY;
  }
  return __real__ZN8BMessage8AddInt64EPKcl(m, field, value);
}

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

class DiskUpdates : public BLooper {
public:
  explicit DiskUpdates(int32 partition)
      : BLooper("disk update test"), partition(partition),
        mounted(create_sem(0, "partition mounted")),
        unmounted(create_sem(0, "partition unmounted")) {
    assert(mounted >= 0 && unmounted >= 0);
  }

  ~DiskUpdates() override {
    delete_sem(mounted);
    delete_sem(unmounted);
  }

  void MessageReceived(BMessage *message) override {
    if (message->what != kDiskDeviceUpdate ||
        message->GetInt32("id", -1) != partition) {
      BLooper::MessageReceived(message);
      return;
    }
    const auto event = message->GetInt32("event", -1);
    if (event == B_DEVICE_PARTITION_MOUNTED)
      release_sem(mounted);
    if (event == B_DEVICE_PARTITION_UNMOUNTED)
      release_sem(unmounted);
  }

  void Expect(int32 event) const {
    const auto sem = event == B_DEVICE_PARTITION_MOUNTED ? mounted : unmounted;
    assert(acquire_sem_etc(sem, 1, B_RELATIVE_TIMEOUT, 5000000) == B_OK);
  }

private:
  int32 partition;
  sem_id mounted, unmounted;
};

void VerifyDiskListing(const DiskListing &fixture) {
  Preferences prefs;
  prefs.icons = false;
  auto *window = new BWindow(BRect(10, 10, 650, 470), "Disk listing",
                             B_TITLED_WINDOW, 0);
  auto *pane = new Pane(0, prefs);
  window->AddChild(pane);
  window->Show();
  assert(window->Lock());
  auto waitForScan = [&] {
    const auto deadline = system_time() + 5000000;
    while (pane->IsScanning() && system_time() < deadline) {
      window->Unlock();
      snooze(10000);
      assert(window->Lock());
    }
    assert(!pane->IsScanning());
  };
  failedOffset = false;
  failOffsetFor = fixture.partition.id;
  pane->Navigate("@disks");
  waitForScan();
  auto notice = dynamic_cast<WrappedLabel *>(pane->FindView("status"));
  assert(failedOffset && notice && strstr(notice->Text(), "Reload"));
  assert(!pane->LocationValid() && pane->MatchPattern("ListiMountQA*", false) == 0 &&
         !pane->SelectedDisk());
  failOffsetFor = -1;
  pane->Reload();
  waitForScan();
  assert(pane->LocationValid() && pane->MatchPattern("ListiMountQA*", true) == 1);
  const auto *row = pane->SelectedDisk();
  assert(row && row->partition && *row->partition == fixture.partition &&
         row->unmountedDisk == (fixture.mountedDevice < 0));
  window->Quit();
}

void VerifyRoundTrip(const char *devicePath) {
  const auto first = FindFixture(devicePath);
  assert(first.mountedDevice < 0);
  VerifyDiskListing(first);
  auto *updates = new DiskUpdates(first.partition.id);
  assert(updates->Run() >= 0);
  const BMessenger updateTarget(updates);
  assert(WatchDiskPartitions(updateTarget) == B_OK);
  auto stale = first.partition;
  ++stale.offset;
  assert(MountDisk(stale) == B_ENTRY_NOT_FOUND);
  auto differentContent = first.partition;
  differentContent.contentName = "Different disk";
  assert(MountDisk(differentContent) == B_ENTRY_NOT_FOUND);
  assert(MountDisk(first.partition) == B_OK);
  updates->Expect(B_DEVICE_PARTITION_MOUNTED);

  const auto mounted = FindFixture(devicePath);
  assert(mounted.mountedDevice >= 0 && !mounted.protectedVolume);
  VerifyDiskListing(mounted);
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
  updates->Expect(B_DEVICE_PARTITION_UNMOUNTED);
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
  StopWatchingDiskPartitions(updateTarget);
  assert(updates->Lock());
  updates->Quit();
  puts("File-backed BFS: stale, busy, mount notifications, remount and data verified");
}

} // namespace

int main() {
  BApplication app("application/x-vnd.Listi-DiskActionsTest");
  DiskPartition partition{42, "/dev/disk/test", "bfs", 100, 1024, "Label"};
  BMessage message;
  assert(partition.WriteTo(message));
  assert(DiskPartition::ReadFrom(message) == partition);
  assert(message.RemoveName("partitionOffset") == B_OK && DiskPartition::ReadFrom(message).id < 0);
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
