// SPDX-License-Identifier: MIT
#pragma once
#include "SearchFilters.h"
#include <cstdint>
#include <functional>
#include <string>
class BQuery;
namespace Listi {
struct SearchRequest {
  std::string root, expression;
  bool predicate = false, includeTrash = false;
  size_t resultLimit = 5000, visitLimit = 250000;
  SearchFilters filters;
  bool flat = false; // Recursive regular files with a root-relative parent.

  // Shared by saved queries and worker messages; not execution limits or flat mode.
  bool WriteCriteria(BMessage &) const;
  static SearchRequest ReadCriteria(const BMessage &);
};
struct SearchHit {
  std::string path;
  int64_t device = -1, node = -1;
  std::string relativeFolder = {};
  int64_t size = 0, modified = 0;
};
struct SearchSummary {
  size_t visited = 0, matched = 0, skipped = 0;
  bool cancelled = false, limited = false;
  std::string error;
};
// Read-only snapshot. Callbacks execute on the caller's worker, never a UI thread.
// Returning false from deliver stops the search; it never silently drops rows.
SearchSummary SearchFiles(const SearchRequest &,
                          const std::function<bool()> &proceed,
                          const std::function<bool(const SearchHit &)> &deliver,
                          BQuery *liveQuery = nullptr);
bool SameSearchEntry(const SearchHit &);
} // namespace Listi
