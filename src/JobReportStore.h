// SPDX-License-Identifier: MIT
#pragma once

#include "FileOperation.h"
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace Listi {
std::string OutcomeLabel(const std::string &status);

struct ReportPage {
  size_t total = 0;
  bool incomplete = false;
  std::vector<std::string> errors;
  std::vector<EntryOutcome> outcomes;
};

// One private, temporary, file-backed database per queued job. A worker
// writes it, then hands it to the report view; it isn't a recovery journal.
class JobReportStore {
public:
  static constexpr size_t kPageSize = 50;

  JobReportStore();
  ~JobReportStore();
  JobReportStore(const JobReportStore &) = delete;
  JobReportStore &operator=(const JobReportStore &) = delete;

  EntryOutcome FindOutcome(const std::string &source);
  void WriteOutcome(const EntryOutcome &entry, bool update);
  void AddError(const std::string &message);
  void Seal();
  bool Incomplete() const;
  size_t OutcomeCount() const;
  size_t ErrorCount() const;
  ReportPage Page(size_t start, size_t limit) const;

private:
  struct State;
  std::unique_ptr<State> state;
};
} // namespace Listi
