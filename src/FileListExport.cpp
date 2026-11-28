// SPDX-License-Identifier: MIT
#include "FileListExport.h"
#include "PaneModel.h"
#include <ctime>

namespace {
std::string TextCell(const std::string &text) {
  std::string cell = "\"";
  const auto first = text.find_first_not_of(" \t\r\n");
  if ((!text.empty() && (text[0] == '\t' || text[0] == '\r' || text[0] == '\n')) ||
      (first != text.npos && std::string("=+-@").find(text[first]) != text.npos))
    cell += '\'';
  for (char c : text) {
    if (c == '"')
      cell += '"';
    cell += c;
  }
  return cell + '"';
}
}
namespace Listi {
std::vector<ExportRow> FreezeFileList(const PaneModel &model, const std::vector<PaneEntry *> &order) {
  std::vector<ExportRow> rows;
  for (const auto *entry : order) {
    if (entry->parent || entry->unmountedDisk)
      continue;
    const auto type = entry->kind == 2 ? "link" : entry->directory ? "folder" : "file";
    std::string size;
    if (!entry->directory && entry->sizeKnown && entry->size >= 0)
      size = std::to_string(entry->size);
    const time_t seconds = entry->modified;
    struct tm utc;
    char modified[32] = {};
    if ((entry->modifiedKnown || seconds != 0) && gmtime_r(&seconds, &utc))
      strftime(modified, sizeof(modified), "%Y-%m-%dT%H:%M:%SZ", &utc);
    rows.push_back({TextCell(entry->name) + "," + TextCell(entry->path) + "," +
                    type + "," + size + "," + modified + "\r\n", model.IsSelected(entry->path)});
  }
  return rows;
}
}
