// SPDX-License-Identifier: MIT
#include "WorkingSets.h"
#include "ApplicationServices.h"
#include "MessageStore.h"
#include "WindowOverview.h"
#include <Alert.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <ScrollView.h>
#include <StringItem.h>
#include <TextControl.h>
#include <mutex>
#include <stdexcept>

namespace Listi {
namespace {
constexpr uint32 kAccept = 'wkac';
std::mutex storeLock;

BMessage ReadSets() {
  auto store = LoadMessageStore(SettingsPath("Listi_working_sets").String(), true);
  if (store.what == 'none')
    return BMessage('wks1');
  if (store.what != 'wks1')
    throw std::runtime_error("Unknown working set format.");
  return store;
}

class SetDialog : public BWindow {
  bool saving;
  BrowserOpener open;
  std::vector<BrowserChoice> choices;
  BListView *list;
  BTextControl *name = nullptr;
  BMessage saved;

  void Save() {
    std::string label(name->Text());
    auto first = label.find_first_not_of(" \t");
    if (first == std::string::npos)
      throw std::runtime_error("Enter a working set name.");
    label = label.substr(first, label.find_last_not_of(" \t") - first + 1);
    BMessage set;
    set.AddString("name", label.c_str());
    BMessage capture(kCaptureWorkingWindow);
    for (int32 n = 0; ; ++n) {
      int32 i = list->CurrentSelection(n);
      if (i < 0)
        break;
      BMessage snapshot;
      if (choices.at(i).target.SendMessage(&capture, &snapshot, 250000, 1000000) != B_OK ||
          snapshot.what != kCaptureWorkingWindow)
        throw std::runtime_error("A selected window closed or is busy. No set was saved.");
      set.AddMessage("window", &snapshot);
    }
    if (!set.HasMessage("window"))
      throw std::runtime_error("Select at least one file manager window.");
    std::lock_guard<std::mutex> guard(storeLock);
    BMessage store = ReadSets(), existing;
    for (int32 i = 0; store.FindMessage("set", i, &existing) == B_OK; ++i)
      if (label == existing.GetString("name", ""))
        throw std::runtime_error("A set with this name already exists. Choose another name.");
    store.AddMessage("set", &set);
    SaveMessageStore(SettingsPath("Listi_working_sets").String(), store);
  }
  void Open() {
    BMessage set, window;
    if (list->CurrentSelection() < 0 ||
        saved.FindMessage("set", list->CurrentSelection(), &set) != B_OK)
      throw std::runtime_error("Select a saved working set.");
    for (int32 i = 0; set.FindMessage("window", i, &window) == B_OK; ++i) {
      BrowserRequest request;
      window.AddString("workingSetName", set.GetString("name", ""));
      request.workingSet = window;
      if (!open(std::move(request)))
        throw std::runtime_error("Could not queue the next window.");
    }
  }

public:
  SetDialog(WorkingWindow &owner, bool save, BrowserOpener opener, BMessage store)
      : BWindow(BRect(0, 0, 530, 320), save ? "Save working set" : "Open working set",
                B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS), saving(save),
        open(std::move(opener)), saved(std::move(store)) {
    list = new BListView("working set choices",
                         save ? B_MULTIPLE_SELECTION_LIST : B_SINGLE_SELECTION_LIST);
    if (save) {
      choices = WorkingSetChoices();
      for (size_t i = 0; i < choices.size(); ++i) {
        list->AddItem(new BStringItem(choices[i].label.c_str()));
        if (choices[i].id == owner.WindowId())
          list->Select(i);
      }
    } else {
      BMessage set;
      for (int32 i = 0; saved.FindMessage("set", i, &set) == B_OK; ++i)
        list->AddItem(new BStringItem(set.GetString("name", "")));
      if (list->CountItems())
        list->Select(0);
    }
    auto *scroll = new BScrollView("choices", list, 0, false, true);
    scroll->SetExplicitMinSize(BSize(320, 110));
    auto group = BLayoutBuilder::Group<>(this, B_VERTICAL);
    group.SetInsets(B_USE_WINDOW_SPACING);
    if (save) {
      name = new BTextControl("set name", "Working set name:", "", nullptr);
      name->TextView()->SetMaxBytes(120);
      group.Add(name);
    }
    auto *accept = new BButton(save ? "Save selected windows" : "Open windows", new BMessage(kAccept));
    accept->SetEnabled(list->CountItems() > 0);
    group.Add(scroll).AddGroup(B_HORIZONTAL).AddGlue()
      .Add(new BButton("Cancel", new BMessage(B_QUIT_REQUESTED))).Add(accept).End();
    AddShortcut(B_ESCAPE, B_NO_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
    CenterIn(owner.Frame());
    MoveOnScreen();
  }
  void MessageReceived(BMessage *m) override {
    if (m->what != kAccept) {
      BWindow::MessageReceived(m);
      return;
    }
    try {
      if (saving)
        Save();
      else
        Open();
      PostMessage(B_QUIT_REQUESTED);
    } catch (const std::exception &error) {
      (new BAlert("Working set", error.what(), "Close"))->Go(nullptr);
    }
  }
};
}

void ShowWorkingSets(WorkingWindow &owner, bool saving, BrowserOpener open) {
  try {
    BMessage store = ReadSets();
    if (!saving && !store.HasMessage("set"))
      throw std::runtime_error("No working sets have been saved yet.");
    (new SetDialog(owner, saving, std::move(open), std::move(store)))->Show();
  } catch (const std::exception &error) {
    (new BAlert("Working set", error.what(), "Close"))->Go(nullptr);
  }
}
}
