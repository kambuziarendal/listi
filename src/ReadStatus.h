// SPDX-License-Identifier: MIT
#pragma once
#include <StringView.h>
#include <functional>
#include <utility>

namespace Listi {
// Only the window's own looper consumes a result. No worker posts to or
// touches a view, even if its filesystem call returns after window teardown.
class ReadStatus : public BStringView {
  std::function<void()> collect;
public:
  ReadStatus(const char *name, const char *text, std::function<void()> collect)
      : BStringView(name, text), collect(std::move(collect)) {
    SetFlags(Flags() | B_PULSE_NEEDED);
  }
  void Pulse() override { collect(); }
};

}
