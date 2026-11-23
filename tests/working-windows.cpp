// SPDX-License-Identifier: MIT
#include "Common.h"
#include "ImagePreview.h"
#include "MainWindow.h"
#include "SavedPlaces.h"
#include "SavedSearches.h"
#include "WindowOverview.h"
#include <Application.h>
#include <Button.h>
#include <ListView.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <RadioButton.h>
#include <StringItem.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>
#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace Listi;
namespace {
void Check(bool ok, const char *label) {
  std::cout << (ok ? "PASS " : "FAIL ") << label << std::endl;
  if (!ok)
    std::exit(EXIT_FAILURE);
}
template<class Predicate> bool Wait(Predicate test) {
  auto end = system_time() + 4000000;
  do {
    if (test())
      return true;
    snooze(15000);
  } while (system_time() < end);
  return false;
}
BMessenger ByTitle(const char *title) {
  for (int32 i = 0; i < be_app->CountWindows(); ++i) {
    auto *w = be_app->WindowAt(i);
    if (w && std::strcmp(w->Title(), title) == 0)
      return BMessenger(w);
  }
  return {};
}
template<class Operation> bool With(BMessenger target, Operation op) {
  if (!target.LockTarget())
    return false;
  BLooper *looper = nullptr;
  target.Target(&looper);
  bool result = op(*static_cast<BWindow *>(looper));
  looper->Unlock();
  return result;
}
BMessenger Dialog(const char *title) {
  BMessenger target;
  Check(Wait([&] { target = ByTitle(title); return target.IsValid(); }), title);
  return target;
}
bool HasTitle(BMessenger target, const char *title) {
  return Wait([&] { return With(target, [&](BWindow &w) { return std::string(w.Title()) == title; }); });
}
void Name(BMessenger target, const char *alias) {
  Check(target.SendMessage(kNameWindow) == B_OK, "open naming action");
  auto dialog = Dialog("Name window");
  Check(With(dialog, [&](BWindow &w) {
    auto *box = dynamic_cast<BTextControl *>(w.FindView("window name"));
    auto *save = dynamic_cast<BButton *>(w.FindView("name save"));
    if (!box || !save) return false;
    box->SetText(alias);
    return save->Invoke() == B_OK;
  }), "save name through native controls");
  Check(Wait([&] { return !dialog.IsValid(); }), "naming dialog closed");
}
std::string Property(BMessenger target, const char *name, const char *set = nullptr) {
  BMessage request(set ? B_SET_PROPERTY : B_GET_PROPERTY), reply;
  request.AddSpecifier(name);
  if (set) request.AddString("data", set);
  if (target.SendMessage(&request, &reply, 1000000, 1000000) != B_OK ||
      reply.GetInt32("error", B_ERROR) != B_OK)
    return {};
  return reply.GetString("result", "");
}
}

int main() {
  BApplication app("application/x-vnd.Listi-WindowOverviewTest");
  Preferences prefs;
  prefs.workspace = Workspace::tviskipt;
  auto *main = CreateMainWindow(prefs, {}, {}, false, {}, false);
  BMessenger file(main);
  auto *menu = dynamic_cast<BMenuBar *>(main->FindView("menu"));
  Check(menu && menu->FindItem(kNameWindow) && menu->FindItem(kUseFolderName) &&
        menu->FindItem(kOpenWindowOverview) && menu->FindItem(kShowLayoutGuide),
        "Window and Help menus expose all actions");
  main->Show();
  Name(file, "Project");
  Check(HasTitle(file, "Project · Listi"), "alias reaches native title");
  Property(file, "SourcePath", "/boot");
  Check(Wait([&] { return Property(file, "SourcePath") == "/boot"; }) &&
        HasTitle(file, "Project · Listi"), "navigation preserves window alias");
  ImagePreview::OpenWindow("/boot");
  auto preview = Dialog("/boot");
  Name(preview, "Project");
  Check(HasTitle(preview, "Project · Listi"), "another window accepts the same alias");

  file.SendMessage(kOpenWindowOverview);
  auto overview = Dialog("Open windows");
  Check(Wait([&] { return With(overview, [&](BWindow &w) {
    auto *list = dynamic_cast<BListView *>(w.FindView("open windows"));
    if (!list || list->CountItems() != 2) return false;
    auto a = std::string(static_cast<BStringItem *>(list->ItemAt(0))->Text());
    auto b = std::string(static_cast<BStringItem *>(list->ItemAt(1))->Text());
    return a.find("Project · Listi") != a.npos && b.find("Project · Listi") != b.npos &&
           a.find("/boot") != a.npos && b.find("/boot") != b.npos &&
           a.substr(0, a.find(' ')) != b.substr(0, b.find(' '));
  }); }), "same name and same location remain distinguishable by stable IDs");
  file.SendMessage(kOpenWindowOverview);
  Check(ByTitle("Open windows") == overview, "reopening reuses the overview");
  Check(With(overview, [&](BWindow &w) {
    auto *list = static_cast<BListView *>(w.FindView("open windows"));
    list->Select(1);
    auto *location = dynamic_cast<BTextView *>(w.FindView("window location"));
    return location && list->Invoke() == B_OK;
  }), "selected target invoked");
  Check(Wait([&] { return !overview.IsValid(); }), "activation dismisses overview");
  preview.SendMessage(B_QUIT_REQUESTED);
  Check(Wait([&] { return !preview.IsValid(); }), "preview closes independently");
  file.SendMessage(kOpenWindowOverview);
  overview = Dialog("Open windows");
  Check(With(overview, [&](BWindow &w) {
    auto *list = static_cast<BListView *>(w.FindView("open windows"));
    return list && list->CountItems() == 1;
  }), "closed window removed");
  overview.SendMessage(B_QUIT_REQUESTED);
  file.SendMessage(kUseFolderName);
  Check(HasTitle(file, "boot · Listi"), "automatic title restored to current folder");

  auto choose = [&](bool apply) {
    file.SendMessage(kShowLayoutGuide);
    auto guide = Dialog("Choose a Listi layout");
    Check(With(guide, [&](BWindow &w) {
      auto *radio = dynamic_cast<BRadioButton *>(w.FindView("Einn"));
      if (!radio) return false;
      radio->SetValue(B_CONTROL_ON);
      return radio->Invoke() == B_OK;
    }), "select Einn in guide");
    Check(Wait([&] { return With(guide, [&](BWindow &w) {
      auto *description = dynamic_cast<BTextView *>(w.FindView("layout description"));
      return description && std::string(description->Text()).find("single") != std::string::npos;
    }); }), "draft selection previews description");
    Check(Property(file, "Workspace") == "Tvískipt", "draft does not alter existing workspace");
    Check(With(guide, [&](BWindow &w) {
      auto *button = dynamic_cast<BButton *>(w.FindView(apply ? "apply layout" : "skip layout"));
      return button && button->Invoke() == B_OK;
    }), "guide button invoked");
    Check(Wait([&] { return !guide.IsValid(); }), "guide closed");
  };
  choose(false);
  Check(Property(file, "Workspace") == "Tvískipt", "cancel preserves original layout");
  choose(true);
  Check(Wait([&] { return Property(file, "Workspace") == "Einn"; }),
        "confirmed choice applies through the normal workspace controller");
  main->Lock();
  main->Quit();
}
