// SPDX-License-Identifier: MIT
#include "InlineAction.h"
#include "NamedEntry.h"
#include "AdaptiveScrollView.h"
#include <StringView.h>
#include "ConnectionView.h"
#include "ExplicitSubmit.h"
#include <Button.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <TextControl.h>
#include <TextView.h>
#include "ThemeTextView.h"
#include <string>
#include <utility>
namespace {
constexpr uint32 kAccept = 'iacc', kAlternate = 'ialt', kCancel = 'iccl',
                 kClosed = 'ican', kThird = 'i3rd';
}
InlineAction::InlineAction(BMessenger receiver, std::function<void()> beforeOpen)
    : BGroupView(B_VERTICAL, 5), target(receiver),
      beforeOpen(std::move(beforeOpen)) {}
BMessage InlineAction::Event(uint32 what) const {
  BMessage message(what);
  message.AddInt64("question", serial);
  return message;
}
void InlineAction::Close() {
  if (!pending)
    return;
  pending = false;
  ++serial;
  Hide();
  name = nullptr;
  nameHint = nullptr;
  options = nullptr;
  result.MakeEmpty();
  // Destroy connection password controls immediately, not on the next prompt.
  while (auto child = ChildAt(0)) {
    RemoveChild(child);
    delete child;
  }
}
void InlineAction::FocusQuestion() {
  if (!pending)
    return;
  if (auto *address = FindView("network address"))
    address->MakeFocus(true);
  else if (name)
    name->MakeFocus(true);
  else if (auto *cancel = FindView("Cancel"))
    cancel->MakeFocus(true);
}
bool InlineAction::Ask(const char *text, const char *button,
                       const BMessage &accepted, const char *initial,
                       const char *alternative, const char *third,
                       const std::vector<std::string> &choices) {
  if (pending) {
    FocusQuestion();
    return false;
  }
  if (beforeOpen)
    beforeOpen();
  ++serial;
  result = accepted;
  auto explanation = new ThemeTextView("question", B_PANEL_BACKGROUND_COLOR,
                                       B_PANEL_TEXT_COLOR);
  explanation->SetText(text);
  explanation->SetToolTip(text);
  explanation->MakeEditable(false);
  explanation->MakeSelectable(false);
  explanation->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
  explanation->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
  explanation->SetHighUIColor(B_PANEL_TEXT_COLOR);
  explanation->SetExplicitMinSize(BSize(300, 35));
  explanation->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 75));
  if (accepted.HasString("preview") && accepted.HasMessage("restoreTarget")) {
    auto *scroll = new Listi::AdaptiveScrollView("restore destinations", explanation, false);
    scroll->SetExplicitMinSize(BSize(300, 58));
    scroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 100));
    AddChild(scroll);
  } else
    AddChild(explanation);
  if (initial) {
    name = new BTextControl("Name:", initial, nullptr);
    AddChild(name);
    name->SetTarget(this);
    name->TextView()->AddFilter(
        new Listi::ExplicitSubmit(BMessenger(this), Event(kAccept)));
  }
  if (name && result.HasString("newEntryIn")) {
    nameHint = new BStringView(
        "name hint",
        "Choose an unused name. Existing entries are never replaced.");
    nameHint->SetExplicitMinSize(BSize(200, B_SIZE_UNSET));
    nameHint->SetTruncation(B_TRUNCATE_END);
    AddChild(nameHint);
  }
  if (!choices.empty()) {
    options = new BPopUpMenu("Output formats");
    for (const auto &choice : choices)
      options->AddItem(new BMenuItem(choice.c_str(), nullptr));
    options->ItemAt(0)->SetMarked(true);
    AddChild(new BMenuField("output format", "Format:", options));
  }
  auto cancel = new BButton("Cancel", new BMessage(Event(kCancel)));
  auto accept = new BButton(button, new BMessage(Event(kAccept)));
  auto buttons = new BGroupView(B_HORIZONTAL, 8);
  BLayoutBuilder::Group<>(buttons, B_HORIZONTAL, 8)
      .AddGlue()
      .Add(cancel)
      .Add(accept);
  if (alternative) {
    auto alternate = new BButton(alternative, new BMessage(Event(kAlternate)));
    buttons->AddChild(alternate);
    alternate->SetTarget(this);
  }
  if (third) {
    auto extra = new BButton(third, new BMessage(Event(kThird)));
    buttons->AddChild(extra);
    extra->SetTarget(this);
  }
  AddChild(buttons);
  cancel->SetTarget(this);
  accept->SetTarget(this);
  pending = true;
  Show();
  if (name) {
    name->MakeFocus(true);
    name->TextView()->SelectAll();
  } else {
    cancel->MakeFocus(true);
  }
  return true;
}
bool InlineAction::Connect(BMessenger pane) {
  if (pending) {
    FocusQuestion();
    return false;
  }
  if (beforeOpen)
    beforeOpen();
  ++serial;
  AddChild(CreateConnectionView(pane, BMessenger(this), Event(kCancel)));
  pending = true;
  Show();
  return true;
}
void InlineAction::MessageReceived(BMessage *message) {
  if (message->what != kAccept && message->what != kAlternate &&
      message->what != kThird && message->what != kCancel) {
    BGroupView::MessageReceived(message);
    return;
  }
  if (!pending || message->GetInt64("question", -1) != serial)
    return;
  BMessage accepted(result);
  if (message->what != kCancel) {
    if (name) {
      std::string value = name->Text();
      if (nameHint) {
        try {
          Listi::ValidateNewEntryName(result.GetString("newEntryIn", ""),
                                      value);
        } catch (const std::exception &e) {
          nameHint->SetText(e.what());
          nameHint->SetToolTip(e.what());
          name->MakeFocus(true);
          return;
        }
      }
      if (value.empty() || (!result.GetBool("placeLabel", false) &&
                            (value == "." || value == ".." ||
                             value.find('/') != std::string::npos))) {
        name->SetToolTip("Enter one file or folder name, without a slash");
        return;
      }
      accepted.AddString("name", value.c_str());
    }
    if (options)
      accepted.AddInt32("option", options->IndexOf(options->FindMarked()));
    accepted.AddInt32("which", message->what == kThird       ? 3
                               : message->what == kAlternate ? 2
                                                            : 1);
  }
  Close();
  // No queued close event may cancel an unrelated later question.
  if (message->what != kCancel && accepted.what != kClosed)
    target.SendMessage(&accepted);
  target.SendMessage(kInlineClosed);
}
