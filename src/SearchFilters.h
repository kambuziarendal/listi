// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>
#include <sys/stat.h>
class BMessage;
namespace Listi {
// Keep the user's calendar dates, not resolved timestamps, in saved criteria.
struct SearchFilters {
  std::string extension, minimumSize, maximumSize, fromDate, throughDate;
  bool created = false;
  bool Active() const;
};
struct CompiledSearchFilters {
  std::string extension;
  int64_t minimum = -1, maximum = -1, from = INT64_MIN, until = INT64_MIN;
  bool active = false, created = false;
  bool Matches(const std::string &path, const struct stat &) const;
};
// Throws on invalid criteria. Dates are local calendar days; upper bound is
// exclusive midnight of the following day, including DST transitions.
CompiledSearchFilters CompileSearchFilters(const SearchFilters &);
bool WriteSearchFilters(BMessage &, const SearchFilters &);
SearchFilters ReadSearchFilters(const BMessage &);
}
