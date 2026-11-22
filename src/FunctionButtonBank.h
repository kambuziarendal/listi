// SPDX-License-Identifier: MIT
#pragma once

#include <GroupView.h>
#include <vector>

class BGridView;
class MainWindowMenu;

namespace Listi {

class FunctionButtonBank : public BGroupView {
public:
  explicit FunctionButtonBank(MainWindowMenu &menu);

  void SetActions(const std::vector<int32> &actions, bool compact, bool wrap = false);
  bool IsCompact() const { return fCompact; }

private:
  MainWindowMenu &fMenu;
  BGridView *fGrid = nullptr;
  std::vector<int32> fActions;
  bool fCompact = false, fWrap = false;
};

}
