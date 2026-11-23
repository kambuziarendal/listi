// SPDX-License-Identifier: MIT
#pragma once
#include "ReadWorker.h"
#include <Messenger.h>
#include <atomic>
#include <memory>
// Out-of-band terminal failure: even a failed completion message must release
// the pane from its reading state. The owning pane polls this while scanning.
struct DirectoryScanDelivery {
  std::atomic<int64> failedSerial{0};
};
Listi::ReadWorker *
CreateDirectoryScanner(BMessenger target,
                       std::shared_ptr<std::atomic<int64>> generation,
                       std::shared_ptr<DirectoryScanDelivery> delivery = {});
