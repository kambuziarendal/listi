// SPDX-License-Identifier: MIT
#include "SearchFilters.h"
#include <Message.h>
#include <UnicodeChar.h>
#include <cerrno>
#include <ctime>
#include <limits>
#include <stdexcept>
namespace Listi {
namespace {
std::string Trim(const std::string &s) {
  auto first = s.find_first_not_of(" \t");
  return first == std::string::npos
             ? ""
             : s.substr(first, s.find_last_not_of(" \t") - first + 1);
}
std::u32string Fold(const std::string &s) {
  std::u32string result;
  const char *p = s.c_str();
  while (*p)
    result += BUnicodeChar::ToLower(BUnicodeChar::FromUTF8(&p));
  return result;
}
int64_t Size(const std::string &raw) {
  auto s = Trim(raw);
  if (s.empty())
    return -1;
  size_t i = 0;
  int64_t value = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    int digit = s[i++] - '0';
    if (value > (INT64_MAX - digit) / 10)
      throw std::runtime_error("File size is too large.");
    value = value * 10 + digit;
  }
  if (!i)
    throw std::runtime_error(
        "Sizes must be whole numbers, optionally followed by B, KiB, MiB or GiB.");
  auto unit = Trim(s.substr(i));
  int64_t factor = 1;
  if (unit == "KiB")
    factor = 1024;
  else if (unit == "MiB")
    factor = 1024 * 1024;
  else if (unit == "GiB")
    factor = 1024LL * 1024 * 1024;
  else if (!unit.empty() && unit != "B")
    throw std::runtime_error("Use B, KiB, MiB or GiB for file sizes.");
  if (value > INT64_MAX / factor)
    throw std::runtime_error("File size is too large.");
  return value * factor;
}
int64_t Date(const std::string &raw, bool nextDay) {
  auto s = Trim(raw);
  if (s.empty())
    return INT64_MIN;
  auto invalid = [] {
    throw std::runtime_error(
        "Use a valid local date: YYYY-MM-DD (1970 to 9999).");
  };
  if (s.size() != 10 || s[4] != '-' || s[7] != '-')
    invalid();
  for (size_t i = 0; i < s.size(); ++i)
    if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9'))
      invalid();
  int year = std::stoi(s.substr(0, 4)), month = std::stoi(s.substr(5, 2)),
      day = std::stoi(s.substr(8, 2));
  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31)
    invalid();
  struct tm date {};
  date.tm_year = year - 1900;
  date.tm_mon = month - 1;
  date.tm_mday = day;
  date.tm_isdst = -1;
  time_t stamp = mktime(&date);
  if (stamp == time_t(-1) || date.tm_year != year - 1900 ||
      date.tm_mon != month - 1 || date.tm_mday != day)
    invalid();
  if (nextDay) {
    ++date.tm_mday;
    date.tm_hour = date.tm_min = date.tm_sec = 0;
    date.tm_isdst = -1;
    time_t next = mktime(&date);
    if (next <= stamp)
      invalid();
    stamp = next;
  }
  return stamp;
}
}
bool SearchFilters::Active() const {
  return !Trim(extension).empty() || !Trim(minimumSize).empty() ||
         !Trim(maximumSize).empty() || !Trim(fromDate).empty() ||
         !Trim(throughDate).empty();
}
CompiledSearchFilters CompileSearchFilters(const SearchFilters &f) {
  for (auto s :
       {f.extension, f.minimumSize, f.maximumSize, f.fromDate, f.throughDate})
    if (s.size() > 255 || s.find('\0') != std::string::npos)
      throw std::runtime_error("Invalid or oversized search filter.");
  CompiledSearchFilters out;
  out.active = f.Active();
  out.created = f.created;
  out.extension = Trim(f.extension);
  if (!out.extension.empty() && out.extension[0] == '.')
    out.extension.erase(0, 1);
  if ((!Trim(f.extension).empty() && out.extension.empty()) ||
      out.extension.find_first_of("/\\*?[] \t\r\n") != std::string::npos)
    throw std::runtime_error(
        "Enter a file extension such as pdf or tar.gz, without wildcards.");
  out.minimum = Size(f.minimumSize);
  out.maximum = Size(f.maximumSize);
  if (out.minimum >= 0 && out.maximum >= 0 && out.minimum > out.maximum)
    throw std::runtime_error(
        "Minimum file size must not exceed maximum file size.");
  out.from = Date(f.fromDate, false);
  out.until = Date(f.throughDate, true);
  if (out.from != INT64_MIN && out.until != INT64_MIN && out.from >= out.until)
    throw std::runtime_error("From date must not be later than through date.");
  return out;
}
bool CompiledSearchFilters::Matches(const std::string &path,
                                    const struct stat &st) const {
  if (!active)
    return true;
  // A file filter never matches a directory or the target of a symlink.
  if (!S_ISREG(st.st_mode))
    return false;
  if (minimum >= 0 && st.st_size < minimum)
    return false;
  if (maximum >= 0 && st.st_size > maximum)
    return false;
  int64_t stamp = created ? st.st_crtime : st.st_mtime;
  if (from != INT64_MIN && stamp < from)
    return false;
  if (until != INT64_MIN && stamp >= until)
    return false;
  if (!extension.empty()) {
    auto name = Fold(path.substr(path.find_last_of('/') + 1));
    auto suffix = Fold("." + extension);
    if (name.size() <= suffix.size() ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix))
      return false;
  }
  return true;
}
bool WriteSearchFilters(BMessage &m, const SearchFilters &f) {
  BMessage filters;
  return filters.AddString("extension", f.extension.c_str()) == B_OK &&
         filters.AddString("minimum", f.minimumSize.c_str()) == B_OK &&
         filters.AddString("maximum", f.maximumSize.c_str()) == B_OK &&
         filters.AddString("from", f.fromDate.c_str()) == B_OK &&
         filters.AddString("through", f.throughDate.c_str()) == B_OK &&
         filters.AddBool("created", f.created) == B_OK &&
         m.AddMessage("filters", &filters) == B_OK;
}
SearchFilters ReadSearchFilters(const BMessage &m) {
  type_code type;
  int32 count;
  if (m.GetInfo("filters", &type, &count) == B_NAME_NOT_FOUND)
    return {};
  BMessage filters;
  if (m.GetInfo("filters", &type, &count) != B_OK || type != B_MESSAGE_TYPE ||
      count != 1 || m.FindMessage("filters", &filters) != B_OK)
    throw std::runtime_error("Invalid saved file filters.");
  SearchFilters f;
  auto read = [&](const char *key, std::string &value) {
    const char *text;
    if (filters.GetInfo(key, &type, &count) != B_OK || type != B_STRING_TYPE ||
        count != 1 || filters.FindString(key, &text) != B_OK)
      throw std::runtime_error("Invalid saved file filter.");
    value = text;
  };
  read("extension", f.extension);
  read("minimum", f.minimumSize);
  read("maximum", f.maximumSize);
  read("from", f.fromDate);
  read("through", f.throughDate);
  if (filters.FindBool("created", &f.created) != B_OK)
    throw std::runtime_error("Invalid saved date field.");
  CompileSearchFilters(f);
  return f;
}
}
