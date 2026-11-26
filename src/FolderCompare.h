// SPDX-License-Identifier: MIT
#pragma once
#include "VirtualFile.h"
#include <string>
#include <vector>
namespace Listi {
enum class CompareState {
  OnlyLeft,
  OnlyRight,
  TypeDifferent,
  SizeDifferent,
  ContentDifferent,
  SizeMatch,
  Verified,
  DirectoryMatch,
  LinkMatch,
  Unverified,
  Error
};
struct CompareEntry {
  std::string relative, left, right, leftHash, rightHash, detail;
  CompareState state = CompareState::Error;
  int64_t leftBytes = -1, rightBytes = -1;
};
struct CompareRequest {
  std::string left, right;
  bool checksum = false;
  size_t entryLimit = 100000;
};
struct CompareResult {
  std::vector<CompareEntry> entries;
  bool complete = false, cancelled = false, checksum = false;
  size_t different = 0, verified = 0, sizeOnly = 0, unverified = 0;
  std::string error;
};
struct CompareRows {
  std::vector<size_t> indices;
  size_t total = 0;
};
CompareRows SelectCompareRows(const CompareResult &, bool showMatches,
                              size_t limit);
CompareResult CompareFolders(const CompareRequest &,
                             const TransferControl & = {});
const char *CompareLabel(CompareState);
bool CompareMatch(CompareState);
} // namespace Listi
class BWindow;
BWindow *OpenFolderCompare(const std::string &left, const std::string &right);
bool FolderCompareBusy();
