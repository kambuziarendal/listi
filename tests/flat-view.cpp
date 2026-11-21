// SPDX-License-Identifier: MIT
#include "FileSearch.h"
#include "Common.h"
#include "FileClipboard.h"
#include "FlatView.h"
#include "SearchWorker.h"
#include "SearchWindow.h"
#include "SavedSearches.h"
#include "Shelf.h"
#include <Application.h>
#include <LayoutBuilder.h>
#include <Button.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <Menu.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <String.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>
#include <atomic>
#include <cerrno>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

using namespace Listi;

namespace {
std::string root, denied;
std::atomic<bool> pauseRoot{false}, dropCompletion{false};
sem_id openedRoot, resumeRoot;
}

extern "C" status_t __real__ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl(const BMessenger *, BMessage *, BHandler *, bigtime_t);
extern "C" status_t __wrap__ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl(const BMessenger *target, BMessage *m, BHandler *reply, bigtime_t timeout) {
  if (m->what == kSearchDone && dropCompletion.load()) return B_TIMED_OUT;
  return __real__ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl(target,m,reply,timeout);
}

extern "C" DIR *__real_opendir(const char *);
extern "C" DIR *__wrap_opendir(const char *path) {
  if (denied == path) {
    errno = EACCES;
    return nullptr;
  }
  if (root == path && pauseRoot.load()) {
    release_sem(openedRoot);
    acquire_sem_etc(resumeRoot, 1, B_RELATIVE_TIMEOUT, 5000000);
  }
  return __real_opendir(path);
}

namespace {
int failures = 0;

void Check(bool success, const char *label) {
  failures += !success;
  std::cout << (success ? "PASS " : "FAIL ") << label << std::endl;
}

template <class Condition> bool Await(Condition condition) {
  const auto deadline = system_time() + 3000000;
  do {
    if (condition())
      return true;
    snooze(10000);
  } while (system_time() < deadline);
  return false;
}

template <class CheckWindow>
bool Inspect(const BMessenger &target, CheckWindow check) {
  if (!target.LockTarget())
    return false;
  BLooper *looper = nullptr;
  auto window = dynamic_cast<BWindow *>(target.Target(&looper));
  const bool passed = window && check(*window);
  looper->Unlock();
  return passed;
}

bool TextContains(BWindow &window, const char *view, const char *text) {
  auto label = dynamic_cast<BStringView *>(window.FindView(view));
  return label && BString(label->Text()).FindFirst(text) >= 0;
}

BColumnListView *Results(BWindow &window) {
  return dynamic_cast<BColumnListView *>(window.FindView("flat results"));
}

std::set<std::string> Folders(const BColumnListView &results) {
  std::set<std::string> folders;
  for (int32 i = 0; i < results.CountRows(); ++i) {
    auto field =
        static_cast<const BStringField *>(results.RowAt(i)->GetField(1));
    folders.insert(field->String());
  }
  return folders;
}

class TestWindow : public BWindow {
public:
  std::string opened;
  int contextClicks = 0;
  TestWindow() : BWindow(BRect(50, 50, 700, 450), "Test lister", B_TITLED_WINDOW,
                         B_ASYNCHRONOUS_CONTROLS) {}
  void MessageReceived(BMessage *message) override {
    if (message->what == kFlatContext)
      ++contextClicks;
    else if (message->what == kOpenPaths)
      opened = message->GetString("path", "");
    else
      BWindow::MessageReceived(message);
  }
};

BMessenger HostFlatView(const std::shared_ptr<Shelf> &shelf, const char *path) {
  auto *window = new TestWindow();
  BLayoutBuilder::Group<>(window, B_VERTICAL).Add(CreateFlatView(BMessenger(window), shelf, path, 0));
  BMessenger host(window);
  window->Show();
  return host;
}

void SetFilter(BWindow &window, const char *text) {
  auto field = static_cast<BTextControl *>(window.FindView("flat filter"));
  field->SetText(text);
  window.PostMessage(field->ModificationMessage(), field->Target());
}
}

int main() {
  BApplication app("application/x-vnd.Listi-FlatViewTest");
  char pattern[] = "/boot/home/listi-flat-view-XXXXXX";
  if (!mkdtemp(pattern))
    return 2;
  root = pattern;
  denied = root + "/denied";
  const auto nested = root + "/Kitchen";
  const auto first = root + "/same.txt";
  const auto second = nested + "/same.txt";
  mkdir(nested.c_str(), 0700);
  mkdir(denied.c_str(), 0700);
  std::ofstream(first) << "root";
  std::ofstream(second) << "nested";
  std::ofstream(denied + "/unreadable.txt") << "unreadable";
  symlink(root.c_str(), (nested + "/cycle").c_str());
  symlink(first.c_str(), (root + "/file-link").c_str());

  SearchRequest request;
  request.flat = true;
  request.root = root;
  request.resultLimit = 1;
  auto bounded = SearchFiles(
      request, [] { return true; }, [](const SearchHit &) { return true; });
  Check(bounded.limited && bounded.matched == 1,
        "Flat View explicitly reports a bounded result set");
  request.resultLimit = 5000;
  request.visitLimit = 1;
  bounded = SearchFiles(
      request, [] { return true; }, [](const SearchHit &) { return true; });
  Check(bounded.limited && bounded.visited == 1,
        "Flat View bounds traversal even where no file matches yet");

  auto shelf = std::make_shared<Shelf>();
  auto flat = HostFlatView(shelf, root.c_str());
  Check(
      Await([&] {
        return Inspect(flat, [&](BWindow &window) {
          auto rows = Results(window);
          return rows && rows->CountRows() == 2 &&
                 Folders(*rows) == std::set<std::string>{".", "Kitchen"} &&
                 TextContains(window, "flat status", "Incomplete: 1");
        });
      }),
      "duplicate names retain their origin; links and denied branch are not traversed");
  Inspect(flat, [&](BWindow &window) {
    if (auto rows = Results(window)) {
      for (int32 i = 0; i < rows->CountRows(); ++i) {
        auto row = rows->RowAt(i);
        auto folder = static_cast<BStringField *>(row->GetField(1));
        if (BString(folder->String()) == ".")
          rows->AddToSelection(row);
      }
      rows->SelectionChanged();
      Check(FlatSelectedFile(*window.FindView("flat view")) == first,
            "Inspector selection uses the exact original path");
    }
    SetFilter(window, "KITCHEN");
    return true;
  });
  Check(Await([&] {
          return Inspect(flat, [&](BWindow &window) {
            auto rows = Results(window);
            return rows && rows->CountRows() == 1 &&
                   !rows->CurrentSelection() &&
                   FlatSelectedFile(*window.FindView("flat view")).empty() &&
                   Folders(*rows) == std::set<std::string>{"Kitchen"};
          });
        }),
        "folder filtering preserves shown origins and deselects hidden files");
  Inspect(flat, [&](BWindow &window) {
    SetFilter(window, "sAmE");
    return true;
  });
  Check(Await([&] {
          return Inspect(flat, [&](BWindow &window) {
            auto rows = Results(window);
            if (!rows || rows->CountRows() != 2)
              return false;
            rows->AddToSelection(rows->RowAt(0));
            rows->AddToSelection(rows->RowAt(1));
            rows->SelectionChanged();
            Check(FlatSelectedFile(*window.FindView("flat view")).empty(),
                  "Inspector clears for multiple selected files");
            auto button =
                static_cast<BButton *>(window.FindView("add flat selection"));
            button->Invoke();
            return true;
          });
        }),
        "case-insensitive name filter shows both originals for selection");
  Check(Await([&] {
          const auto snapshot = shelf->Snapshot();
          std::set<std::string> paths;
          for (const auto &entry : snapshot.entries)
            paths.insert(entry.path);
          return paths == std::set<std::string>{first, second};
        }),
        "multi-selection adds the exact originals to the shared Shelf");

  Check(Inspect(flat, [&](BWindow &window) {
    auto *view = window.FindView("flat view");
    auto *rows = Results(window);
    BMessage none(kNone), all(kAll), invert(kInvert), copy(kClipboardCopy);
    view->MessageReceived(&none);
    if (rows->CurrentSelection())
      return false;
    view->MessageReceived(&all);
    auto selected = FlatSelection(*view);
    if (std::set<std::string>(selected.sources.begin(), selected.sources.end()) !=
        std::set<std::string>{first, second})
      return false;
    view->MessageReceived(&invert);
    if (rows->CurrentSelection())
      return false;
    for (int32 i = 0; i < rows->CountRows(); ++i) {
      auto *row = rows->RowAt(i);
      if (BString(static_cast<BStringField *>(row->GetField(1))->String()) == "Kitchen")
        rows->SetFocusRow(row, true);
    }
    if (!be_clipboard->Lock())
      return false;
    const BMessage previous(*be_clipboard->Data());
    be_clipboard->Unlock();
    view->MessageReceived(&copy);
    if (!be_clipboard->Lock())
      return false;
    const auto copied = ReadFileClipboard(*be_clipboard->Data());
    *be_clipboard->Data() = previous;
    be_clipboard->Commit();
    be_clipboard->Unlock();
    rows->ItemInvoked();
    return copied.size() == 1 && copied.front().path == second;
  }), "selection commands, native file clipboard and invocation use the selected nested file");
  Check(Await([&] { return Inspect(flat, [&](BWindow &window) {
    return static_cast<TestWindow &>(window).opened == second;
  }); }), "double-click dispatches the actual nested file to the opener");

  auto rightClickRootFile = [&](bool selectAll) {
    return Inspect(flat, [&](BWindow &window) {
      auto *rows = Results(window);
      if (selectAll) {
        BMessage all(kAll);
        window.FindView("flat view")->MessageReceived(&all);
      }
      for (int32 i = 0; i < rows->CountRows(); ++i) {
        auto *row = rows->RowAt(i);
        BRect rect;
        if (BString(static_cast<BStringField *>(row->GetField(1))->String()) == "." &&
            rows->GetRowRect(row, &rect)) {
          BMessage click(B_MOUSE_DOWN);
          click.AddInt32("buttons", B_SECONDARY_MOUSE_BUTTON);
          click.AddPoint("where", BPoint(10, (rect.top + rect.bottom) / 2));
          return window.PostMessage(&click, rows->ScrollView()) == B_OK;
        }
      }
      return false;
    });
  };
  auto contextSelection = [&](int clicks, const std::set<std::string> &expected) {
    return Await([&] { return Inspect(flat, [&](BWindow &window) {
      const auto selected = FlatSelection(*window.FindView("flat view"));
      return static_cast<TestWindow &>(window).contextClicks == clicks &&
             std::set<std::string>(selected.sources.begin(), selected.sources.end()) == expected;
    }); });
  };
  Check(rightClickRootFile(false) && contextSelection(1, {first}),
        "right-clicking an unselected row targets that file only");
  Check(rightClickRootFile(true) && contextSelection(2, {first, second}),
        "right-clicking within a multi-selection preserves both file references");

  shelf->Clear();
  rename(second.c_str(), (second + ".old").c_str());
  std::ofstream(second) << "replacement";
  Inspect(flat, [&](BWindow &window) {
    auto button = static_cast<BButton *>(window.FindView("add flat selection"));
    button->Invoke();
    return true;
  });
  Check(
      Await([&] {
        return Inspect(flat, [&](BWindow &window) {
          return TextContains(window, "flat Shelf status", "changed");
        });
      }) &&
          shelf->Snapshot().entries.empty(),
      "a replacement rejects the entire frozen selection instead of collecting its new identity");

  flat.SendMessage(B_QUIT_REQUESTED);
  Check(Await([&] { return !flat.IsValid(); }), "Flat View closes cleanly");

  openedRoot = create_sem(0, "flat test worker started");
  resumeRoot = create_sem(0, "flat test worker may finish");
  pauseRoot = true;
  auto scanning = HostFlatView(shelf, root.c_str());
  Check(acquire_sem_etc(openedRoot, 1, B_RELATIVE_TIMEOUT, 3000000) == B_OK,
        "scan is running on a worker while the window is open");
  Inspect(scanning, [](BWindow &window) {
    auto *stop = dynamic_cast<BButton *>(window.FindView("cancel flat scan"));
    return stop && stop->Invoke() == B_OK;
  });
  Check(
      Await([&] {
        return Inspect(scanning, [&](BWindow &window) {
          return TextContains(window, "flat status",
                              "Incomplete: scan stopped.");
        });
      }),
      "Stop marks a partial snapshot incomplete without waiting for the scanner");
  pauseRoot = false;
  release_sem(resumeRoot);
  scanning.SendMessage(B_QUIT_REQUESTED);
  Check(Await([&] { return !scanning.IsValid(); }),
        "closing a cancelled Flat View joins its worker");
  dropCompletion = true;
  auto lost = HostFlatView(shelf, root.c_str());
  Check(Await([&] { return Inspect(lost, [](BWindow &window) {
      return TextContains(window, "flat status", "Incomplete: results could not be delivered.");
    }); }), "failed terminal delivery cannot leave Flat View scanning indefinitely");
  auto search = OpenFileSearch(BMessenger(), 0, nested.c_str(), SavedSearches{}.Message());
  Check(Inspect(search, [](BWindow &window) {
    auto field = dynamic_cast<BTextControl *>(window.FindView("search expression"));
    if (!field)
      return false;
    field->SetText("same");
    return window.PostMessage(kSearchStart) == B_OK;
  }), "start Find files with a real name criterion");
  Check(Await([&] { return Inspect(search, [](BWindow &window) {
    return TextContains(window, "search status", "could not be delivered");
  }); }), "Find files exposes a lost completion and allows retry");
  dropCompletion = false;
  search.SendMessage(kSearchStart);
  Check(Await([&] { return Inspect(search, [](BWindow &window) {
    return TextContains(window, "search status", "Complete snapshot");
  }); }), "a new search completes after the failed generation");
  // Use the visible sort menu. Both filenames are real matching entries;
  // changing order must not change which original the user selected.
  auto searchRows = [](BWindow &window) {
    return dynamic_cast<BColumnListView *>(window.FindView("search results"));
  };
  auto sortBy = [&](int32 order) {
    return Inspect(search, [&](BWindow &window) {
      auto field = dynamic_cast<BMenuField *>(window.FindView("result order"));
      auto item = field ? field->Menu()->ItemAt(order) : nullptr;
      return item && static_cast<BInvoker *>(item)->Invoke() == B_OK;
    });
  };
  auto resultName = [](BRow *row) {
    return std::string(static_cast<BStringField *>(row->GetField(0))->String());
  };
  Check(sortBy(1) && Await([&] {
    return Inspect(search, [&](BWindow &window) {
      auto rows = searchRows(window);
      if (!rows || rows->CountRows() != 2 ||
          resultName(rows->RowAt(0)) != "same.txt" ||
          resultName(rows->RowAt(1)) != "same.txt.old")
        return false;
      rows->AddToSelection(rows->RowAt(0));
      return true;
    });
  }), "standalone search sorts ascending by filename");
  Check(sortBy(2) && Await([&] {
    return Inspect(search, [&](BWindow &window) {
      auto rows = searchRows(window);
      return rows && rows->CountRows() == 2 &&
             resultName(rows->RowAt(0)) == "same.txt.old" &&
             resultName(rows->RowAt(1)) == "same.txt" &&
             rows->CurrentSelection() == rows->RowAt(1);
    });
  }), "descending sort keeps the selected original");
  search.SendMessage(kSearchStart);
  Check(Await([&] {
    return Inspect(search, [&](BWindow &window) {
      auto rows = searchRows(window);
      return rows && rows->CountRows() == 2 && !rows->CurrentSelection() &&
             resultName(rows->RowAt(0)) == "same.txt.old" &&
             TextContains(window, "search status", "Complete snapshot");
    });
  }), "new search inserts arriving hits in the selected order");
  search.SendMessage(B_QUIT_REQUESTED);
  Check(Await([&] { return !search.IsValid(); }), "Find files closes after retry");
  bool closedEveryTime = true;
  for (int i = 0; i < 24 && closedEveryTime; ++i) {
    auto transient = OpenFileSearch(BMessenger(), 0, nested.c_str(),
                                    SavedSearches{}.Message());
    transient.SendMessage(B_QUIT_REQUESTED);
    closedEveryTime = Await([&] { return !transient.IsValid(); });
  }
  Check(closedEveryTime, "standalone searches can close immediately on open");
  lost.SendMessage(B_QUIT_REQUESTED);
  Check(Await([&] { return !lost.IsValid(); }), "close after lost completion");
  delete_sem(openedRoot);
  delete_sem(resumeRoot);

  for (const auto &path : {first, second, second + ".old", nested + "/cycle",
                           root + "/file-link", denied + "/unreadable.txt"})
    unlink(path.c_str());
  rmdir(nested.c_str());
  rmdir(denied.c_str());
  rmdir(root.c_str());
  return failures ? 1 : 0;
}
