// SPDX-License-Identifier: MIT
#pragma once

#include <View.h>
#include <memory>
#include <string>

struct PaneEntry;
class Pane;
class PaneModel;

class MillerColumns : public BView {
public:
  MillerColumns(Pane *, PaneModel *);
  ~MillerColumns() override;
  void AttachedToWindow() override;
  void FrameResized(float, float) override;
  void MakeFocus(bool = true) override;
  void MessageReceived(BMessage *) override;

  PaneEntry *ScrollRow(const std::string *path, float &top);
  float ContentWidth(float viewport) const;
  BPoint ScrollPosition() const;
  void RestoreScroll(BPoint);
  void SetLocation(const std::string &);
  void Refresh();
  void RedrawRows();
  void SyncSelection();
  void SetActive(bool);
  void SetEnabled(bool);
  void ApplyFont(const BFont &);

private:
  struct State;
  std::unique_ptr<State> state;
  void LayoutColumns();
};
