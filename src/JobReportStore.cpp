// SPDX-License-Identifier: MIT
#include "JobReportStore.h"
#include "FileOperation.h"

#include <sqlite3.h>
#include <algorithm>
#include <map>
#include <mutex>

namespace Listi {
namespace {
struct Statement {
  sqlite3_stmt *stmt = nullptr;
  ~Statement() { sqlite3_finalize(stmt); }

  bool Prepare(sqlite3 *db, const char *sql) {
    return sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK;
  }
};
const char *String(sqlite3_stmt *stmt, int column) {
  const auto *value = sqlite3_column_text(stmt, column);
  return value ? reinterpret_cast<const char *>(value) : "";
}
bool Bind(sqlite3_stmt *stmt, int column, const std::string &value) {
  return sqlite3_bind_text64(stmt, column, value.data(), value.size(),
                             SQLITE_TRANSIENT, SQLITE_UTF8) == SQLITE_OK;
}
EntryOutcome ReadOutcome(sqlite3_stmt *row) {
  return {String(row, 0), String(row, 1), String(row, 2), String(row, 3)};
}
std::string Detail(const EntryOutcome &entry) {
  std::string detail = entry.detail;
  if (entry.upload) {
    const auto &upload = *entry.upload;
    if (upload.bytesWritten)
      detail += "\n  Bytes acknowledged by transport: " + std::to_string(*upload.bytesWritten);
    if (upload.destinationSizeChecked)
      detail += "\n  Staged file size checked.";
    if (upload.contentVerified)
      detail += "\n  Staged contents verified.";
    if (upload.publication == Publication::NotAttempted)
      detail += "\n  Final name not published.";
    else if (upload.publication == Publication::Unknown)
      detail += "\n  Destination publication not confirmed.";
    else if (upload.publication == Publication::Published)
      detail += "\n  Destination published.";
    if (upload.sourceRetained)
      detail += "\n  Source retained.";
    for (const auto &path : upload.recoveryPaths)
      detail += "\n  Recovery path: " + path;
  }
  return detail;
}
constexpr size_t kEmergencyRows = 16;
} // namespace

std::string OutcomeLabel(const std::string &status) {
  if (status == "source-retained")
    return "Copied; source retained";
  if (status == "published-flush-failed")
    return "Published; directory flush failed";
  if (status == "partial")
    return "Partially completed";
  if (status == "unprocessed")
    return "Not processed";
  if (status.empty())
    return "Unknown";
  std::string label(status);
  if (label.front() >= 'a' && label.front() <= 'z')
    label.front() -= 'a' - 'A';
  return label;
}

struct JobReportStore::State {
  sqlite3 *db = nullptr;
  Statement write, find, error;
  mutable std::mutex lock;
  size_t outcomes = 0, errors = 0, storedErrors = 0;
  bool incomplete = false;
  // Keep a small high-priority sample if the temp filesystem fails. The real
  // transfer and its separate recovery journals must remain independent.
  std::map<std::string, EntryOutcome> important;
  std::vector<std::string> recentErrors;

  ~State() { sqlite3_close_v2(db); }
};

JobReportStore::JobReportStore() : state(std::make_unique<State>()) {
  auto &s = *state;
  // Empty filename is SQLite's private temporary database, removed on close.
  // Set the temp store and cache explicitly; the history's page cache is small.
  s.incomplete =
      sqlite3_open_v2("", &s.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      nullptr) != SQLITE_OK ||
      sqlite3_exec(s.db,
          "PRAGMA temp_store=FILE; PRAGMA cache_size=-128;"
          "PRAGMA journal_mode=MEMORY; PRAGMA synchronous=OFF;"
          "CREATE TABLE entries(id INTEGER PRIMARY KEY, lookup TEXT UNIQUE, src TEXT NOT NULL,"
          " dst TEXT NOT NULL, status TEXT NOT NULL, detail TEXT NOT NULL);"
          "CREATE TABLE errors(id INTEGER PRIMARY KEY, message TEXT NOT NULL);",
          nullptr, nullptr, nullptr) != SQLITE_OK ||
      !s.write.Prepare(s.db,
          "INSERT INTO entries(lookup,src,dst,status,detail) VALUES(?,?,?,?,?) "
          "ON CONFLICT(lookup) DO UPDATE SET dst=excluded.dst,"
          "status=excluded.status,detail=excluded.detail") ||
      !s.find.Prepare(s.db, "SELECT src,dst,status,detail FROM entries WHERE lookup=?") ||
      !s.error.Prepare(s.db, "INSERT INTO errors(message) VALUES(?)");
}
JobReportStore::~JobReportStore() = default;

EntryOutcome JobReportStore::FindOutcome(const std::string &source) {
  auto &s = *state;
  if (!s.incomplete && Bind(s.find.stmt, 1, source)) {
    const int status = sqlite3_step(s.find.stmt);
    auto row = status == SQLITE_ROW ? ReadOutcome(s.find.stmt)
                                    : EntryOutcome{source, "", "unprocessed", ""};
    sqlite3_reset(s.find.stmt);
    if (status == SQLITE_DONE || status == SQLITE_ROW)
      return row;
    s.incomplete = true;
  }
  s.incomplete = true;
  auto found = s.important.find(source);
  return found == s.important.end() ? EntryOutcome{source, "", "unprocessed", ""}
                                    : found->second;
}

void JobReportStore::WriteOutcome(const EntryOutcome &row, bool update) {
  auto &s = *state;
  const std::string detail = Detail(row);
  if (!s.incomplete) {
    auto stmt = s.write.stmt;
    sqlite3_set_last_insert_rowid(s.db, 0);
    const bool key = update ? Bind(stmt, 1, row.source)
                            : sqlite3_bind_null(stmt, 1) == SQLITE_OK;
    const bool ok = key && Bind(stmt, 2, row.source) &&
                    Bind(stmt, 3, row.destination) && Bind(stmt, 4, row.status) &&
                    Bind(stmt, 5, detail) && sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_reset(stmt);
    if (ok) {
      if (sqlite3_last_insert_rowid(s.db) != 0)
        ++s.outcomes;
      return;
    }
    s.incomplete = true;
  }
  if (!detail.empty() || row.status == "partial" || row.status == "failed" ||
      row.status == "outcome unknown" || row.status.rfind("published-", 0) == 0) {
    if (s.important.size() == kEmergencyRows && !s.important.count(row.source))
      s.important.erase(s.important.begin());
    s.important[row.source] = {row.source, row.destination, row.status, detail};
  }
}

void JobReportStore::AddError(const std::string &message) {
  auto &s = *state;
  ++s.errors;
  if (!s.incomplete && Bind(s.error.stmt, 1, message)) {
    const bool written = sqlite3_step(s.error.stmt) == SQLITE_DONE;
    sqlite3_reset(s.error.stmt);
    if (written) {
      ++s.storedErrors;
      return;
    }
  }
  s.incomplete = true;
  if (s.recentErrors.size() == kEmergencyRows)
    s.recentErrors.erase(s.recentErrors.begin());
  s.recentErrors.push_back(message);
}

void JobReportStore::Seal() {
  // No more source updates after handoff. Return SQLite's cache memory.
  if (state->db)
    sqlite3_db_release_memory(state->db);
}
bool JobReportStore::Incomplete() const { return state->incomplete; }
size_t JobReportStore::OutcomeCount() const { return state->outcomes; }
size_t JobReportStore::ErrorCount() const { return state->errors; }

ReportPage JobReportStore::Page(size_t start, size_t limit) const {
  const auto &s = *state;
  std::lock_guard<std::mutex> guard(s.lock);
  ReportPage page;
  const size_t errors = s.storedErrors + s.recentErrors.size();
  page.total = errors + s.outcomes + s.important.size();
  page.incomplete = s.incomplete;
  if (start >= page.total)
    return page;
  const auto end = start + std::min({page.total - start, limit, kPageSize});
  auto read = [&](const char *sql, size_t first, size_t last, auto consume) {
    if (first >= last)
      return;
    Statement query;
    if (!query.Prepare(s.db, sql) ||
        sqlite3_bind_int64(query.stmt, 1, first + 1) != SQLITE_OK ||
        sqlite3_bind_int64(query.stmt, 2, last) != SQLITE_OK) {
      page.incomplete = true;
      return;
    }
    size_t count = 0;
    while (sqlite3_step(query.stmt) == SQLITE_ROW) {
      consume(query.stmt);
      ++count;
    }
    page.incomplete |= count != last - first;
  };
  // Put errors and any emergency details first so failures are easy to find.
  read("SELECT message FROM errors WHERE id BETWEEN ? AND ? ORDER BY id",
       start, std::min(end, s.storedErrors), [&](sqlite3_stmt *row) {
         page.errors.emplace_back(String(row, 0));
       });
  for (size_t i = std::max(start, s.storedErrors); i < std::min(end, errors); ++i)
    page.errors.push_back(s.recentErrors[i - s.storedErrors]);
  const size_t storedStart = errors + s.important.size();
  size_t i = errors;
  for (const auto &entry : s.important) {
    if (i >= start && i < end)
      page.outcomes.push_back(entry.second);
    ++i;
  }
  if (end > storedStart)
    read("SELECT src,dst,status,detail FROM entries WHERE id BETWEEN ? AND ? ORDER BY id",
         std::max(start, storedStart) - storedStart, end - storedStart,
         [&](sqlite3_stmt *row) { page.outcomes.push_back(ReadOutcome(row)); });
  return page;
}
} // namespace Listi
