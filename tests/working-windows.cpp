// SPDX-License-Identifier: MIT
#include "Common.h"
#include "FileOperation.h"
#include "ReadWorker.h"
#include "ImagePreview.h"
#include "MainWindow.h"
#include "MainWindowMenu.h"
#include "PanelTools.h"
#include "Pane.h"
#include "SavedPlaces.h"
#include "SavedSearches.h"
#include "WindowOverview.h"
#include <Application.h>
#include <Button.h>
#include <ListView.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <RadioButton.h>
#include <PopUpMenu.h>
#include <StringItem.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>
#include <cstdlib>
#include <atomic>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

namespace {
std::string copyFolder;
std::atomic<bool> holdNextStat{false}, chooseDestination{false};
sem_id statEntered, releaseStat;
}
extern "C" int __real_stat(const char *, struct stat *);
extern "C" int __wrap_stat(const char *path, struct stat *info) {
  if (holdNextStat.load() && path == copyFolder && holdNextStat.exchange(false)) {
    release_sem(statEntered);
    acquire_sem_etc(releaseStat, 1, B_RELATIVE_TIMEOUT, 8000000);
  }
  return __real_stat(path, info);
}
extern "C" BMenuItem *__real__ZN10BPopUpMenu2GoE6BPointbbb(BPopUpMenu *, BPoint, bool, bool, bool);
extern "C" BMenuItem *__wrap__ZN10BPopUpMenu2GoE6BPointbbb(
    BPopUpMenu *menu, BPoint at, bool deliver, bool open, bool asynchronous) {
  if (std::strcmp(menu->Name(), "File actions") == 0 ||
      std::strcmp(menu->Name(), "Folder actions") == 0)
    return menu->FindItem("Detach to Einn");
  if (std::strcmp(menu->Name(), "Copy to window") != 0)
    return __real__ZN10BPopUpMenu2GoE6BPointbbb(menu, at, deliver, open, asynchronous);
  if (chooseDestination.exchange(false))
    static_cast<BInvoker *>(menu->ItemAt(0))->Invoke();
  delete menu;
  return nullptr;
}

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
class CopySource : public WorkingWindow {
public:
  std::atomic<int> dispatches{0}, offered{0}, accepted{0}, rejected{0};
  Listi::Job original;
  explicit CopySource(const std::string &source)
      : WorkingWindow(BRect(0, 0, 300, 200), "Copy source", "test", 0) {
    original.sources = {source};
  }
  void MessageReceived(BMessage *m) override {
    try {
      Job job;
      if (m->what == kCopyToWindow) {
        PickCopyFolder(original);
        ++offered;
      } else if (m->what == 'ping') {
        ++dispatches;
      } else if (HandleCopyFolder(*m, job)) {
        if (job.checkDestinationWindow)
          ++accepted;
      } else {
        WorkingWindow::MessageReceived(m);
      }
    } catch (const std::exception &) {
      ++rejected;
    }
  }
};

void CheckMoveToWindow() {
  Preferences prefs;
  prefs.workspace = Workspace::einn;
  auto *source = CreateMainWindow(prefs, {}, {}, false, {}, false);
  auto *target = CreateMainWindow(prefs, {}, {}, false, {}, false);
  BMessenger from(source), to(target);
  source->Show();
  target->Show();
  int64 id = 0;
  Check(With(from, [&](BWindow &w) {
    auto &bar = static_cast<PanelTools *>(w.FindView("first panel tools"))->FolderBar();
    bar.OpenDirectory("/boot/system");
    bar.Rename("Transferred");
    id = bar.ActiveId();
    w.MenusBeginning();
    auto *item = w.KeyMenuBar()->FindItem(kMoveTabToWindow);
    const bool invoked = item && static_cast<BInvoker *>(item)->Invoke() == B_OK;
    w.MenusEnded();
    return invoked;
  }), "invoke the chosen destination from the native Window menu");
  Check(Wait([&] { return With(to, [&](BWindow &w) {
    auto &bar = static_cast<PanelTools *>(w.FindView("first panel tools"))->FolderBar();
    return bar.Count() == 2 && bar.ActiveId() == id && bar.Snapshot().alias == "Transferred";
  }); }), "destination keeps its original tab and accepts the selected tab identity");
  Check(With(from, [&](BWindow &w) {
    auto &bar = static_cast<PanelTools *>(w.FindView("first panel tools"))->FolderBar();
    return bar.Count() == 1 && !bar.CopyTab(id);
  }), "source window and its other tab remain open");
  Check(With(from, [](BWindow &w) { return w.QuitRequested(); }), "source ready to close");
  Check(from.SendMessage(B_QUIT_REQUESTED) == B_OK, "source close request delivered");
  Check(Wait([&] { return !from.IsValid(); }), "source window closes independently");
  to.SendMessage(B_QUIT_REQUESTED);
  Check(Wait([&] { return !to.IsValid(); }), "destination window closes");
}

void CheckContextDetach() {
  Preferences prefs;
  prefs.workspace = Workspace::tviskipt;
  std::optional<Listi::FolderTab> detached;
  auto capture = [&](BrowserRequest request) {
    detached = request.detached;
    return false; // Keep the original tab for the next context-menu check.
  };
  auto *window = CreateMainWindow(prefs, {}, {}, false, {}, false, {}, capture);
  BMessenger owner(window);
  auto *right = static_cast<PanelTools *>(window->FindView("second panel tools"));
  auto *left = static_cast<PanelTools *>(window->FindView("first panel tools"));
  window->Show();
  Check(With(owner, [&](BWindow &) {
    right->Files().Navigate("/boot");
    return true;
  }), "navigate context-menu fixture");
  Check(Wait([&] { return With(owner, [&](BWindow &) {
    return !right->Files().IsScanning();
  }); }), "right lister completes its directory scan");
  for (const char *clicked : {"", "/boot/system"}) {
    Check(With(owner, [&](BWindow &browser) {
      BMessage activate(kActivate);
      activate.AddInt32("pane", 0);
      browser.MessageReceived(&activate);
      if (!left->Files().IsSource())
        return false;
      detached.reset();
      right->Files().ContextMenu(clicked, BPoint(50, 50));
      return detached && detached->id == right->FolderBar().ActiveId() &&
             detached->state.location == "/boot" &&
             detached->id != left->FolderBar().ActiveId();
    }), *clicked ? "folder row can detach its lister without entering the selected folder"
                 : "background can detach the right lister independently of prior focus");
  }
  owner.SendMessage(B_QUIT_REQUESTED);
  Check(Wait([&] { return !owner.IsValid(); }), "context menu fixture closes");
}

void CheckCopyWindows() {
  char temp[] = "/boot/home/Listi-copy-window-XXXXXX";
  Check(mkdtemp(temp), "create disposable copy-window fixture");
  const std::string root(temp);
  const auto source = root + "/source";
  copyFolder = root + "/target";
  std::ofstream(source) << "selected";
  statEntered = create_sem(0, "target stat entered");
  releaseStat = create_sem(0, "release target stat");
  auto *owner = new CopySource(source);
  owner->Show();
  BMessenger requester(owner);
  int requests = 0, rejections = 0;
  for (int scenario = 0; scenario < 4; ++scenario) {
    Check(mkdir(copyFolder.c_str(), 0700) == 0, "create destination directory");
    struct stat original{};
    Check(stat(copyFolder.c_str(), &original) == 0, "capture directory scan identity");
    auto *target = new WorkingWindow(BRect(0, 0, 300, 200), "Copy target", "test", 0);
    target->SetCopyFolder(copyFolder, {int32_t(original.st_dev), int64_t(original.st_ino)});
    target->Show();
    BMessenger destination(target);
    chooseDestination = true;
    holdNextStat = true;
    requester.SendMessage(kCopyToWindow);
    ++requests;
    Check(acquire_sem_etc(statEntered, 1, B_RELATIVE_TIMEOUT, 3000000) == B_OK,
          "selected destination reaches a blocked stat");
    requester.SendMessage('ping');
    Check(Wait([&] { return owner->dispatches > scenario; }),
          "source dispatches while destination stat is blocked");
    if (scenario == 0) {
      requester.SendMessage(kCopyToWindow); // Simulate dismissal of the replacement chooser.
      ++requests;
      Check(Wait([&] { return owner->offered == requests; }),
            "replacement chooser can be dismissed while the old probe is blocked");
    } else if (scenario == 1) {
      destination.SendMessage(B_QUIT_REQUESTED);
      Check(Wait([&] { return !destination.IsValid(); }), "destination closes before probe returns");
    } else if (scenario == 2) {
      Check(With(destination, [&](BWindow &window) {
        static_cast<WorkingWindow &>(window).SetCopyFolder(root, {});
        return true;
      }), "destination changes location before probe returns");
    } else {
      Check(rename(copyFolder.c_str(), (root + "/retired").c_str()) == 0 &&
            mkdir(copyFolder.c_str(), 0700) == 0, "replace destination directory identity");
    }
    release_sem(releaseStat);
    if (scenario != 0) {
      ++rejections;
      Check(Wait([&] { return owner->rejected == rejections; }), "stale destination is rejected");
    }
    Check(DrainReadWorkers(2000000), "retired destination probe exits after release");
    Check(owner->accepted == 0, "retired probe never returns a job");
    if (destination.IsValid()) {
      destination.SendMessage(B_QUIT_REQUESTED);
      Check(Wait([&] { return !destination.IsValid(); }), "fixture destination closes");
    }
    rmdir(copyFolder.c_str());
  }
  requester.SendMessage(B_QUIT_REQUESTED);
  Check(Wait([&] { return !requester.IsValid(); }), "fixture copy source closes");
  delete_sem(statEntered);
  delete_sem(releaseStat);
  unlink(source.c_str());
  rmdir((root + "/retired").c_str());
  rmdir(root.c_str());
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
    auto guide = Dialog("Listi setup");
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
  CheckCopyWindows();
  CheckMoveToWindow();
  CheckContextDetach();
}
