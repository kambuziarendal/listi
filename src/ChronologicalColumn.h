// SPDX-License-Identifier: MIT
#pragma once
#include <ColumnTypes.h>

// BDateColumn narrows the subtraction of two time_t values to int.
class ChronologicalColumn : public BDateColumn {
public:
  using BDateColumn::BDateColumn;
  int CompareFields(BField *left, BField *right) override {
    const auto a = static_cast<BDateField *>(left)->Seconds();
    const auto b = static_cast<BDateField *>(right)->Seconds();
    return (a > b) - (a < b);
  }
};
