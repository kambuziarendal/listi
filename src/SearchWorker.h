// SPDX-License-Identifier: MIT
#pragma once
#include "ReadWorker.h"
#include <Messenger.h>
#include <atomic>
#include <memory>
namespace Listi {
constexpr uint32 kSearchStart = 'ssrt', kSearchStop = 'sstp',
                 kSearchRows = 'shit', kSearchDone = 'sfin',
                 kSearchBegin = 'sbeg';
// The caller invalidates generation before Stop or Quit, interrupting scans.
ReadWorker *NewSearchWorker(BMessenger target,
                         std::shared_ptr<std::atomic<int64>> generation,
                         std::shared_ptr<std::atomic<int64>> failedSerial = {});
}
