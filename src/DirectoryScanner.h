// SPDX-License-Identifier: MIT
#pragma once
#include "ReadWorker.h"
#include <Messenger.h>
#include <NodeMonitor.h>
#include <mutex>
#include <optional>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
// Out-of-band terminal failure: even a failed completion message must release
// the pane from its reading state. The owning pane polls this while scanning.
inline constexpr uint32 kReadVisibleIcon = 'rvic';
inline constexpr uint32 kVisibleIconReady = 'ivir';
struct DirectoryScanDelivery {
  explicit DirectoryScanDelivery(int maxPending = 0) : limit(maxPending) {}
  std::atomic<int64> failedSerial{0};

  // The window port can empty into a much larger looper queue. Limit rows
  // waiting on the pane itself so user input need not wait behind a full scan.
  bool ReserveBatch(const std::atomic<int64> &generation, int64 serial) {
    if (!limit)
      return true;
    std::unique_lock<std::mutex> guard(lock);
    // A failed terminal notification can end a scan before the pane handles
    // its last rows. An unacknowledged old batch must not consume a new slot.
    if (current != serial) {
      current = serial;
      pending = 0;
    }
    while (pending >= limit && generation.load() == serial)
      room.wait_for(guard, std::chrono::milliseconds(50));
    if (generation.load() != serial)
      return false;
    ++pending;
    return true;
  }
  void ReleaseBatch(int64 serial) {
    if (!limit)
      return;
    std::lock_guard<std::mutex> guard(lock);
    if (current == serial && pending > 0) {
      --pending;
      room.notify_one();
    }
  }

private:
  const int limit;
  std::mutex lock;
  std::condition_variable room;
  int64 current = -1;
  int pending = 0;
};
// A retired scanner must never install a watch after its pane has navigated.
// A pane watches either the opened directory or changes to the volume roster.
class DirectoryWatch {
  std::mutex lock;
  std::optional<node_ref> node;
  int64 current = -1;
  bool watchingVolumes = false;

public:
  void Begin(int64 serial, bool keep, BMessenger target) {
    std::lock_guard<std::mutex> guard(lock);
    current = serial;
    if (keep)
      return;
    if (watchingVolumes) {
      // The mount watch has no node_ref, and no directory watch coexists.
      stop_watching(target);
      watchingVolumes = false;
    } else if (node) {
      watch_node(&*node, B_STOP_WATCHING, target);
      node.reset();
    }
  }
  status_t InstallVolumes(int64 serial, BMessenger target) {
    std::lock_guard<std::mutex> guard(lock);
    if (current != serial)
      return B_CANCELED;
    if (watchingVolumes)
      return B_OK;
    const auto status = watch_node(nullptr, B_WATCH_MOUNT, target);
    watchingVolumes = status == B_OK;
    return status;
  }
  status_t Install(int64 serial, node_ref opened, BMessenger target) {
    std::lock_guard<std::mutex> guard(lock);
    if (current != serial)
      return B_CANCELED;
    if (node && node->device == opened.device && node->node == opened.node)
      return B_OK;
    if (node) {
      watch_node(&*node, B_STOP_WATCHING, target);
      node.reset();
    }
    auto status = watch_node(&opened, B_WATCH_ALL | B_WATCH_CHILDREN, target);
    if (status == B_OK)
      node = opened;
    return status;
  }
};
Listi::ReadWorker *
CreateDirectoryScanner(BMessenger target,
                       std::shared_ptr<std::atomic<int64>> generation,
                       std::shared_ptr<DirectoryScanDelivery> delivery = {},
                       std::shared_ptr<DirectoryWatch> watch = {});
