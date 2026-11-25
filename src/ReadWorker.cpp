// SPDX-License-Identifier: MIT
#include "ReadWorker.h"
#include <Messenger.h>
#include <array>
#include <mutex>

namespace Listi {
namespace {
// Reserve at start, not at retirement: even repeated window creation/detach
// against an unavailable volume cannot create unbounded orphaned readers.
std::mutex slotsLock;
std::array<thread_id, kReadWorkerLimit> slots{};

bool Finished(thread_id thread) {
  thread_info info;
  return thread <= 0 || get_thread_info(thread, &info) == B_BAD_THREAD_ID;
}
}

void ReadWorker::DispatchMessage(BMessage *message, BHandler *handler) {
  if (retired.load()) {
    Quit();
    return;
  }
  BLooper::DispatchMessage(message, handler);
}

bool StartReadWorker(ReadWorker *&worker) {
  std::lock_guard<std::mutex> guard(slotsLock);
  for (auto &slot : slots) {
    if (!Finished(slot))
      continue;
    slot = worker->Run();
    if (slot >= 0)
      return true;
    break;
  }
  worker->Quit();
  worker = nullptr;
  return false;
}

void RetireReadWorker(ReadWorker *&worker) {
  if (!worker)
    return;
  BMessenger wake(worker);
  const auto thread = worker->Thread();
  worker->retired.store(true);
  worker = nullptr;
  // The worker may already have exited. A full port also suffices: its next
  // dispatch observes retirement instead of starting another read.
  BMessage quit(B_QUIT_REQUESTED);
  wake.SendMessage(&quit, (BHandler *)nullptr, 0);
  status_t result;
  wait_for_thread_etc(thread, B_RELATIVE_TIMEOUT, 10000, &result);
}

bool DrainReadWorkers(bigtime_t budget) {
  const auto deadline = system_time() + budget;
  std::lock_guard<std::mutex> guard(slotsLock);
  for (const auto thread : slots) {
    if (Finished(thread))
      continue;
    status_t result;
    status_t wait;
    do {
      wait = wait_for_thread_etc(thread, B_ABSOLUTE_TIMEOUT, deadline, &result);
    } while (wait == B_INTERRUPTED && system_time() < deadline);
    if (wait != B_OK && !Finished(thread))
      return false;
  }
  return true;
}
}
