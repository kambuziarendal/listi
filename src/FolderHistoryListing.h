// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <map>
#include <string>
#include <sys/stat.h>

namespace Listi {
class HistoryStore;
struct HistoryRow {
  std::string path, revision, change;
  mode_t mode;
};

using HistoryRows = std::map<std::string, HistoryRow>;
HistoryRows DescribeVersion(const HistoryStore &, const std::string &point, const std::string &previous,
                        std::string &status, const std::atomic_bool &cancel);
}
