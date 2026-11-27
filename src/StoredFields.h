// SPDX-License-Identifier: MIT
#pragma once
#include <Message.h>
#include <limits>
#include <stdexcept>
#include <string>

namespace Listi {
inline void CheckStoredField(status_t status) {
  if (status != B_OK)
    throw std::runtime_error("Could not encode the complete settings.");
}

// A missing list is empty; failure to read an advertised item is never EOF.
inline int32 StoredCount(const BMessage &message, const char *field,
                         size_t limit = std::numeric_limits<int32>::max()) {
  type_code type;
  int32 count;
  const auto status = message.GetInfo(field, &type, &count);
  if (status == B_NAME_NOT_FOUND)
    return 0;
  if (status != B_OK || type != B_MESSAGE_TYPE || count <= 0 || size_t(count) > limit)
    throw std::runtime_error(std::string("Invalid stored list: ") + field);
  return count;
}

inline BMessage StoredItem(const BMessage &message, const char *field, int32 index) {
  BMessage item;
  if (message.FindMessage(field, index, &item) != B_OK)
    throw std::runtime_error(std::string("Could not read stored item: ") + field);
  return item;
}
}
