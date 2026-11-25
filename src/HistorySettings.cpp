// SPDX-License-Identifier: MIT
#include "HistorySettings.h"
#include "MessageStore.h"
#include <FindDirectory.h>
#include <Path.h>
#include <stdexcept>

namespace Listi {
std::string HistorySettingsFile() {
  BPath path;
  if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK ||
      path.Append("Listi_automatic_history") != B_OK)
    throw std::runtime_error("Cannot find user settings.");
  return path.Path();
}

std::vector<BMessage> HistoryPairs(const BMessage &config) {
  std::vector<BMessage> pairs;
  BMessage pair;
  for (int32 i = 0; config.FindMessage("pair", i, &pair) == B_OK; ++i) {
    if (pairs.size() == kHistoryPairLimit || !*pair.GetString("path", "") ||
        !pair.HasString("path", 1))
      throw std::runtime_error("Invalid automatic history settings.");
    pairs.push_back(pair);
  }
  return pairs;
}

BMessage ReadHistorySettings() {
  auto config = LoadMessageStore(HistorySettingsFile(), true);
  if (config.what != 'none' && config.what != kHistoryConfigFormat)
    throw std::runtime_error("Cannot read automatic history settings.");
  return config;
}

BMessage HistoryPairFor(const BMessage &config, const std::string &source) {
  for (const auto &pair : HistoryPairs(config))
    if (source == pair.GetString("path", 0, ""))
      return pair;
  return BMessage();
}
}
