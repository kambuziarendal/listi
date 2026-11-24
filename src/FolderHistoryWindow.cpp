// SPDX-License-Identifier: MIT
#include "FolderHistory.h"
#include "HistorySettings.h"
#include "AutomaticHistory.h"
#include "FolderHistoryListing.h"
#include "PanelTools.h"
#include "ViewVisibility.h"

#include <Button.h>
#include <Entry.h>
#include <FilePanel.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <Messenger.h>
#include <Path.h>
#include <ScrollView.h>
#include <Screen.h>
#include <Slider.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace Listi {
namespace {
namespace fs = std::filesystem;
constexpr uint32 kOpen = 'fbop', kChoose = 'fbch', kChosen = 'fbcn', kSave = 'fbsv',
    kLoad = 'fbld', kOlder = 'fbol', kNewer = 'fbnw', kSelect = 'fbse',
    kPickFile = 'fbpf', kPickFolder = 'fbpd', kRestore = 'fbrs', kDone = 'fbdn',
    kCancel = 'fbca', kRefresh = 'fbrf', kSuggest = 'fbsu', kConfirm = 'fbcf', kBack = 'fbbk';
std::atomic_int active{0};

std::string PointDate(const std::string &id) {
  time_t seconds = std::strtoll(id.c_str() + 6, nullptr, 10) / 1000000;
  tm local{};
  char text[64];
  if (localtime_r(&seconds, &local) && strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local))
    return text;
  return "Unknown date";
}

// A suggestion is not a reservation. Restore still creates the result exclusively.
std::string SuggestCopy(const fs::path &parent, const std::string &name) {
  const fs::path base(name);
  for (unsigned i = 1; i <= 1000; ++i) {
    auto proposal = i == 1 ? base : fs::path(base.stem().string() + " (" +
        std::to_string(i) + ")" + base.extension().string());
    auto path = parent / proposal;
    if (!fs::exists(fs::symlink_status(path))) return path.string();
  }
  throw std::runtime_error("Choose another containing folder: no unused copy name was found.");
}

class Timeline : public BSlider {
public:
  Timeline() : BSlider("version timeline", "Saved version", new BMessage(kLoad),
                       0, 1, B_HORIZONTAL) {
    SetModificationMessage(new BMessage(kLoad));
    SetToolTip("Slide or scroll to browse saved versions");
  }
  void MessageReceived(BMessage *m) override {
    if (m->what == B_MOUSE_WHEEL_CHANGED && IsEnabled()) {
      float delta = m->GetFloat("be:wheel_delta_y", 0);
      if (delta != 0) {
        SetValue(Value() + (delta > 0 ? -1 : 1));
        Invoke();
      }
      return;
    }
    BSlider::MessageReceived(m);
  }
};

struct Task {
  std::atomic_bool cancel{false};
  std::mutex lock;
  std::shared_ptr<HistoryStore> store;
  std::vector<std::string> points;
  HistoryRows rows;
  std::string notice, suggestion, file;
  bool manual = false, failed = false;
};

class HistoryView : public BGroupView {
public:
  HistoryView(const char *path, int32 side)
      : BGroupView(B_VERTICAL), source(path), panel(side) {
    SetName("folder history browser");
    auto *header = new BStringView("history source", ("History: " + source).c_str());
    header->SetTruncation(B_TRUNCATE_MIDDLE);
    header->SetToolTip(source.c_str());
    auto controls = BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING);
    controls.SetInsets(B_USE_SMALL_SPACING);
    auto top = controls.AddGroup(B_HORIZONTAL);
    top.Add(header).AddGlue();
    refresh = Button("Refresh", kRefresh);
    top.Add(refresh);
    if (panel >= 0) {
      auto *message = new BMessage(kClosePanelPage);
      message->AddInt32("pane", panel);
      close = new BButton("close history", "×", message);
      close->SetToolTip("Close history and return to this panel’s files");
      top.Add(close);
    }
    top.End();
    choose = Button("Choose history folder…", kChoose);
    controls.Add(choose, 0);
    timeline = new Timeline();
    older = Button("‹", kOlder);
    newer = Button("›", kNewer);
    older->SetToolTip("Previous saved version");
    newer->SetToolTip("Next saved version");
    controls.AddGroup(B_HORIZONTAL).Add(older).Add(timeline).Add(newer).End();
    list = new BListView("saved files");
    list->SetSelectionMessage(new BMessage(kSelect));
    controls.Add(new BScrollView("saved files scroll", list, 0, false, true));
    notice = new BStringView("history status", "Looking for saved versions…");
    notice->SetTruncation(B_TRUNCATE_END);
    controls.Add(notice);
    if (panel < 0) {
      save = Button("Save version", kSave);
      one = Button("Restore file…", kPickFile);
      all = Button("Restore folder…", kPickFolder);
      cancel = Button("Cancel", kCancel);
      confirmRow = new BGroupView(B_VERTICAL);
      copyPath = new BTextControl("restore path", "New copy:", "", nullptr);
      confirm = Button("Restore copy", kConfirm);
      back = Button("Cancel", kBack);
      BLayoutBuilder::Group<>(confirmRow, B_VERTICAL, B_USE_SMALL_SPACING).Add(copyPath)
          .AddGroup(B_HORIZONTAL).AddGlue().Add(back).Add(confirm).End();
      controls.Add(confirmRow, 0);
      SetViewVisible(confirmRow, false);
      actions = new BGroupView(B_HORIZONTAL);
      BLayoutBuilder::Group<>(actions, B_HORIZONTAL).Add(save).Add(cancel).AddGlue().Add(one).Add(all);
      controls.Add(actions, 0);
    }
    SetViewVisible(choose, false);
    if (close) close->SetFlat(true);
    Update();
  }

  ~HistoryView() override {
    if (task) task->cancel = true;
    Clear();
  }

  bool Writing() const { return task && (kind == kRestore || kind == kSave); }

  void AttachedToWindow() override {
    BGroupView::AttachedToWindow();
    for (auto *button : {older, newer, choose, refresh, save, cancel, one, all, confirm, back})
      if (button) button->SetTarget(this);
    list->SetTarget(this);
    timeline->SetTarget(this);
    if (close) close->SetTarget(Window());
    Start(kOpen);
  }

  void MessageReceived(BMessage *m) override {
    switch (m->what) {
    case kRefresh: Start(store ? kRefresh : kOpen); break;
    case kChoose:
      if (configuredUnavailable) {
        if (LaunchHistoryHelper(true) != B_OK) Status("Could not open automatic history settings.");
      } else Pick(false, {});
      break;
    case kChosen: {
      entry_ref ref;
      BPath path;
      if (!task && m->FindRef("refs", &ref) == B_OK &&
          BEntry(&ref, true).GetPath(&path) == B_OK)
        Start(kOpen, path.Path());
      break;
    }
    case kSave: Start(kSave); break;
    case kCancel:
      if (task) {
        task->cancel = true;
        cancel->SetEnabled(false);
        Status("Cancel requested…");
      }
      break;
    case kOlder: case kNewer:
      timeline->SetValue(timeline->Value() + (m->what == kOlder ? -1 : 1));
      // fall through
    case kLoad:
      EndConfirmation();
      if (Wanted() != loaded) {
        Clear();
        if (!task) Start(kLoad);
      }
      Update();
      break;
    case kSelect: Update(); break;
    case kPickFile: case kPickFolder:
      if (store && !task && !loaded.empty() && loaded == Wanted() &&
          (m->what == kPickFolder || SelectedFile()))
        Pick(true, m->what == kPickFolder ? "" : SelectedFile()->path);
      break;
    case kSuggest: {
      entry_ref ref;
      BPath path;
      if (!task && m->FindRef("refs", &ref) == B_OK &&
          BEntry(&ref, true).GetPath(&path) == B_OK) {
        auto version = std::string(m->GetString("point", ""));
        if (version != loaded || version != Wanted()) {
          Status("The version changed. Choose Restore again.");
          break;
        }
        Start(kSuggest, path.Path(), m->GetString("file", ""));
      }
      break;
    }
    case kConfirm:
      if (!task && !pendingPoint.empty() && loaded == pendingPoint && loaded == Wanted()) {
        std::string destination = copyPath->Text();
        auto file = pendingFile;
        EndConfirmation();
        Start(kRestore, destination, file);
      }
      break;
    case kBack: EndConfirmation(); Status("Restore canceled."); Update(); break;
    case kDone:
      Finish();
      break;
    default: BGroupView::MessageReceived(m);
    }
  }

private:
  BButton *Button(const char *label, uint32 what) {
    return new BButton(label, label, new BMessage(what));
  }
  void Status(const std::string &text) {
    notice->SetText(text.c_str());
    notice->SetToolTip(text.c_str());
  }
  std::string Wanted() const {
    const auto i = timeline->Value();
    return i >= 0 && size_t(i) < points.size() ? points[i] : "";
  }
  const HistoryRow *SelectedFile() const {
    auto i = list->CurrentSelection();
    return i >= 0 && size_t(i) < rows.size() && S_ISREG(rows[i].mode) ? &rows[i] : nullptr;
  }
  void Clear() {
    while (list->CountItems()) delete list->RemoveItem(list->CountItems() - 1);
    rows.clear();
    loaded.clear();
  }
  void Update() {
    const bool browse = !task || kind == kLoad;
    const auto i = timeline->Value();
    const auto id = Wanted();
    std::string label = id.empty() ? "Saved versions" : PointDate(id) + "  (" +
        std::to_string(i + 1) + "/" + std::to_string(points.size()) + ")";
    timeline->SetLabel(label.c_str());
    timeline->SetEnabled(browse && points.size() > 1);
    older->SetEnabled(browse && i > 0);
    newer->SetEnabled(browse && size_t(i + 1) < points.size());
    refresh->SetEnabled(!task);
    choose->SetEnabled(!task);
    if (save) {
      SetViewVisible(actions, pendingPoint.empty());
      save->SetEnabled(!task && bool(store));
      SetViewVisible(cancel, Writing());
      cancel->SetEnabled(task && !task->cancel);
      all->SetEnabled(!task && !loaded.empty() && loaded == id);
      one->SetEnabled(all->IsEnabled() && SelectedFile());
    }
  }
  void Pick(bool restore, const std::string &file) {
    BMessenger target(this);
    BMessage selected(restore ? kSuggest : kChosen);
    selected.AddString("point", loaded.c_str());
    selected.AddString("file", file.c_str());
    picker = std::make_unique<BFilePanel>(B_OPEN_PANEL,
        &target, nullptr, B_DIRECTORY_NODE, false, &selected);
    picker->SetButtonLabel(B_DEFAULT_BUTTON, restore ? "Choose destination folder" : "Choose history folder");
    picker->Show();
  }

  void EndConfirmation() {
    pendingPoint.clear();
    pendingFile.clear();
    if (confirmRow) SetViewVisible(confirmRow, false);
  }

  void Start(uint32 action, std::string path = {}, std::string file = {}) {
    if (task || (action != kOpen && !store)) return;
    auto id = Wanted();
    if (action == kLoad && id.empty()) return;
    auto work = std::make_shared<Task>();
    if (active.fetch_add(1) >= 8) {
      --active;
      Status("History readers are busy. Try Refresh shortly.");
      return;
    }
    EndConfirmation();
    kind = action;
    loading = id;
    task = std::move(work);
    Status(action == kRestore ? "Restoring copy…" : action == kSave ? "Saving version…" : "Reading history…");
    Update();
    auto previous = timeline->Value() > 0 && !id.empty() ? points[timeline->Value() - 1] : "";
    try {
      std::thread([action, id, previous, path, file, source = source, existing = store,
                   work = task, reply = BMessenger(this)] {
        {
          const std::lock_guard<std::mutex> guard(work->lock);
          try {
            auto history = existing;
            if (action == kOpen) {
              auto pair = HistoryPairFor(ReadHistorySettings(), source);
              auto destination = path.empty() ? pair.GetString("path", 1, "") : path;
              if (destination.empty()) {
                work->manual = true;
                throw std::runtime_error("Choose the folder where versions are stored.");
              }
              try {
                history = std::make_shared<HistoryStore>(source, destination);
              } catch (const fs::filesystem_error &) {
                throw std::runtime_error("History folder is unavailable. Check that it is connected: " + destination);
              }
              if (pair.HasString("path") && !history->FolderIdentity().HasSameData(pair))
                throw std::runtime_error("Configured folders changed. Check automatic history settings.");
            }
            if (action == kOpen || action == kRefresh || action == kSave) {
              if (action == kSave) history->Save(work->cancel);
              auto found = history->Points();
              const char *point;
              for (int32 i = 0; found.FindString("point", i, &point) == B_OK; ++i)
                work->points.emplace_back(point);
              std::reverse(work->points.begin(), work->points.end());
              work->store = std::move(history);
            } else if (action == kLoad) {
              work->rows = DescribeVersion(*history, id, previous, work->notice, work->cancel);
            } else if (action == kSuggest) {
              auto name = file.empty() ? fs::path(source).filename().string() + " restored" :
                  fs::path(file).filename().string();
              work->suggestion = SuggestCopy(path, name);
              work->file = file;
              work->notice = "Review the new copy. Existing files will not be replaced.";
            } else {
              history->Restore(id, path, work->cancel, file);
              work->notice = "Restored copy: " + path;
            }
          } catch (const std::exception &error) {
            work->notice = error.what();
            work->rows.clear();
            work->store.reset();
            work->points.clear();
            work->failed = true;
          }
        }
        --active;
        reply.SendMessage(kDone);
      }).detach();
    } catch (const std::exception &e) {
      --active;
      task.reset();
      Status(e.what());
      Update();
    }
  }
  void Finish() {
    if (!task) return;
    auto work = std::move(task);
    const std::lock_guard<std::mutex> guard(work->lock);
    Status(work->notice);
    if (work->store) {
      store = std::move(work->store);
      SetViewVisible(choose, false);
      Clear();
      points = std::move(work->points);
      timeline->SetLimits(0, std::max<int32>(1, int32(points.size()) - 1));
      timeline->SetValue(points.empty() ? 0 : int32(points.size()) - 1);
      if (points.empty()) Status("No versions saved yet.");
      else Start(kLoad);
    } else if (kind == kSuggest && !work->failed && loading == Wanted()) {
      pendingPoint = loading;
      pendingFile = std::move(work->file);
      copyPath->SetText(work->suggestion.c_str());
      copyPath->SetToolTip(work->suggestion.c_str());
      SetViewVisible(confirmRow, true);
    } else if (kind == kLoad) {
      if (Wanted() != loading) Start(kLoad);
      else if (!work->failed) {
        Clear();
        for (auto &[path, entry] : work->rows) {
          std::string label = path + (S_ISDIR(entry.mode) ? "/" : "");
          for (char &c : label) if (c >= 0 && c < ' ') c = '?';
          if (!entry.change.empty()) label += "  ·  " + entry.change;
          rows.push_back(std::move(entry));
          list->AddItem(new BStringItem(label.c_str()));
        }
        loaded = loading;
      }
    } else if (kind == kOpen) {
      configuredUnavailable = !work->manual && !manual;
      choose->SetLabel(configuredUnavailable ? "Automatic history settings…" : "Choose history folder…");
      SetViewVisible(choose, true);
    }
    manual |= work->manual;
    Update();
  }

  const std::string source;
  const int32 panel;
  BButton *older, *newer, *refresh, *choose, *close = nullptr, *save = nullptr,
          *one = nullptr, *all = nullptr, *cancel = nullptr;
  BSlider *timeline;
  BListView *list;
  BStringView *notice;
  std::unique_ptr<BFilePanel> picker;
  std::shared_ptr<HistoryStore> store;
  std::shared_ptr<Task> task;
  uint32 kind = 0;
  bool manual = false, configuredUnavailable = false;
  std::string pendingPoint, pendingFile;
  BGroupView *confirmRow = nullptr, *actions = nullptr;
  BTextControl *copyPath = nullptr;
  BButton *confirm = nullptr, *back = nullptr;
  std::vector<std::string> points;
  std::vector<HistoryRow> rows;
  std::string loaded, loading;
};

class HistoryWindow : public BWindow {
public:
  explicit HistoryWindow(const char *source)
      : BWindow(BRect(140, 100, 770, 520), "Folder history", B_TITLED_WINDOW,
                B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS), view(new HistoryView(source, -1)) {
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0).Add(view);
    GetLayout()->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    auto screen = BScreen(this).Frame();
    ResizeTo(std::min(620.f, screen.Width() - 32), std::min(420.f, screen.Height() - 56));
    MoveOnScreen();
  }
  bool QuitRequested() override { return !view->Writing(); }
private:
  HistoryView *view;
};
}

bool FolderHistoryBusy() { return active.load() != 0; }
void OpenFolderHistory(const char *source) { (new HistoryWindow(source))->Show(); }
BView *CreateHistoryPreview(const char *source, int32 pane) { return new HistoryView(source, pane); }
}
