// SPDX-License-Identifier: MIT
#include "FileProperties.h"
#include "Pane.h"
#include "FileListExportWindow.h"
#include <Entry.h>
#include <fcntl.h>
#include <RadioButton.h>
#include "PropertiesWindow.h"
#include <Application.h>
#include <Alert.h>
#include <fs_index.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Node.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace {
constexpr const char *kLabel = "Listi:ui-label", *kNumber = "Listi:ui-number";

void Check(bool condition, const char *name) {
  if (!condition)
    throw std::runtime_error(name);
  std::cout << "PASS " << name << std::endl;
}

template <class F> bool Await(F condition) {
  const auto deadline = system_time() + 5000000;
  do {
    if (condition())
      return true;
    snooze(10000);
  } while (system_time() < deadline);
  return false;
}

struct Fixture {
  std::string root, first, second, third;
  std::vector<std::string> extra;
  Fixture() {
    char pattern[] = "/boot/home/listi-column-ui-XXXXXX";
    if (!mkdtemp(pattern))
      throw std::runtime_error("mkdtemp failed");
    root = pattern;
    first = root + "/first";
    second = root + "/second";
    third = root + "/third";
  }
  ~Fixture() {
    for (const auto &path : extra)
      unlink(path.c_str());
    unlink(first.c_str());
    unlink(second.c_str());
    unlink(third.c_str());
    rmdir(root.c_str());
  }
  void Extra(const char *name, type_code type, const char *value) {
    extra.push_back(root + "/" + name);
    std::ofstream(extra.back()) << "sort fixture";
    Listi::PropertySession file(extra.back());
    file.Save(nullptr, kNumber, type, value);
  }
};

struct WindowLock {
  BWindow *window;
  explicit WindowLock(BWindow *owner) : window(owner) {
    if (!window->Lock())
      throw std::runtime_error("could not lock test window");
  }
  ~WindowLock() { window->Unlock(); }
};

class Windows {
  std::vector<BMessenger> windows;

public:
  void Add(BWindow *window) { windows.emplace_back(window); }
  ~Windows() {
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
      if (!it->LockTarget())
        continue;
      BLooper *looper = nullptr;
      it->Target(&looper);
      looper->Quit();
    }
  }
};

BButton *FindButton(BView *view, const char *label) {
  if (auto button = dynamic_cast<BButton *>(view))
    if (BString(button->Label()) == label)
      return button;
  for (int32 i = 0; i < view->CountChildren(); ++i)
    if (auto result = FindButton(view->ChildAt(i), label))
      return result;
  return nullptr;
}

bool Click(BWindow &window, const char *label) {
  for (int32 i = 0; i < window.CountChildren(); ++i)
    if (auto button = FindButton(window.ChildAt(i), label))
      return button->IsEnabled() && button->Invoke() == B_OK;
  return false;
}

BColumnListView &List(BView &scope, const char *name) {
  auto list = dynamic_cast<BColumnListView *>(scope.FindView(name));
  if (!list)
    throw std::runtime_error("missing native list");
  return *list;
}

std::string Cell(BRow &row, int32 column) {
  auto field = dynamic_cast<BStringField *>(row.GetField(column));
  return field ? field->String() : "";
}

void SelectAttribute(BColumnListView &list, const char *name) {
  for (int32 i = 0; i < list.CountRows(); ++i) {
    auto row = list.RowAt(i);
    if (Cell(*row, 0) == name) {
      list.DeselectAll();
      list.AddToSelection(row);
      return;
    }
  }
  throw std::runtime_error("attribute not found in Properties");
}

std::set<std::pair<std::string, std::string>> Values(BColumnListView &list) {
  std::set<std::pair<std::string, std::string>> values;
  for (int32 i = 0; i < list.CountRows(); ++i) {
    auto row = list.RowAt(i);
    const auto label = Cell(*row, 3);
    if (!label.empty())
      values.emplace(label, Cell(*row, 4));
  }
  return values;
}

bool ContainsStatus(BWindow &window, const char *text) {
  auto status = dynamic_cast<BStringView *>(window.FindView("property status"));
  return status && BString(status->Text()).FindFirst(text) >= 0;
}
}

int main() {
  BApplication app("application/x-vnd.Listi-AttributeColumnsUITest");
  try {
    Fixture f;
    Windows windows;
    std::ofstream(f.first) << "first";
    std::ofstream(f.second) << "second";
    std::ofstream(f.third) << "third";
    Check(chmod(f.first.c_str(), 0644) == 0, "set known starting permissions");
    Listi::PropertySession first(f.first), second(f.second);
    first.Save(nullptr, kLabel, B_STRING_TYPE, "alpha");
    first.Save(nullptr, kNumber, B_INT32_TYPE, "-14");
    second.Save(nullptr, kLabel, B_STRING_TYPE, "");

    const std::vector<std::pair<std::string, time_t>> dates = {
        {f.first, -2208988800LL}, {f.second, 0}, {f.third, 946684800}};
    for (const auto &[path, date] : dates) {
      const timespec times[2] = {{date, 0}, {date, 0}};
      Check(utimensat(AT_FDCWD, path.c_str(), times, 0) == 0, "set chronology fixture");
    }
    Preferences prefs;
    prefs.folderModes[f.root] = 1; // Same remembered Icons view in both panes.
    auto left = new Pane(0, prefs);
    auto right = new Pane(1, prefs);
    auto browser = new BWindow(BRect(20, 40, 1000, 520), "Columns UI fixture",
                               B_TITLED_WINDOW, 0);
    BLayoutBuilder::Group<>(browser, B_HORIZONTAL).Add(left).Add(right);
    windows.Add(browser);
    browser->Show();
    {
      WindowLock lock(browser);
      left->Navigate(f.root.c_str());
      right->Navigate(f.root.c_str());
    }
    Check(Await([&] {
            WindowLock lock(browser);
            return !left->IsScanning() && !right->IsScanning() &&
                   left->CurrentView() == 1 && right->CurrentView() == 1 &&
                   f.root == left->Path() && f.root == right->Path();
          }),
          "both panes start in their shared remembered Icons view");

    const auto device = first.Read().stat.st_dev;
    index_info beforeIndex{};
    const bool hadIndex = fs_stat_index(device, kLabel, &beforeIndex) == 0;
    Check(hadIndex || errno == ENOENT, "read the prior volume index state");
    OpenFileProperties(f.first, BMessenger(left));
    BWindow *properties = nullptr;
    for (int32 i = 0; i < app.CountWindows(); ++i) {
      auto candidate = app.WindowAt(i);
      if (candidate && BString(candidate->Title()) == "Listi: Properties") {
        properties = candidate;
        break;
      }
    }
    Check(properties != nullptr, "actual native Properties window opened");
    windows.Add(properties);
    Check(Await([&] {
            WindowLock lock(properties);
            return ContainsStatus(*properties, "Properties loaded.");
          }),
          "Properties read both stored synthetic attributes");
    {
      WindowLock lock(properties);
      auto attributes = dynamic_cast<BColumnListView *>(
          properties->FindView("property attributes"));
      Check(attributes != nullptr, "Properties exposes its attribute list");
      SelectAttribute(*attributes, kLabel);
      auto permissions = dynamic_cast<BTextControl *>(
          properties->FindView("property permissions"));
      BMessage edit('pred'), create('prnw'), save('prsv');
      properties->MessageReceived(&edit);
      Check(permissions && permissions->IsEnabled(),
            "inspecting an unchanged attribute leaves permission editing available");
      properties->MessageReceived(&create);
      auto key = dynamic_cast<BTextControl *>(properties->FindView("attribute name"));
      auto value = dynamic_cast<BTextView *>(properties->FindView("attribute value"));
      Check(key && value, "new attribute editor is available");
      key->SetText("Listi:ui-draft");
      value->SetText("saved independently");
      permissions->SetText("0600");
      // Changing mode elsewhere must not rebase the pending permission edit.
      Check(chmod(f.first.c_str(), 0640) == 0, "simulate external permission edit");
      properties->MessageReceived(&save);
    }
    Check(Await([&] {
            WindowLock lock(properties);
            return ContainsStatus(*properties, "Attribute saved.");
          }), "two drafts allow the attribute to be saved first");
    {
      WindowLock lock(properties);
      auto permissions = dynamic_cast<BTextControl *>(
          properties->FindView("property permissions"));
      Check(permissions->IsEnabled() && std::string(permissions->Text()) == "0600",
            "attribute refresh preserves the pending permission text");
      BMessage apply('prpm');
      properties->MessageReceived(&apply);
    }
    Check(Await([&] {
            WindowLock lock(properties);
            return ContainsStatus(*properties, "Permissions changed");
          }), "permission draft retains its original expected mode");
    {
      WindowLock lock(properties);
      BMessage discard('prdc');
      properties->MessageReceived(&discard);
      SelectAttribute(*dynamic_cast<BColumnListView *>(
                          properties->FindView("property attributes")), kLabel);
      Check(Click(*properties, "Make searchable…"), "index action asks before making the volume searchable");
    }
    Check(Await([&] {
            for (int32 i = 0; i < app.CountWindows(); ++i) {
              auto alert = dynamic_cast<BAlert *>(app.WindowAt(i));
              if (!alert || alert->LockWithTimeout(100000) != B_OK) continue;
              auto cancel = alert->ButtonAt(0);
              bool ok = !alert->IsHidden() && cancel && cancel->IsDefault() &&
                        cancel->Invoke() == B_OK;
              alert->Unlock();
              if (ok) return true;
            }
            return false;
          }), "cancel the default choice in the native volume confirmation");
    {
      WindowLock lock(properties);
      index_info info;
      const bool hasIndex = fs_stat_index(device, kLabel, &info) == 0;
      Check(hasIndex == hadIndex && (hasIndex ? info.type == beforeIndex.type : errno == ENOENT),
            "cancelling the volume action leaves indexes untouched");
      Check(Click(*properties, "Show in Details"),
            "selected string attribute is sent to the originating pane");
    }
    Check(
        Await([&] {
          WindowLock lock(browser);
          auto &list = List(*left, "files");
          return !left->IsScanning() && left->CurrentView() == 0 &&
                 left->PaneOverride() == -1 && right->CurrentView() == 1 &&
                 List(*right, "files").CountColumns() == 3 &&
                 list.CountColumns() == 4 &&
                 Values(list) ==
                     std::set<std::pair<std::string, std::string>>{
                         {"alpha", ""}, {"(empty)", ""}, {"(missing)", ""}};
        }),
        "Details shows stored, empty and absent values without changing the other pane");

    {
      WindowLock lock(properties);
      Check(Click(*properties, "Edit selected"),
            "editable attribute can still be opened");
    }
    Check(
        Await([&] {
          WindowLock lock(properties);
          auto editor = dynamic_cast<BTextView *>(
              properties->FindView("attribute value"));
          if (!editor || !editor->IsEditable() ||
              BString(editor->Text()) != "alpha")
            return false;
          editor->SetText("edited");
          auto attributes = static_cast<BColumnListView *>(
              properties->FindView("property attributes"));
          SelectAttribute(*attributes, kNumber);
          return Click(*properties, "Show in Details");
        }),
        "a second column can be requested without discarding an open edit draft");
    Check(
        Await([&] {
          WindowLock lock(browser);
          auto &list = List(*left, "files");
          if (left->IsScanning() || list.CountColumns() != 5)
            return false;
          return Values(list) == std::set<std::pair<std::string, std::string>>{
                                     {"alpha", "-14"},
                                     {"(empty)", "(missing)"},
                                     {"(missing)", "(missing)"}};
        }),
        "both columns show the matching typed attributes for each native row");
    second.Save(nullptr, kNumber, B_INT32_TYPE, "2");
    Listi::PropertySession(f.third).Save(nullptr, kNumber, B_INT32_TYPE, "10");
    f.Extra("string-negative", B_STRING_TYPE, "-14");
    f.Extra("string-ten", B_STRING_TYPE, "10");
    f.Extra("string-two", B_STRING_TYPE, "2");
    f.Extra("wide-first", B_UINT64_TYPE, "18446744073709551615");
    f.Extra("wide-second", B_UINT64_TYPE, "18446744073709551614");
    {
      WindowLock lock(browser);
      left->Reload();
    }
    Check(
        Await([&] {
          WindowLock lock(browser);
          auto &list = List(*left, "files");
          return !left->IsScanning() && list.CountRows() == 9;
        }),
        "refreshed Details contains both numeric and numeric-looking string cells");
    {
      WindowLock lock(browser);
      auto &list = List(*left, "files");
      auto attribute = list.ColumnAt(4);
      list.SetSortColumn(attribute, false, true);
      BMessage state;
      list.SaveState(&state);
      Check(state.GetInt32("sortID", -1) == 4,
            "the native Details list is sorted by an attribute column");
    }
    Check(
        Await([&] {
          WindowLock lock(browser);
          auto &list = List(*left, "files");
          std::vector<std::string> ordered;
          for (int32 i = 0; i < list.CountRows(); ++i) {
            auto cell = Cell(*list.RowAt(i), 4);
            if (!cell.empty())
              ordered.push_back(std::move(cell));
          }
          return ordered == std::vector<std::string>{"-14",
                                                     "2",
                                                     "10",
                                                     "18446744073709551614",
                                                     "18446744073709551615",
                                                     "-14",
                                                     "10",
                                                     "2"};
        }),
        "numbers sort by value including exact uint64, while strings sort as text");
    for (bool selectedOnly : {false, true}) {
      std::vector<std::string> names;
      {
        WindowLock lock(browser);
        auto &list = List(*left, "files");
        list.DeselectAll();
        for (int32 i = 0; i < list.CountRows(); ++i) {
          const std::string name = Cell(*list.RowAt(i), 0);
          if (name == "..") continue;
          if (i % 2) list.AddToSelection(list.RowAt(i));
          if (!selectedOnly || i % 2) names.push_back(name);
        }
        // Haiku's programmatic AddToSelection changes the outline only. A
        // user gesture also notifies the pane, which owns the export selection.
        list.SelectionChanged();
        if (selectedOnly)
          Check(left->Selected().size() == names.size(), "pane received selected rows");
        Listi::ShowFileListExport(*left);
      }
      BWindow *exporter = nullptr;
      for (int32 i = 0; i < app.CountWindows(); ++i)
        if (std::string(app.WindowAt(i)->Title()) == "Export file list")
          exporter = app.WindowAt(i);
      Check(exporter != nullptr, "export dialog opened");
      const std::string leaf = selectedOnly ? "listi-selected.csv" : "listi-visible.csv";
      // Save outside the watched fixture so the first export cannot change the second.
      const std::string output = f.root + "-" + leaf;
      f.extra.push_back(output);
      {
        WindowLock lock(exporter);
        auto radio = static_cast<BRadioButton *>(exporter->FindView(selectedOnly ? "selected" : "all"));
        radio->SetValue(B_CONTROL_ON);
        BMessage refresh('scop');
        exporter->MessageReceived(&refresh);
        auto preview = static_cast<BTextView *>(exporter->FindView("preview"));
        std::string sample = preview->Text();
        size_t at = 0;
        for (size_t i = 0; i < std::min(size_t(3), names.size()); ++i) {
          at = sample.find("\"" + names[i] + "\",", at);
          Check(at != std::string::npos, "preview follows panel order");
          ++at;
        }
        entry_ref folder;
        Check(get_ref_for_path("/boot/home", &folder) == B_OK, "CSV output folder");
        BMessage save(B_SAVE_REQUESTED);
        save.AddRef("directory", &folder);
        save.AddString("name", output.substr(output.rfind('/') + 1).c_str());
        exporter->MessageReceived(&save);
      }
      std::ifstream saved(output);
      std::string line;
      Check(bool(std::getline(saved, line)), "CSV header saved");
      for (const auto &name : names)
        Check(std::getline(saved, line) && line.find("\"" + name + "\",") == 0,
              "saved CSV follows panel order");
      Check(!std::getline(saved, line), "no extra CSV rows");
    }
    for (bool ascending : {true, false}) {
      {
        WindowLock lock(browser);
        left->Sort({2, ascending});
      }
      Check(Await([&] {
        WindowLock lock(browser);
        std::vector<std::string> paths;
        for (const auto *entry : left->DisplayedRows())
          if (entry->path == f.first || entry->path == f.second || entry->path == f.third)
            paths.push_back(entry->path);
        const std::vector<std::string> expected = ascending
            ? std::vector<std::string>{f.first, f.second, f.third}
            : std::vector<std::string>{f.third, f.second, f.first};
        return paths == expected;
      }), "Details sorts 1900, 1970 and 2000 chronologically");
    }
    {
      WindowLock lock(browser);
      auto display = dynamic_cast<BMenuField *>(left->FindView("display mode"));
      auto clear = display
                       ? display->Menu()->FindItem("Clear attribute columns")
                       : nullptr;
      Check(clear && static_cast<BInvoker *>(clear)->Invoke() == B_OK,
            "clear via the actual pane menu item");
    }
    Check(
        Await([&] {
          WindowLock lock(browser);
          auto &list = List(*left, "files");
          BMessage state;
          list.SaveState(&state);
          return !left->IsScanning() && left->CurrentView() == 1 &&
                 right->CurrentView() == 1 && list.CountColumns() == 3 &&
                 state.GetInt32("sortID", -1) == 0 &&
                 List(*right, "files").CountColumns() == 3;
        }),
        "clearing sorted columns restores the remembered view and a valid native sort");

    BNode node(f.first.c_str());
    Check(Listi::AttributeCell(node, kLabel) == "alpha",
          "showing, sorting and clearing columns did not write the draft");
    {
      WindowLock lock(properties);
      auto editor =
          dynamic_cast<BTextView *>(properties->FindView("attribute value"));
      Check(editor && editor->IsEditable() &&
                BString(editor->Text()) == "edited",
            "editable Properties draft remains intact");
      Check(Click(*properties, "Save attribute"),
            "draft saves through Properties");
    }
    Check(Await([&] {
            WindowLock lock(properties);
            return ContainsStatus(*properties, "Properties loaded.") &&
                   Listi::AttributeCell(node, kLabel) == "edited" &&
                   Listi::AttributeCell(node, kNumber) == "-14";
          }),
          "saved draft edits only its original attribute");
    {
      WindowLock lock(properties);
      auto *list = dynamic_cast<BColumnListView *>(
          properties->FindView("property attributes"));
      SelectAttribute(*list, kLabel);
      list->SetFocusRow(list->CurrentSelection(), true);
      Check(chmod(f.first.c_str(), 0064) == 0, "set a mode with no owner bits");
      BMessage refresh('prrf');
      properties->MessageReceived(&refresh);
    }
    Check(Await([&] {
            WindowLock lock(properties);
            if (!ContainsStatus(*properties, "Properties loaded."))
              return false;
            auto *list = dynamic_cast<BColumnListView *>(
                properties->FindView("property attributes"));
            auto *selection = list->CurrentSelection();
            return selection && list->FocusRow() == selection &&
                   Cell(*selection, 0) == kLabel;
          }), "refresh keeps the selected attribute and its live keyboard row");
    {
      WindowLock lock(properties);
      auto *mode = dynamic_cast<BTextControl *>(
          properties->FindView("property permissions"));
      Check(mode && std::string(mode->Text()) == "064",
            "mode with no owner bits is displayed as three octal digits");
      BMessage apply('prpm');
      properties->MessageReceived(&apply);
    }
    Check(Await([&] {
            WindowLock lock(properties);
            return ContainsStatus(*properties, "Properties loaded.");
          }), "Apply accepts the unchanged permission text");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    return 1;
  }
}
