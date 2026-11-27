// SPDX-License-Identifier: MIT
#pragma once
#include <GroupView.h>
#include <Message.h>
#include <Messenger.h>
#include <functional>
#include <string>
#include <vector>
class BTextControl;
class BStringView;
class BPopUpMenu;
constexpr uint32 kInlineClosed = 'icls';
// Owns one frozen question. New requests cannot replace a pending decision.
class InlineAction : public BGroupView {
public:
  explicit InlineAction(BMessenger target, std::function<void()> beforeOpen = {});
  int64 Ticket() const { return serial; }
  bool Pending() const { return pending; }
  bool Ask(const char *text, const char *button, const BMessage &result,
           const char *initial = nullptr, const char *alternative = nullptr,
           const char *third = nullptr,
           const std::vector<std::string> &choices = {});
  void FocusQuestion();
  bool Connect(BMessenger pane);
  void Close();
  void MessageReceived(BMessage *) override;

private:
  BMessenger target;
  std::function<void()> beforeOpen;
  BMessage result;
  BTextControl *name = nullptr;
  BStringView *nameHint = nullptr;
  BPopUpMenu *options = nullptr;
  bool pending = false;
  int64 serial = 0;
  BMessage Event(uint32 what) const;
};
