// SPDX-License-Identifier: MIT
#pragma once
#include <View.h>
#include "PaneModel.h"
class Pane;
class PaneModel;
struct Preferences;
class FileDetails : public BView {
public:
  using BView::BView;
  virtual PaneSort Sorting() = 0;
  virtual void Sort(PaneSort) = 0;
  virtual void SetActive(bool) = 0;
  virtual bool CancelSelectionGesture() = 0;
  virtual void Apply(const Preferences &) = 0;
  virtual void SetAttributes(const std::vector<std::string> &) = 0;
  virtual void Append(size_t first) = 0;
  virtual void Refresh() = 0;
  virtual void SyncSelection() = 0;
  virtual void Redraw() = 0;
  virtual void RedrawEntry(const std::string &) = 0;
};
FileDetails *CreateFileDetails(Pane *, PaneModel *, const Preferences &);
