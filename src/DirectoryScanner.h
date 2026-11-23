// SPDX-License-Identifier: MIT
#pragma once
#include "ReadWorker.h"
#include <Messenger.h>
#include <NodeMonitor.h>
#include <mutex>
#include <optional>
#include <atomic>
#include <memory>
// Out-of-band terminal failure: even a failed completion message must release
// the pane from its reading state. The owning pane polls this while scanning.
inline constexpr uint32 kReadVisibleIcon = 'rvic';
inline constexpr uint32 kVisibleIconReady = 'ivir';
struct DirectoryScanDelivery {
  std::atomic<int64> failedSerial{0};
};
// The pane and its retired scanner may briefly coexist. Register against the
// opened directory, but never install a stale reader's watch after navigation.
class DirectoryWatch {
  std::mutex lock;
  std::optional<node_ref> node;
  int64 current = -1;

public:
  void Begin(int64 serial, bool keep, BMessenger target) {
    std::lock_guard<std::mutex> guard(lock);
    current = serial;
    if (!keep && node) {
      watch_node(&*node, B_STOP_WATCHING, target);
      node.reset();
    }
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
    auto status = watch_node(&opened, B_WATCH_ALL, target);
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
