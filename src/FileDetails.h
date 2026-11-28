// SPDX-License-Identifier: MIT
#pragma once
#include <View.h>
#include "PaneModel.h"
struct PaneEntry;
class Pane;
class PaneModel;
struct Preferences;
class FileDetails : public BView {
public:
  virtual PaneEntry *ScrollRow(const std::string *path, float &top) = 0;
  using BView::BView;
  virtual std::vector<PaneEntry *> DisplayedRows() const = 0;
  virtual PaneSort Sorting() = 0;
  virtual void Sort(PaneSort) = 0;
  virtual void SetActive(bool) = 0;
  virtual bool CancelSelectionGesture() = 0;
  virtual void Apply(const Preferences &) = 0;
  virtual void SetAttributes(const std::vector<std::string> &) = 0;
  virtual void Append(size_t first) = 0;
  virtual void Refresh() = 0;
  virtual void SyncSelection() = 0;
  virtual BPoint ScrollPosition() = 0;
  virtual void RestoreScroll(BPoint) = 0;
  virtual void Redraw() = 0;
  virtual void RedrawEntry(const std::string &) = 0;
};
FileDetails *CreateFileDetails(Pane *, PaneModel *, const Preferences &);
