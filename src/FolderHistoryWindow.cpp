// SPDX-License-Identifier: MIT
#include "FolderHistory.h"

#include <Button.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <Messenger.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>
#include <ctime>
#include <cstdlib>
#include <stdexcept>
#include <mutex>
#include <sys/stat.h>
#include <thread>

namespace Listi {
namespace {
constexpr uint32 kConnect = 'fhcn', kSave = 'fhsv', kBrowse = 'fhbr',
    kRestore = 'fhrs', kCancel = 'fhca', kDone = 'fhdn';
std::atomic_int activeOperations{0};

struct Operation {
  std::atomic_bool canceled{false};
  std::mutex lock;
  std::shared_ptr<HistoryStore> connectedStore;
};

std::string PointLabel(const std::string &id) {
  time_t seconds = std::strtoll(id.c_str() + 6, nullptr, 10) / 1000000;
  tm local{};
  char text[64];
  if (localtime_r(&seconds, &local) &&
      strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local))
    return std::string(text) + "  ·  " + id.substr(6);
  return id;
}

std::string DisplayName(std::string name) {
  for (char &character : name)
    if (character >= 0 && character < ' ') character = '?';
  return name;
}

void Describe(const BMessage &folder, const std::string &prefix, std::string &text) {
  BMessage child;
  for (int32 i = 0; text.size() < 256 * 1024 &&
       folder.FindMessage("child", i, &child) == B_OK; ++i) {
    auto path = prefix + DisplayName(child.GetString("name", ""));
    const auto mode = child.GetInt32("mode", 0);
    if (S_ISDIR(mode)) {
      text += path + "/\n";
      Describe(child, path + "/", text);
    } else if (S_ISLNK(mode)) {
      text += path + " → " + DisplayName(child.GetString("target", "")) + "\n";
    } else {
      text += path + "\n";
    }
  }
}

class PointItem : public BStringItem {
public:
  explicit PointItem(std::string value)
      : BStringItem(PointLabel(value).c_str()), id(std::move(value)) {}
  std::string id;
};

class HistoryWindow : public BWindow {
public:
  explicit HistoryWindow(const char *root)
      : BWindow(BRect(110, 88, 920, 620), "Folder history", B_TITLED_WINDOW,
                B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS),
        source(root) {
    auto *label = new BStringView("source", ("Source: " + source).c_str());
    folder = new BTextControl("store", "History folder:", "", nullptr);
    destination = new BTextControl("destination", "New copy path:", "", nullptr);
    connect = new BButton("connect", "Connect", new BMessage(kConnect));
    save = new BButton("save", "Create point", new BMessage(kSave));
    restore = new BButton("restore", "Restore as copy", new BMessage(kRestore));
    cancel = new BButton("cancel", "Cancel operation", new BMessage(kCancel));
    points = new BListView("points");
    points->SetSelectionMessage(new BMessage(kBrowse));
    contents = new BTextView("contents");
    contents->MakeEditable(false);
    status = new BStringView("status", "Enter an existing, dedicated folder outside the source.");
    status->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    status->SetTruncation(B_TRUNCATE_END);
    auto *pointScroll = new BScrollView("point scroll", points, 0, false, true);
    auto *contentScroll = new BScrollView("content scroll", contents, 0, false, true);
    pointScroll->SetExplicitMinSize(BSize(280, 230));
    contentScroll->SetExplicitMinSize(BSize(320, 230));

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_SPACING)
        .Add(label)
        .AddGroup(B_HORIZONTAL).Add(folder).Add(connect).End()
        .AddGroup(B_HORIZONTAL).Add(save).Add(cancel).AddGlue().End()
        .AddGroup(B_HORIZONTAL)
          .AddGroup(B_VERTICAL).Add(new BStringView("points label", "Saved points")).Add(pointScroll).End()
          .AddGroup(B_VERTICAL).Add(new BStringView("contents label", "Contents (read-only)")).Add(contentScroll).End()
        .End()
        .AddGroup(B_HORIZONTAL).Add(destination).Add(restore).End()
        .Add(status);
    UpdateControls();
    folder->MakeFocus();
  }

  ~HistoryWindow() override { ClearPoints(); }

  bool QuitRequested() override {
    if (!operation) return true;
    status->SetText("Finish or cancel the current operation before closing.");
    return false;
  }

  void MessageReceived(BMessage *message) override {
    switch (message->what) {
    case kConnect: case kSave: case kBrowse: case kRestore:
      Start(message->what);
      break;
    case kCancel:
      if (operation) {
        operation->canceled = true;
        cancel->SetEnabled(false);
        status->SetText("Cancel requested…");
      }
      break;
    case kDone: {
      if (!operation) break;
      {
        const std::lock_guard<std::mutex> lock(operation->lock);
        if (operation->connectedStore) store = std::move(operation->connectedStore);
      }
      operation.reset();
      --activeOperations;
      status->SetText(message->GetString("status", ""));
      if (message->HasBool("replace")) {
        loadedPoint.clear();
        ClearPoints();
        const char *point;
        for (int32 i = 0; message->FindString("point", i, &point) == B_OK; ++i)
          points->AddItem(new PointItem(point));
        contents->SetText("");
      }
      if (const char *viewed = message->GetString("viewed", nullptr)) {
        if (SelectedPoint() != viewed) {
          Start(kBrowse);
          break;
        }
        if (const char *listing = message->GetString("listing", nullptr))
          contents->SetText(listing);
        loadedPoint = message->HasString("listing") ? viewed : "";
      }
      if (!message->HasString("viewed") && !message->HasBool("replace") &&
          SelectedPoint() != loadedPoint) {
        Start(kBrowse);
        break;
      }
      UpdateControls();
      break;
    }
    default:
      BWindow::MessageReceived(message);
    }
  }

private:
  void ClearPoints() {
    while (auto *point = points->RemoveItem(int32(0))) delete point;
  }

  std::string SelectedPoint() const {
    auto *item = static_cast<PointItem *>(points->ItemAt(points->CurrentSelection()));
    return item ? item->id : "";
  }

  void UpdateControls() {
    const bool idle = !operation;
    folder->SetEnabled(idle && !store);
    connect->SetEnabled(idle && !store);
    save->SetEnabled(idle && bool(store));
    destination->SetEnabled(idle && bool(store));
    restore->SetEnabled(idle && !loadedPoint.empty() && SelectedPoint() == loadedPoint);
    cancel->SetEnabled(!idle);
  }

  void Start(uint32 action) {
    if (operation || (action == kConnect ? bool(store) : !store)) return;
    std::string point = SelectedPoint();
    if (action == kRestore && point != loadedPoint) return;
    if ((action == kBrowse || action == kRestore) && point.empty()) {
      contents->SetText("");
      UpdateControls();
      return;
    }
    if (action == kBrowse) {
      contents->SetText("");
      loadedPoint.clear();
    }
    operation = std::make_shared<Operation>();
    ++activeOperations;
    status->SetText(action == kSave ? "Creating history point…" :
        action == kRestore ? "Restoring copy…" : "Reading history…");
    UpdateControls();
    try {
      std::thread([action, point, source = source, folder = std::string(folder->Text()),
          destination = std::string(destination->Text()), connected = store,
          operation = operation, reply = BMessenger(this)] {
        BMessage result(kDone);
        std::string status;
        if (action == kBrowse) result.AddString("viewed", point.c_str());
        try {
          if (action == kConnect || action == kSave) {
            auto history = action == kConnect ? std::make_shared<HistoryStore>(source, folder) : connected;
            if (action == kSave) history->Save(operation->canceled);
            result = history->Points();
            result.what = kDone;
            result.AddBool("replace", true);
            if (action == kConnect) {
              if (operation->canceled) throw std::runtime_error("Canceled.");
              const std::lock_guard<std::mutex> lock(operation->lock);
              operation->connectedStore = std::move(history);
            }
            status = action == kConnect ? "History folder connected." : "History point saved.";
          } else if (action == kBrowse) {
            auto manifest = connected->Browse(point);
            std::string listing;
            Describe(manifest, "", listing);
            if (operation->canceled) throw std::runtime_error("Canceled.");
            if (listing.empty()) listing = "(Empty folder)";
            if (listing.size() >= 256 * 1024) listing += "\n(Listing truncated; restore uses the full point.)";
            result.AddString("listing", listing.c_str());
            status = "Viewing saved history.";
          } else {
            connected->Restore(point, destination, operation->canceled);
            status = "Copy restored to " + destination;
          }
        } catch (const std::exception &error) {
          status = error.what();
        }
        result.AddString("status", status.c_str());
        reply.SendMessage(&result);
      }).detach();
    } catch (const std::exception &error) {
      operation.reset();
      --activeOperations;
      status->SetText(error.what());
      UpdateControls();
    }
  }

  const std::string source;
  std::shared_ptr<HistoryStore> store;
  std::shared_ptr<Operation> operation;
  std::string loadedPoint;
  BTextControl *folder, *destination;
  BButton *connect, *save, *restore, *cancel;
  BListView *points;
  BTextView *contents;
  BStringView *status;
};
}

bool FolderHistoryBusy() { return activeOperations != 0; }

void OpenFolderHistory(const char *source) { (new HistoryWindow(source))->Show(); }
}
