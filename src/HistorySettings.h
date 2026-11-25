// SPDX-License-Identifier: MIT
#pragma once

#include <Message.h>
#include <string>
#include <vector>

namespace Listi {
inline constexpr size_t kHistoryPairLimit = 3;
inline constexpr uint32 kHistoryConfigFormat = 'aH01';
std::string HistorySettingsFile();
std::vector<BMessage> HistoryPairs(const BMessage &config);
BMessage ReadHistorySettings();
BMessage HistoryPairFor(const BMessage &config, const std::string &source);
}
