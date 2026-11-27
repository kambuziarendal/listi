// SPDX-License-Identifier: MIT
#pragma once
#include <Message.h>
#include <atomic>

// Each native test runs in its own process. Fail the second row of just the
// armed scanner, leaving BWindow and unrelated workers' messages untouched.
namespace ScanFault {
enum Mode { None, Append, Decode };
inline std::atomic<Mode> mode{None};
inline std::atomic<int> calls{0};
inline std::atomic<uint32> kind{0};
inline void Arm(Mode fault, uint32 what) {
  kind = what;
  calls = 0;
  mode = fault;
}
inline bool Fail(Mode fault, const BMessage &message) {
  return mode == fault && kind == message.what && ++calls == 2;
}
}

extern "C" status_t __real__ZN8BMessage10AddMessageEPKcPKS_(
    BMessage *, const char *, const BMessage *);
extern "C" status_t __wrap__ZN8BMessage10AddMessageEPKcPKS_(
    BMessage *message, const char *field, const BMessage *row) {
  if (ScanFault::Fail(ScanFault::Append, *message))
    return B_NO_MEMORY;
  return __real__ZN8BMessage10AddMessageEPKcPKS_(message, field, row);
}
extern "C" status_t __real__ZNK8BMessage11FindMessageEPKciPS_(
    const BMessage *, const char *, int32, BMessage *);
extern "C" status_t __wrap__ZNK8BMessage11FindMessageEPKciPS_(
    const BMessage *message, const char *field, int32 index, BMessage *row) {
  if (ScanFault::Fail(ScanFault::Decode, *message))
    return B_NO_MEMORY;
  return __real__ZNK8BMessage11FindMessageEPKciPS_(message, field, index, row);
}
