// SPDX-License-Identifier: MIT
#pragma once
#include <Looper.h>
#include <atomic>

namespace Listi {
constexpr size_t kReadWorkerLimit = 64;

// A reader owns its data, never its view. Retiring a reader revokes new work
// and wakes the looper without waiting on the lock held across blocking I/O.
class ReadWorker : public BLooper {
public:
  using BLooper::BLooper;
  void DispatchMessage(BMessage *message, BHandler *handler) override;

private:
  std::atomic<bool> retired{false};
  friend void RetireReadWorker(ReadWorker *&);
};

bool StartReadWorker(ReadWorker *&worker);
void RetireReadWorker(ReadWorker *&worker);
bool DrainReadWorkers(bigtime_t budget);
}
