// SPDX-License-Identifier: MIT
#include "FolderHistoryListing.h"
#include "FolderHistory.h"
#include <stdexcept>

namespace Listi {
namespace {
constexpr size_t kDisplayLimit = 2048;
void Collect(const BMessage &dir, const std::string &prefix, HistoryRows &rows, unsigned depth,
             bool &truncated, const std::atomic_bool &cancel) {
  BMessage child;
  for (int32 i = 0; dir.FindMessage("child", i, &child) == B_OK; ++i) {
    if (cancel) throw std::runtime_error("Canceled.");
    if (rows.size() >= kDisplayLimit || depth > 128) { truncated = true; return; }
    std::string name = child.GetString("name", "");
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos)
      throw std::runtime_error("Invalid name in saved version.");
    auto path = prefix + name;
    mode_t mode = child.GetInt32("mode", 0);
    auto revision = std::string(child.GetString("digest", child.GetString("target", "")));
    rows.emplace(path, HistoryRow{path, std::move(revision), {}, mode});
    if (S_ISDIR(mode)) Collect(child, path + "/", rows, depth + 1, truncated, cancel);
    if (truncated) return;
  }
}

}

HistoryRows DescribeVersion(const HistoryStore &history, const std::string &point, const std::string &previous,
                        std::string &status, const std::atomic_bool &cancel) {
  HistoryRows rows;
  bool truncated = false;
  Collect(history.Browse(point), "", rows, 0, truncated, cancel);
  if (!previous.empty()) {
    HistoryRows before;
    Collect(history.Browse(previous), "", before, 0, truncated, cancel);
    if (!truncated) {
      for (auto &[path, row] : rows) {
        auto it = before.find(path);
        if (it == before.end()) row.change = "Added";
        else {
          if (it->second.mode != row.mode || it->second.revision != row.revision)
            row.change = "Changed";
          before.erase(it);
        }
      }
      for (auto &[path, row] : before) {
        row.change = "Removed";
        row.mode = 0;
        rows.emplace(path, std::move(row));
      }
    }
  }
  status = truncated ? "Listing limited; restore uses the saved version."
      : rows.empty() ? "This saved folder was empty."
      : previous.empty() ? "Read-only · Oldest saved version."
      : "Read-only · Changes since the preceding version.";
  return rows;
}
}
