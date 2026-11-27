// SPDX-License-Identifier: MIT
#include "WorkingSets.h"
#include "ApplicationServices.h"
#include "MessageStore.h"
#include "StoredFields.h"
#include "WindowOverview.h"
#include <Alert.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <ScrollView.h>
#include <StringItem.h>
#include <TextControl.h>
#include <TextView.h>
#include <mutex>
#include <stdexcept>

namespace Listi {
namespace {
constexpr uint32 kAccept = 'wkac', kPreviewSet = 'wkpr';
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

  BTextView *preview = nullptr;
  void Preview() {
    BMessage set, window;
    std::string folders;
    int32 count = 0;
    if (list->CurrentSelection() >= 0 &&
        saved.FindMessage("set", list->CurrentSelection(), &set) == B_OK) {
      for (; set.FindMessage("window", count, &window) == B_OK; ++count) {
        folders += "\n" + std::to_string(count + 1) + ".";
        BMessage panel, tab;
        for (int side = 0; window.FindMessage("panel", side, &panel) == B_OK; ++side) {
          if (panel.FindMessage("tab", panel.GetInt32("active", 0), &tab) == B_OK) {
            folders += side == 0 ? " " : "  |  ";
            folders += tab.HasMessage("query") ? tab.GetString("label", "Search")
                                            : tab.GetString("location", "Unknown folder");
          }
        }
      }
    }
    const auto summary = count == 0 ? std::string("Select a saved working set.") :
        "Opens " + std::to_string(count) + (count == 1 ? " new window." : " new windows.") +
        " Your current windows stay open." + folders;
    preview->SetText(summary.c_str());
    preview->ScrollTo(0, 0);
  }
  void Save() {
    std::string label(name->Text());
    auto first = label.find_first_not_of(" \t");
    if (first == std::string::npos)
      throw std::runtime_error("Enter a working set name.");
    label = label.substr(first, label.find_last_not_of(" \t") - first + 1);
    std::vector<BMessenger> selected;
    for (int32 n = 0; ; ++n) {
      const int32 i = list->CurrentSelection(n);
      if (i < 0)
        break;
      selected.push_back(choices.at(i).target);
    }
    const auto set = CaptureWorkingSet(label, selected);
    std::lock_guard<std::mutex> guard(storeLock);
    BMessage store = ReadSets();
    const auto count = StoredCount(store, "set");
    for (int32 i = 0; i < count; ++i)
      if (label == StoredItem(store, "set", i).GetString("name", ""))
        throw std::runtime_error("A set with this name already exists. Choose another name.");
    CheckStoredField(store.AddMessage("set", &set));
    SaveMessageStore(SettingsPath("Listi_working_sets").String(), store);
  }
  void Open() {
    BMessage set;
    if (list->CurrentSelection() < 0 ||
        saved.FindMessage("set", list->CurrentSelection(), &set) != B_OK)
      throw std::runtime_error("Select a saved working set.");
    QueueWorkingSet(set, open);
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
      const auto count = StoredCount(saved, "set");
      for (int32 i = 0; i < count; ++i)
        list->AddItem(new BStringItem(StoredItem(saved, "set", i).GetString("name", "")));
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
    group.Add(scroll);
    if (!save) {
      preview = new BTextView("working set preview");
      preview->MakeEditable(false);
      auto *details = new BScrollView("saved panel folders", preview, 0, false, true);
      details->SetExplicitMinSize(BSize(320, 70));
      group.Add(details);
      list->SetSelectionMessage(new BMessage(kPreviewSet));
      Preview();
    }
    group.AddGroup(B_HORIZONTAL).AddGlue()
      .Add(new BButton("Cancel", new BMessage(B_QUIT_REQUESTED))).Add(accept).End();
    AddShortcut(B_ESCAPE, B_NO_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
    CenterIn(owner.Frame());
    MoveOnScreen();
  }
  void MessageReceived(BMessage *m) override {
    if (m->what == kPreviewSet && preview) {
      Preview();
      return;
    }
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

BMessage CaptureWorkingSet(const std::string &label, const std::vector<BMessenger> &selected) {
  if (selected.empty())
    throw std::runtime_error("Select at least one file manager window.");
  BMessage set;
  CheckStoredField(set.AddString("name", label.c_str()));
  BMessage capture(kCaptureWorkingWindow);
  for (const auto &target : selected) {
    BMessage snapshot;
    if (target.SendMessage(&capture, &snapshot, 250000, 1000000) != B_OK ||
        snapshot.what != kCaptureWorkingWindow)
      throw std::runtime_error("Could not capture a complete selected window. No set was saved.");
    CheckStoredField(set.AddMessage("window", &snapshot));
  }
  return set;
}

void QueueWorkingSet(const BMessage &set, const BrowserOpener &open) {
  const auto count = StoredCount(set, "window");
  if (!count)
    throw std::runtime_error("There are no windows in this saved set.");
  for (int32 i = 0; i < count; ++i) {
    try {
      BrowserRequest request;
      auto &snapshot = request.workingSet.emplace();
      if (set.FindMessage("window", i, &snapshot) != B_OK)
        throw std::runtime_error("Unreadable saved window.");
      CheckStoredField(snapshot.AddString("workingSetName", set.GetString("name", "")));
      CheckStoredField(snapshot.AddInt32("setIndex", i + 1));
      if (!open(std::move(request)))
        throw std::runtime_error("Could not queue the window.");
    } catch (const std::exception &error) {
      const auto queued = i == 0 ? std::string("No windows were queued.") :
          "The first " + std::to_string(i) + " of " + std::to_string(count) +
          " windows were already queued.";
      throw std::runtime_error(queued + " Window " + std::to_string(i + 1) + ": " + error.what());
    }
  }
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
