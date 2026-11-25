// SPDX-License-Identifier: MIT
#include "FileListExport.h"
#include "PaneModel.h"
#include <cassert>
#include <iostream>
int main() {
  PaneModel model;
  std::vector<std::string> selection;
  for (int i = 0; i < 22; ++i) {
    auto entry = std::make_unique<PaneEntry>();
    entry->name = i == 0 ? "=1+2" : i == 1 ? "Håkon,\"Ø\"\nÆ" : std::to_string(i);
    entry->path = "/fixture/" + entry->name;
    entry->modified = 1;
    entry->sizeKnown = false;
    if (i < 20)
      selection.push_back(entry->path);
    model.Add(std::move(entry));
  }
  model.Filter("");
  model.Sort({0, false});
  model.RestoreSelection(selection);
  const auto rows = Listi::FreezeFileList(model);
  assert(rows.size() == 22);
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto *entry = model.Visible()[i];
    std::string expected = entry->name;
    if (expected == "=1+2")
      expected = "'=1+2";
    else if (expected == "Håkon,\"Ø\"\nÆ")
      expected = "Håkon,\"\"Ø\"\"\nÆ";
    assert(rows[i].csv.find("\"" + expected + "\",") == 0);
    assert(rows[i].csv.find(",file,,1970-01-01T00:00:01Z\r\n") != std::string::npos);
  }
  model.Filter("Håkon");
  assert(Listi::FreezeFileList(model).size() == 1);
  model.Clear();
  int n = 0;
  std::cout << Listi::kExportHeader;
  for (const auto &row : rows)
    if (row.selected) {
      ++n;
      std::cout << row.csv;
    }
  assert(n == 20);
}
