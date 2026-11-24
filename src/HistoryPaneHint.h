// SPDX-License-Identifier: MIT
#pragma once

#include <Messenger.h>
#include <array>
#include <string>

namespace Listi {
inline constexpr uint32 kReadHistoryConfig = 'fhcf';
inline constexpr int32 kUnknownVersions = -1, kUnavailableVersions = -2;
bool RequestHistoryConfig(BMessenger target, const std::array<std::string, 2> &visible);
}
