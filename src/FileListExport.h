// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <vector>
class PaneModel;
struct PaneEntry;
namespace Listi {
struct ExportRow {
  std::string csv;
  bool selected;
};
inline constexpr char kExportHeader[] = "Name,Full path,Type,Size (bytes),Modified (UTC)\r\n";
std::vector<ExportRow> FreezeFileList(const PaneModel &, const std::vector<PaneEntry *> &order);
}
