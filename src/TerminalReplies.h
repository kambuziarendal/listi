// SPDX-License-Identifier: MIT
#pragma once
#include <Message.h>
#include <atomic>
#include <mutex>
#include <vector>

// Keep terminal replies that missed the window port. The view drains them on
// its next pulse; playback frames deliberately do not use this path.
class TerminalReplies {
  std::mutex lock;
  std::vector<BMessage> waiting;
public:
  template<class Send>
  void Deliver(BMessage &reply, const std::atomic<int64> &generation, Send send) {
    if (send() == B_OK)
      return;
    std::lock_guard<std::mutex> guard(lock);
    if (generation.load() == reply.GetInt64("serial", -1))
      waiting.emplace_back(reply);
  }
  std::vector<BMessage> Take() {
    std::vector<BMessage> result;
    std::lock_guard<std::mutex> guard(lock);
    result.swap(waiting);
    return result;
  }
};
