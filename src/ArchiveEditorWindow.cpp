// SPDX-License-Identifier: MIT
#include "ArchiveEditor.h"
#include "VirtualFile.h"
#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include "AdaptiveColumnList.h"
#include <ColumnTypes.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <Path.h>
#include <Roster.h>
#include <StringView.h>
#include <Window.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
namespace {
constexpr uint32 kBegin = 'aebg';
constexpr uint32 kLoaded = 'aeld', kDone = 'aedn', kTick = 'aetk',
                 kOpen = 'aeop', kSave = 'aesv', kFolder = 'aefl',
                 kCancel = 'aecn', kCanQuit = 'aecq', kFocus = 'aefc',
                 kRestart = 'aers';
struct WindowRecord {
  std::string member;
  BMessenger window;
};
std::mutex registryMutex;
std::vector<WindowRecord> windows;
std::string Root() {
  BPath p;
  if (find_directory(B_USER_SETTINGS_DIRECTORY, &p) != B_OK)
    throw std::runtime_error("Cannot locate archive edit recovery folder.");
  p.Append("Listi_archive_edits");
  return p.Path();
}
BStringView *Line(const char *name, const std::string &text) {
  auto v = new BStringView(name, text.c_str());
  v->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
  v->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
  v->SetTruncation(B_TRUNCATE_MIDDLE);
  v->SetToolTip(text.c_str());
  return v;
}
void Launch(const std::string &path, const std::string &editor) {
  entry_ref file, app;
  if (get_ref_for_path(path.c_str(), &file) != B_OK ||
      get_ref_for_path(editor.c_str(), &app) != B_OK)
    throw std::runtime_error("Could not locate the editor or working file. The "
                             "working copy is kept.");
  BMessage refs(B_REFS_RECEIVED);
  refs.AddRef("refs", &file);
  auto status = be_roster->Launch(&app, &refs);
  if (status != B_OK && status != B_ALREADY_RUNNING)
    throw std::runtime_error(std::string("Could not open editor: ") +
                             strerror(status));
}
struct Delivery {
  std::mutex mutex;
  std::shared_ptr<Listi::ArchiveEditSession> session;
  std::string error, status;
  std::atomic<bool> cancel{false};
};
class EditorWindow : public BWindow {
  std::shared_ptr<Delivery> delivery = std::make_shared<Delivery>();
  std::shared_ptr<Listi::ArchiveEditSession> session;
  std::unique_ptr<BMessageRunner> timer;
  BStringView *status, *copy;
  BButton *open, *save, *folder, *cancel, *restart;
  std::string editor;
  bool busy = false, paused = false, closing = false;
  void Status(const std::string &s) {
    status->SetText(s.c_str());
    status->SetToolTip(s.c_str());
  }
  void Controls() {
    open->SetEnabled(!busy && bool(session));
    save->SetEnabled(!busy && bool(session));
    folder->SetEnabled(!busy && bool(session));
    cancel->SetEnabled(busy);
    restart->SetEnabled(!busy && bool(session));
  }
  void Work(uint32 done,
            std::function<void(Delivery &, const Listi::Hooks &)> action) {
    busy = true;
    delivery->cancel = false;
    Controls();
    auto state = delivery;
    BMessenger target(this);
    std::thread([state, target, done, action] {
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->error.clear();
        state->status.clear();
        Listi::Hooks hooks;
        hooks.proceed = [state] { return !state->cancel.load(); };
        try {
          action(*state, hooks);
        } catch (const Listi::Cancelled &) {
          state->error = "Save cancelled. Working copy kept; any completed "
                         "archive save remains in place.";
        } catch (const std::exception &e) {
          state->error = e.what();
        }
      }
      target.SendMessage(done);
    }).detach();
  }

public:
  EditorWindow(const std::string &member, const std::string &application)
      : BWindow(BRect(150, 150, 970, 430), "Listi: Edit archive file",
                B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS),
        editor(application) {
    status = Line("archive edit status", "Preparing private working copy...");
    copy = Line("archive edit copy", "");
    restart =
        new BButton("Start from current archive...", new BMessage(kRestart));
    open = new BButton("Open editor", new BMessage(kOpen));
    save = new BButton("Save now", new BMessage(kSave));
    folder = new BButton("Open working folder", new BMessage(kFolder));
    cancel = new BButton("Cancel save", new BMessage(kCancel));
    BLayoutBuilder::Group<>(this, B_VERTICAL, 10)
        .SetInsets(12)
        .Add(Line("archive edit member", member))
        .Add(Line("archive edit instructions",
                  "Save in your editor. Stable file saves are written back "
                  "while this window stays open."))
        .Add(Line("archive edit recovery",
                  "Working copies, including decrypted files, are kept "
                  "privately on this computer after closing."))
        .Add(copy)
        .Add(status)
        .AddGroup(B_HORIZONTAL, 8)
        .Add(open)
        .Add(save)
        .Add(folder)
        .AddGlue()
        .Add(cancel)
        .End()
        .AddGroup(B_HORIZONTAL, 8)
        .Add(restart)
        .AddGlue()
        .Add(new BButton("Keep copy and close", new BMessage(B_QUIT_REQUESTED)))
        .End();
    AddShortcut('W', B_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
    {
      std::lock_guard<std::mutex> lock(registryMutex);
      windows.push_back({member, BMessenger(this)});
    }
    Controls();
    BMessage tick(kTick);
    timer = std::make_unique<BMessageRunner>(BMessenger(this), &tick, 1000000);
    BMessage begin(kBegin);
    begin.AddString("member", member.c_str());
    PostMessage(&begin);
    Show();
  }
  ~EditorWindow() override {
    timer.reset();
    std::lock_guard<std::mutex> lock(registryMutex);
    windows.erase(std::remove_if(windows.begin(), windows.end(),
                                 [this](const auto &r) {
                                   return r.window == BMessenger(this);
                                 }),
                  windows.end());
  }
  bool QuitRequested() override {
    if (busy) {
      closing = true;
      delivery->cancel = true;
      Status("Finishing the current check/save before closing. Working copy is "
             "kept.");
      return false;
    }
    return true;
  }
  void MessageReceived(BMessage *m) override {
    if (m->what == kBegin) {
      std::string member = m->GetString("member", ""), application = editor;
      Work(kLoaded, [member, application](auto &d, const auto &hooks) {
        d.session = Listi::ArchiveEditSession::Open(member, Root(), hooks);
        Launch(d.session->WorkingFile(), application);
        d.status =
            "Editor opened. Saved changes will be written back automatically.";
      });
      return;
    }
    if (m->what == kFocus) {
      Activate();
      return;
    }
    if (m->what == kCanQuit) {
      BMessage reply;
      reply.AddBool("busy", busy);
      m->SendReply(&reply);
      return;
    }
    if (m->what == kOpen) {
      if (!busy && session)
        Work(kDone, [this](auto &d, const auto &) {
          Launch(session->WorkingFile(), editor);
          d.status =
              "Editor opened. Save in the editor to update its working copy.";
        });
      return;
    }
    if (m->what == kRestart) {
      if (!busy && session) {
        std::string question = "Start a new edit from the current archive? The "
                               "previous working copy is kept here, but will "
                               "no longer save back automatically:\n\n" +
                               session->WorkingFile() +
                               "\n\nClose the old document in your editor "
                               "before using the new copy.";
        auto alert = new BAlert("Start a new archive edit", question.c_str(),
                                "Cancel", "Start new edit", nullptr,
                                B_WIDTH_AS_USUAL, B_WARNING_ALERT);
        if (alert->Go() == 1)
          Work(kLoaded, [this](auto &d, const auto &hooks) {
            d.session = session->StartFresh(hooks);
            Launch(d.session->WorkingFile(), editor);
            d.status = "New edit opened from the current archive. Previous "
                       "working copy retained.";
          });
      }
      return;
    }
    if (m->what == kCancel) {
      delivery->cancel = true;
      return;
    }
    if (m->what == kFolder) {
      if (!busy && session) {
        entry_ref ref;
        if (get_ref_for_path(
                Listi::ParentLocation(session->WorkingFile()).c_str(), &ref) ==
            B_OK)
          be_roster->Launch(&ref);
      }
      return;
    }
    if (m->what == kTick || m->what == kSave) {
      bool manual = m->what == kSave;
      if (closing || busy || !session || (!manual && paused))
        return;
      if (manual) {
        paused = false;
        Status("Saving working copy to archive...");
      }
      Work(kDone, [this, manual](auto &d, const auto &hooks) {
        if (manual || session->Observe(hooks) == Listi::ArchiveEditSession::Changed)
          d.status =
              session->Save(hooks)
                  ? "Saved to archive. Working copy retained."
                  : "Working copy already matches the last saved version.";
      });
      return;
    }
    if (m->what == kLoaded || m->what == kDone) {
      std::lock_guard<std::mutex> lock(delivery->mutex);
      busy = false;
      if (delivery->session) {
        session = delivery->session;
        copy->SetText(session->WorkingFile().c_str());
        copy->SetToolTip(session->WorkingFile().c_str());
      }
      if (!delivery->error.empty()) {
        paused = true;
        Status(delivery->error + " Automatic save paused.");
      } else {
        if (m->what == kLoaded)
          paused = false;
        if (!delivery->status.empty())
          Status(delivery->status);
      }
      Controls();
      if (closing)
        PostMessage(B_QUIT_REQUESTED);
      return;
    }
    BWindow::MessageReceived(m);
  }
};
struct EditRow : BRow {
  Listi::ArchiveEditRecord record;
  explicit EditRow(const Listi::ArchiveEditRecord &r) : record(r) {
    const auto label = r.error.empty() ? r.member : "Unavailable: " + r.directory;
    SetField(new BStringField(label.c_str()), 0);
  }
};
class RecoveryWindow : public BWindow {
  BColumnListView *list;
  BStringView *status;
  BButton *open;
  std::string editor;
  bool busy = true;
  struct Records {
    std::mutex mutex;
    std::vector<Listi::ArchiveEditRecord> values;
    std::string error;
  };
  std::shared_ptr<Records> records = std::make_shared<Records>();

public:
  explicit RecoveryWindow(const std::string &application)
      : BWindow(BRect(130, 110, 960, 510), "Listi: Resume archive edits",
                B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS),
        editor(application) {
    list = new Listi::AdaptiveColumnList("archive edit sessions",
                               B_WILL_DRAW | B_NAVIGABLE, B_FANCY_BORDER);
    list->AddColumn(
        new BStringColumn("Archive file", 760, 100, 2000, B_TRUNCATE_MIDDLE),
        0);
    list->SetSelectionMode(B_SINGLE_SELECTION_LIST);
    list->SetInvocationMessage(new BMessage(kOpen));
    list->SetSelectionMessage(new BMessage(kTick));
    list->SetTarget(this);
    list->SetExplicitMinSize(BSize(450, 180));
    open = new BButton("Resume selected edit", new BMessage(kOpen));
    open->SetEnabled(false);
    status =
        Line("archive recovery status", "Reading retained edit sessions...");
    BLayoutBuilder::Group<>(this, B_VERTICAL, 8)
        .SetInsets(10)
        .Add(list)
        .Add(status)
        .AddGroup(B_HORIZONTAL, 8)
        .Add(open)
        .AddGlue()
        .Add(new BButton("Close", new BMessage(B_QUIT_REQUESTED)))
        .End();
    Show();
    auto state = records;
    BMessenger target(this);
    std::thread([state, target] {
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        try {
          state->values = Listi::ListArchiveEdits(Root());
        } catch (const std::exception &e) {
          state->error = e.what();
        }
      }
      target.SendMessage(kLoaded);
    }).detach();
  }
  bool QuitRequested() override { return !busy; }
  void MessageReceived(BMessage *m) override {
    if (m->what == kLoaded) {
      std::lock_guard<std::mutex> lock(records->mutex);
      busy = false;
      for (auto &r : records->values)
        list->AddRow(new EditRow(r));
      list->InvalidateLayout();
      status->SetText(records->error.empty()
                          ? (records->values.empty()
                                 ? "No retained archive edits."
                                 : "Choose an edit to resume. Changed source "
                                   "archives will not be overwritten.")
                          : records->error.c_str());
      return;
    }
    if (m->what == kTick) {
      auto row = static_cast<EditRow *>(list->CurrentSelection());
      open->SetEnabled(row && row->record.error.empty());
      if (row) {
        const auto text = row->record.error.empty() ? std::string("Ready to resume.")
            : row->record.error + " Working files retained at " + row->record.directory;
        status->SetText(text.c_str());
        status->SetToolTip(text.c_str());
      }
      return;
    }
    if (m->what == kOpen) {
      auto row = static_cast<EditRow *>(list->CurrentSelection());
      if (row && row->record.error.empty())
        OpenArchiveEditor(row->record.member, editor);
      return;
    }
    BWindow::MessageReceived(m);
  }
};
} // namespace
void OpenArchiveEditor(const std::string &member, const std::string &editor) {
  {
    std::lock_guard<std::mutex> lock(registryMutex);
    for (const auto &r : windows)
      if (r.member == member) {
        r.window.SendMessage(kFocus);
        return;
      }
  }
  new EditorWindow(member, editor);
}
void ResumeArchiveEditors(const std::string &editor) {
  new RecoveryWindow(editor);
}
bool ArchiveEditorsAllowQuit() {
  std::vector<WindowRecord> copy;
  {
    std::lock_guard<std::mutex> lock(registryMutex);
    copy = windows;
  }
  for (const auto &r : copy) {
    BMessage query(kCanQuit), reply;
    if (r.window.IsValid() &&
        (r.window.SendMessage(&query, &reply, 500000, 500000) != B_OK ||
         reply.GetBool("busy", true)))
      return false;
  }
  return true;
}
