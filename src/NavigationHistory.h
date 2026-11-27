// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Listi {
struct VisitPosition {
  using Identity = std::pair<int64_t, int64_t>;
  static constexpr size_t SelectionLimit = 64;
  std::map<std::string, Identity> selected;
  std::string focus, anchor;
  Identity focusId{-1, -1}, anchorId{-1, -1};
  float x = 0, y = 0, offset = 0;
  std::string archiveVersion;
};

// Only the latest successful scan may commit a visit. Addresses are normalized
// by the caller, with authentication kept separately in the connection store.
class NavigationHistory {
public:
  static constexpr size_t Limit = 64;
  int Back() const { return cursor < 0 ? -1 : detached ? cursor : cursor - 1; }
  int Forward() const {
    return !detached && cursor >= 0 && cursor + 1 < int(visits.size())
               ? cursor + 1
               : -1;
  }
  const std::string &At(int index) const { return visits.at(index).path; }
  const std::optional<VisitPosition> &PositionAt(int index) const {
    return visits.at(index).position;
  }
  void SavePosition(VisitPosition position) {
    if (cursor >= 0 && !detached && !waiting)
      visits[cursor].position = std::move(position);
  }
  size_t Size() const { return visits.size(); }
  int PendingTarget() const { return waiting ? destination : -1; }
  int Position() const { return cursor; }
  const std::vector<std::string> &RecentFolders() const { return recentFolders; }
  void Begin(int64_t serial, const std::string &requested, int target = -1,
             bool unresolved = false) {
    pending = serial;
    destination = target;
    waiting = true;
    detached = unresolved || cursor < 0 || requested != visits[cursor].path;
  }
  bool Complete(int64_t serial, const std::string &resolved, bool success) {
    if (!waiting || serial != pending)
      return false;
    waiting = false;
    if (!success) {
      detached = true;
      return true;
    }
    if (destination >= 0 && destination < int(visits.size())) {
      cursor = destination;
      visits[cursor].path = resolved;
    } else if (cursor < 0 || visits[cursor].path != resolved) {
      visits.resize(cursor + 1);
      visits.push_back({resolved, {}});
      if (visits.size() > Limit)
        visits.erase(visits.begin());
      cursor = int(visits.size()) - 1;
    }
    detached = false;
    // The volume overview is navigable but is not itself a folder.
    if (resolved != "@disks" && resolved.rfind("@query:", 0) != 0 &&
        resolved.rfind("@set-query:", 0) != 0 &&
        (recentFolders.empty() || recentFolders.front() != resolved)) {
      auto previous = std::find(recentFolders.begin(), recentFolders.end(), resolved);
      if (previous != recentFolders.end())
        recentFolders.erase(previous);
      recentFolders.insert(recentFolders.begin(), resolved);
      if (recentFolders.size() > Limit)
        recentFolders.pop_back();
    }
    return true;
  }

private:
  struct Visit { std::string path; std::optional<VisitPosition> position; };
  std::vector<Visit> visits;
  // Visit order is independent of the Back/Forward branch and its cursor.
  std::vector<std::string> recentFolders;
  int cursor = -1, destination = -1;
  int64_t pending = 0;
  bool waiting = false, detached = false;
};
} // namespace Listi
