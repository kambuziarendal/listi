// SPDX-License-Identifier: MIT
#pragma once
#include <View.h>
#include <string>
struct PaneEntry;
class Pane;
class PaneModel;
class FileGrid : public BView {
public:
  virtual PaneEntry *ScrollRow(const std::string *path, float &top) = 0;
  using BView::BView;
  virtual void SetActive(bool) = 0;
  virtual bool CancelSelectionGesture() = 0;
  virtual void Reset() = 0;
  virtual void SetMode(int) = 0;
  virtual void Update() = 0;
  virtual void RedrawEntry(const std::string &) = 0;
  virtual float ContentHeight(float width) const = 0;
  virtual void SyncSelection() = 0;
};
FileGrid *CreateFileGrid(Pane *, PaneModel *);
