// SPDX-License-Identifier: MIT
#include "RemovableCopy.h"
#include "StagingSpace.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace Listi {
namespace {
struct Descriptors {
  int source, staging;
  ~Descriptors() {
    if (source >= 0)
      close(source);
    if (staging >= 0)
      close(staging);
  }
};

// One chunk at a time: the queue owns pause/cancel, device checks and progress.
// No callback, queue pointer, path or permission to rename reaches this thread.
struct Transfer {
  std::array<char, 128 * 1024> buffer;
  std::mutex mutex;
  std::condition_variable changed;
  bool permit = true, finished = false;
  std::atomic<bool> stop{false};
  size_t bytes = 0;
  int error = 0;

  bool Next() {
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [&] { return permit || stop; });
    permit = false;
    return !stop;
  }

  void Progress(size_t count) {
    std::lock_guard<std::mutex> lock(mutex);
    bytes = count;
    changed.notify_one();
  }
};

int Stream(const Descriptors &fd, const struct stat &before, Transfer &state,
           bool privateStaging) {
  auto &buffer = state.buffer;
  while (state.Next()) {
    ssize_t count;
    do {
      if (state.stop)
        return ECANCELED;
      count = read(fd.source, buffer.data(), buffer.size());
    } while (count < 0 && errno == EINTR);
    if (count < 0)
      return errno;
    if (!count) {
      const struct timespec times[] = {before.st_atim, before.st_mtim};
      if (state.stop)
        return ECANCELED;
      if (fchmod(fd.staging, before.st_mode & 07777))
        return errno;
      if (state.stop)
        return ECANCELED;
      if (futimens(fd.staging, times))
        return errno;
      if (state.stop)
        return ECANCELED;
      if (fsync(fd.staging))
        return errno;
      return 0;
    }
    ssize_t offset = 0;
    while (offset < count) {
      if (state.stop)
        return ECANCELED;
      const auto n = privateStaging
                         ? WriteStaging(fd.staging, buffer.data() + offset, count - offset)
                         : write(fd.staging, buffer.data() + offset, count - offset);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return n < 0 ? errno : EIO;
      offset += n;
    }
    state.Progress(count);
  }
  return ECANCELED;
}
} // namespace

void WriteRemovableStagingFile(int source, int staging, const struct stat &before,
                               const std::function<void(size_t)> &checkpoint,
                               bool privateStaging) {
  // Allocate ownership before starting the thread. Even a spawn failure closes
  // both descriptors. A blocked close belongs to the I/O worker too.
  Descriptors owned{source, staging};
  auto state = std::make_shared<Transfer>();
  std::thread io([source, staging, before, state, privateStaging] {
    int error;
    {
      Descriptors fd{source, staging};
      error = Stream(fd, before, *state, privateStaging);
      if (close(fd.staging) && !error)
        error = errno;
      fd.staging = -1;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    state->error = error;
    state->finished = true;
    state->changed.notify_one();
  });
  owned.source = owned.staging = -1;
  try {
    for (;;) {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->changed.wait_for(lock, std::chrono::milliseconds(100),
                              [&] { return state->finished || state->bytes; });
      const auto bytes = state->bytes;
      const bool finished = state->finished;
      state->bytes = 0;
      lock.unlock();
      checkpoint(bytes);
      if (finished)
        break;
      if (bytes) {
        lock.lock();
        state->permit = true;
        state->changed.notify_one();
      }
    }
  } catch (...) {
    std::unique_lock<std::mutex> lock(state->mutex);
    state->stop = true;
    state->changed.notify_one();
    // Ordinary cancellation drains promptly and permits normal stage cleanup.
    // Never touch the suspect mount to clean up if the kernel is still stuck.
    const bool drained = state->changed.wait_for(
        lock, std::chrono::milliseconds(250), [&] { return state->finished; });
    lock.unlock();
    if (!drained) {
      io.detach();
      throw RemovableCopyInterrupted(
          "Transfer stopped; disk I/O is still pending. No final file was "
          "published for this entry. Check the disk for a temporary copy "
          "after reconnecting.");
    }
    io.join();
    throw;
  }
  io.join();
  // The driver can fail the descriptor before its devfs node disappears.
  // Suspend the suspected endpoint even if the last devfs checkpoint passed.
  if (state->error == EIO || state->error == ENODEV || state->error == ENXIO)
    throw RemovableCopyInterrupted(
        "Removable disk I/O failed; no final file was published for this "
        "entry. Check for a temporary copy after reconnecting.");
  if (state->error)
    throw std::runtime_error(std::string("Transfer involving removable disk: ") +
                             strerror(state->error));
}
} // namespace Listi
