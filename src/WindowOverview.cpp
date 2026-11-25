// SPDX-License-Identifier: MIT
#include "WindowOverview.h"
#include "WrappedLabel.h"
#include "FileOperation.h"
#include "JobSnapshot.h"
#include "ReadWorker.h"
#include "TransferCommand.h"
#include <PopUpMenu.h>
#include <stdexcept>
#include <MenuItem.h>
#include <Volume.h>
#include <sys/stat.h>
#include <unistd.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MessageRunner.h>
#include <ScrollView.h>
#include <StringItem.h>
#include <TextControl.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace Listi {
namespace {
constexpr uint32 kRaise = 'wrai', kApplyName = 'wset', kSaveName = 'wsnm',
                 kRefreshWindows = 'wrfs', kSelectWindow = 'wsel', kShowWindow = 'wshw';

constexpr uint32 kProbeCopyFolder = 'pbcf', kCopyFolderReady = 'cfrd';
struct WindowInfo {
  int64 id;
  BMessenger target;
  std::string title, kind, location;
  bool named;
  std::string setName;
  std::string copyFolder;
  NodeIdentity copyNode;
  int64 copyRevision;
};
std::mutex registryLock;
std::atomic<int64> nextId{0};
std::map<int64, WindowInfo> windows;
uint64 revision = 0;

void Raise(BWindow &window) {
  const uint32 workspaces = window.Workspaces();
  if (workspaces && !(workspaces & (uint32(1) << current_workspace()))) {
    for (int32 i = 0; i < 32; ++i)
      if (workspaces & (uint32(1) << i)) {
        activate_workspace(i);
        break;
      }
  }
  if (window.IsMinimized())
    window.Minimize(false);
  window.Activate();
}

bool RaiseExisting(BMessenger previous) {
  BMessage raise(kRaise);
  return previous.IsValid() &&
         previous.SendMessage(&raise, static_cast<BHandler *>(nullptr), 50000) == B_OK;
}

class NameDialog : public BWindow {
  BMessenger owner;
  BTextControl *name;
public:
  NameDialog(BWindow &window, const std::string &current)
      : BWindow(BRect(0, 0, 410, 130), "Name window", B_TITLED_WINDOW,
                B_AUTO_UPDATE_SIZE_LIMITS | B_NOT_ZOOMABLE), owner(&window) {
    name = new BTextControl("window name", "Window name:", current.c_str(), nullptr);
    name->TextView()->SetMaxBytes(160);
    auto *save = new BButton("name save", "Save name", new BMessage(kSaveName));
    BLayoutBuilder::Group<>(this, B_VERTICAL)
        .SetInsets(B_USE_WINDOW_SPACING).Add(name)
        .AddGroup(B_HORIZONTAL).AddGlue()
        .Add(new BButton("Cancel", new BMessage(B_QUIT_REQUESTED))).Add(save).End();
    save->MakeDefault(true);
    AddShortcut(B_ESCAPE, B_NO_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
    name->MakeFocus();
    name->TextView()->SelectAll();
    CenterIn(window.Frame());
    MoveOnScreen();
  }
  void MessageReceived(BMessage *m) override {
    if (m->what == kRaise) {
      Raise(*this);
      return;
    }
    if (m->what != kSaveName) {
      BWindow::MessageReceived(m);
      return;
    }
    std::string value(name->Text());
    auto first = value.find_first_not_of(" \t");
    if (first == std::string::npos ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
      name->SetLabel("Enter a name:");
      name->MakeFocus();
      return;
    }
    value = value.substr(first, value.find_last_not_of(" \t") - first + 1);
    BMessage apply(kApplyName);
    apply.AddString("alias", value.c_str());
    owner.SendMessage(&apply);
    PostMessage(B_QUIT_REQUESTED);
  }
};

class Overview : public BWindow {
  BListView *list;
  WrappedLabel *location;
  BButton *show, *rename, *reset;
  std::vector<WindowInfo> rows;
  std::unique_ptr<BMessageRunner> timer;
  uint64 displayed = 0;
  int64 initial;
  const WindowInfo *Selected() const {
    auto index = list->CurrentSelection();
    return index >= 0 && size_t(index) < rows.size() ? &rows[index] : nullptr;
  }
  void Select() {
    auto *selected = Selected();
    location->SetMessage(selected ? selected->location.c_str() : "");
    reset->SetEnabled(selected && selected->named);
    for (auto *button : {show, rename})
      button->SetEnabled(selected != nullptr);
  }
  void Refresh() {
    int64 wanted = Selected() ? Selected()->id : initial;
    std::vector<WindowInfo> snapshot;
    {
      std::lock_guard<std::mutex> guard(registryLock);
      if (displayed == revision)
        return;
      for (const auto &item : windows)
        snapshot.push_back(item.second);
      displayed = revision;
    }
    rows.swap(snapshot);
    while (auto *item = list->RemoveItem(int32(0)))
      delete item;
    for (size_t i = 0; i < rows.size(); ++i) {
      const auto &row = rows[i];
      auto label = "#" + std::to_string(row.id) + "  " + row.title + "  ·  " + row.kind + "  ·  " + row.location;
      if (!row.setName.empty())
        label += "  ·  Set: " + row.setName;
      list->AddItem(new BStringItem(label.c_str()));
      if (row.id == wanted)
        list->Select(i);
    }
    if (list->CurrentSelection() < 0 && !rows.empty())
      list->Select(0);
    Select();
  }
public:
  explicit Overview(WorkingWindow &owner)
      : BWindow(BRect(0, 0, 540, 300), "Open windows", B_TITLED_WINDOW,
                B_AUTO_UPDATE_SIZE_LIMITS), initial(owner.WindowId()) {
    list = new BListView("open windows");
    list->SetSelectionMessage(new BMessage(kSelectWindow));
    list->SetInvocationMessage(new BMessage(kShowWindow));
    location = new WrappedLabel("window location", "");
    rename = new BButton("Name…", new BMessage(kNameWindow));
    reset = new BButton("Use automatic name", new BMessage(kUseFolderName));
    show = new BButton("Activate", new BMessage(kShowWindow));
    auto *scroll = new BScrollView("window list", list, 0, false, true);
    scroll->SetExplicitMinSize(BSize(260, 90));
    BLayoutBuilder::Group<>(this, B_VERTICAL)
        .SetInsets(B_USE_WINDOW_SPACING).Add(scroll).Add(location, 0)
        .AddGroup(B_HORIZONTAL).Add(rename).Add(reset).AddGlue().Add(show).End();
    list->SetTarget(this);
    AddShortcut(B_ESCAPE, B_NO_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
    show->MakeDefault(true);
    CenterIn(owner.Frame()); MoveOnScreen();
    Refresh();
    BMessage tick(kRefreshWindows);
    timer = std::make_unique<BMessageRunner>(BMessenger(this), &tick, 250000);
  }
  void MessageReceived(BMessage *m) override {
    if (m->what == kRaise) { Refresh(); Raise(*this); return; }
    if (m->what == kRefreshWindows) { Refresh(); return; }
    if (m->what == kSelectWindow) { Select(); return; }
    if (m->what == kNameWindow || m->what == kUseFolderName || m->what == kShowWindow) {
      // Never retarget a stale selection to the next row when a window closes.
      auto *row = Selected();
      if (row) {
        BMessage command(m->what == kShowWindow ? kRaise : m->what);
        if (row->target.SendMessage(&command, static_cast<BHandler *>(nullptr), 50000) == B_OK &&
            m->what == kShowWindow)
          PostMessage(B_QUIT_REQUESTED);
      }
      Refresh();
      return;
    }
    BWindow::MessageReceived(m);
  }
};
}

WorkingWindow::WorkingWindow(BRect frame, const char *title, const char *type, uint32 flags)
    : BWindow(frame, title, B_TITLED_WINDOW, flags), automatic(title), kind(type), id(++nextId) {
  Publish();
}
WorkingWindow::~WorkingWindow() {
  RetireReadWorker(copyProbe);
  if (nameDialog.IsValid()) {
    BMessage close(B_QUIT_REQUESTED);
    nameDialog.SendMessage(&close, static_cast<BHandler *>(nullptr), 0);
  }
  std::lock_guard<std::mutex> guard(registryLock);
  windows.erase(id);
  ++revision;
}
void WorkingWindow::Describe(const std::string &title, const std::string &path, bool dirty) {
  if (automatic == title && location == path && modified == dirty)
    return;
  automatic = title;
  location = path;
  modified = dirty;
  Publish();
}
void WorkingWindow::SetCopyFolder(const std::string &path, const NodeIdentity &node) {
  if (copyFolder != path || copyDevice != node.device || copyInode != node.inode) {
    copyFolder = path;
    copyDevice = node.device;
    copyInode = node.inode;
    ++copyFolderRevision;
    Publish();
  }
}

namespace {
void CheckCopyWindow(int64 id, int64 revision) {
  std::lock_guard<std::mutex> guard(registryLock);
  auto found = windows.find(id);
  if (found == windows.end() || found->second.copyRevision != revision)
    throw std::runtime_error("The destination window closed or changed folders. Choose again.");
}

class CopyFolderProbe : public ReadWorker {
  BMessenger target;
  Job job;
  int64 request, window, revision;
public:
  CopyFolderProbe(BMessenger target, Job job, int64 request, int64 window, int64 revision)
      : ReadWorker("copy destination reader"), target(target), job(std::move(job)),
        request(request), window(window), revision(revision) {}
  void MessageReceived(BMessage *message) override {
    if (message->what != kProbeCopyFolder) {
      ReadWorker::MessageReceived(message);
      return;
    }
    BMessage result(kCopyFolderReady);
    try {
      CheckCopyWindow(window, revision);
      const auto &folder = job.destination;
      BVolume volume(job.expectedDestination.device);
      if (volume.InitCheck() != B_OK || volume.IsShared() || volume.IsReadOnly() ||
          access(folder.c_str(), W_OK | X_OK))
        throw std::runtime_error("The destination folder is no longer writable.");
      result = EncodeJob(PrepareTransfer(std::move(job)), kCopyFolderReady);
      CheckCopyWindow(window, revision);
    } catch (const std::exception &error) {
      result.AddString("error", error.what());
    }
    result.AddInt64("request", request);
    result.AddInt64("window", window);
    result.AddInt64("revision", revision);
    target.SendMessage(&result, static_cast<BHandler *>(nullptr), 50000);
  }
};
}

void WorkingWindow::PickCopyFolder(Job job) {
  ++copyRequest;
  pendingCopy.reset();
  RetireReadWorker(copyProbe);
  std::vector<WindowInfo> snapshot;
  {
    std::lock_guard<std::mutex> guard(registryLock);
    for (const auto &item : windows)
      if (item.first != id && !item.second.copyFolder.empty() &&
          item.second.copyNode.inode >= 0)
        snapshot.push_back(item.second);
  }
  if (snapshot.empty())
    throw std::runtime_error("Open another window on a local folder first.");
  auto *choices = new BPopUpMenu("Copy to window", false, false);
  for (const auto &row : snapshot) {
    auto label = row.title + "  ·  #" + std::to_string(row.id) + "  ·  " + row.copyFolder;
    auto *choice = new BMessage(kProbeCopyFolder);
    choice->AddInt64("request", copyRequest);
    choice->AddInt64("window", row.id);
    choice->AddInt64("revision", row.copyRevision);
    auto *item = new BMenuItem(label.c_str(), choice);
    choices->AddItem(item);
  }
  choices->SetTargetForItems(BMessenger(this));
  choices->SetAsyncAutoDestruct(true);
  pendingCopy = std::make_unique<Job>(std::move(job));
  choices->Go(BPoint(Frame().left + 24, Frame().top + 36), true, true, true);
}

bool WorkingWindow::HandleCopyFolder(BMessage &message, Job &job) {
  if (message.what != kProbeCopyFolder && message.what != kCopyFolderReady)
    return false;
  if (message.IsSourceRemote() || message.GetInt64("request", -1) != copyRequest)
    return true;
  const int64 window = message.GetInt64("window", -1);
  const int64 revision = message.GetInt64("revision", -1);
  if (message.what == kProbeCopyFolder) {
    if (!pendingCopy)
      return true;
    auto selected = std::move(pendingCopy);
    {
      std::lock_guard<std::mutex> guard(registryLock);
      auto found = windows.find(window);
      if (found == windows.end() || found->second.copyRevision != revision)
        throw std::runtime_error("The destination window closed or changed folders. Choose again.");
      selected->destination = found->second.copyFolder;
      selected->expectedDestination = found->second.copyNode;
    }
    copyProbe = new CopyFolderProbe(BMessenger(this), std::move(*selected),
                                    copyRequest, window, revision);
    if (!StartReadWorker(copyProbe) || copyProbe->PostMessage(kProbeCopyFolder) != B_OK) {
      RetireReadWorker(copyProbe);
      throw std::runtime_error("Cannot start destination reader. Try again.");
    }
  } else if (copyProbe) {
    RetireReadWorker(copyProbe);
    const char *error;
    if (message.FindString("error", &error) == B_OK)
      throw std::runtime_error(error);
    CheckCopyWindow(window, revision);
    job = DecodeJob(message);
    job.checkDestinationWindow = [window, revision] { CheckCopyWindow(window, revision); };
  }
  return true;
}

void WorkingWindow::Publish() {
  std::string title = alias.empty() ? automatic : alias + " · Listi";
  if (modified)
    title += " *";
  if (title != Title())
    SetTitle(title.c_str());
  std::lock_guard<std::mutex> guard(registryLock);
  windows[id] = {id, BMessenger(this), title, kind, location, !alias.empty(), setName, copyFolder,
                 {copyDevice, copyInode}, copyFolderRevision};
  ++revision;
}
bool WorkingWindow::HandleWindowAction(BMessage *m) {
  switch (m->what) {
  case kRaise:
    Raise(*this);
    return true;
  case kNameWindow:
    if (!RaiseExisting(nameDialog)) {
      auto *dialog = new NameDialog(*this, alias);
      nameDialog = BMessenger(dialog);
      dialog->Show();
    }
    return true;
  case kApplyName:
    alias = m->GetString("alias", "");
    Publish();
    return true;
  case kUseFolderName:
    alias.clear();
    Publish();
    return true;
  default:
    return false;
  }
}
void WorkingWindow::MessageReceived(BMessage *m) {
  if (!HandleWindowAction(m))
    BWindow::MessageReceived(m);
}
std::vector<BrowserChoice> WorkingSetChoices() {
  std::lock_guard<std::mutex> guard(registryLock);
  std::vector<BrowserChoice> choices;
  for (const auto &entry : windows) {
    const auto &w = entry.second;
    if (w.kind == "File manager")
      choices.push_back({"#" + std::to_string(w.id) + "  " + w.title + "  ·  " + w.location,
                         w.target, w.id});
  }
  return choices;
}
BMessenger OpenWindowOverview(WorkingWindow &owner, BMessenger previous) {
  if (RaiseExisting(previous))
    return previous;
  auto *overview = new Overview(owner);
  BMessenger messenger(overview);
  overview->Show();
  return messenger;
}
}
