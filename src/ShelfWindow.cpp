// SPDX-License-Identifier: MIT
#include "ShelfWindow.h"
#include "ArchiveWindow.h"
#include "FileClipboard.h"
#include "InlineAction.h"
#include "JobSnapshot.h"
#include "Shelf.h"
#include "ShelfCollections.h"
#include "VirtualFile.h"
#include <Alert.h>
#include <Button.h>
#include "AdaptiveColumnList.h"
#include <ColumnTypes.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <Path.h>
#include <StringView.h>
#include <Window.h>
#include <algorithm>
#include <stdexcept>
#include <thread>

namespace Listi {
namespace {
constexpr uint32 kRemove = 'sfrm', kSelect = 'sfsl', kPickMove = 'sfbw',
                 kPickCopy = 'sfcp', kFolderChosen = 'sfdc', kDropped = 'sfdp',
                 kCreateArchive = 'sfar', kClear = 'sfcl',
                 kSaveCollection = 'sfsc', kCollectionNamed = 'sfcn',
                 kLoadCollection = 'sflc', kDeleteCollection = 'sfxc',
                 kPickRelink = 'sfrl', kRelinkChosen = 'sfrc',
                 kResolveFinished = 'sfrx';

std::string CollectionsFile() {
  BPath path;
  if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK ||
      path.Append("Lister_collections") != B_OK)
    return {};
  return path.Path();
}

std::string RefPath(const entry_ref &ref, bool traverse) {
  BEntry entry(&ref, traverse);
  BPath path;
  if (entry.GetPath(&path) != B_OK)
    throw std::runtime_error("The selected entry is no longer available.");
  return path.Path();
}

} // namespace

size_t AddShelfDrop(Shelf &shelf, const BMessage &drop) {
  type_code type;
  int32 count;
  if (drop.GetInfo("refs", &type, &count) != B_OK || type != B_REF_TYPE ||
      count < 1 || size_t(count) > Shelf::kMaximumEntries ||
      drop.FlattenedSize() > 2 * 1024 * 1024)
    throw std::runtime_error("Drop between 1 and 1024 local entries.");
  std::vector<std::string> paths;
  for (int32 i = 0; i < count; ++i) {
    entry_ref ref;
    if (drop.FindRef("refs", i, &ref) != B_OK)
      throw std::runtime_error("A dropped entry is unavailable.");
    paths.push_back(RefPath(ref, false));
  }
  // Listi drags carry the identities from drag start; Tracker drops have refs
  // only. Capture Tracker identities here, but never refresh Listi's snapshot.
  BMessage selection;
  std::map<std::string, NodeIdentity> expected;
  if (drop.FindMessage("listi:drag-selection", &selection) == B_OK) {
    for (const auto &item : ReadFileClipboard(selection))
      expected.emplace(item.path, item.node);
    std::set<std::string> unique(paths.begin(), paths.end());
    if (expected.size() != paths.size() || unique.size() != paths.size())
      throw std::runtime_error(
          "The dragged selection does not match its references.");
  }
  return shelf.Add(paths, expected);
}

namespace {
const char *StateLabel(ShelfState state) {
  switch (state) {
  case ShelfState::Missing:
    return "Unavailable";
  case ShelfState::Replaced:
    return "Replaced";
  case ShelfState::Ambiguous:
    return "Needs relink";
  default:
    return "Ready";
  }
}

class ShelfRow : public BRow {
  ShelfState state;

public:
  std::string path;
  explicit ShelfRow(const ShelfEntry &entry)
      : state(entry.DisplayState()), path(entry.path) {
    const auto slash = path.find_last_of('/');
    SetField(new BStringField(path.substr(slash + 1).c_str()), 0);
    SetField(new BStringField(path.substr(0, slash ? slash : 1).c_str()), 1);
    SetField(new BStringField(StateLabel(state)), 2);
  }

  bool UpdateState(ShelfState current) {
    if (current == state)
      return false;
    state = current;
    static_cast<BStringField *>(GetField(2))->SetString(StateLabel(state));
    return true;
  }
};

class ShelfList : public Listi::AdaptiveColumnList {
public:
  ShelfList()
      : AdaptiveColumnList("shelf entries", B_WILL_DRAW | B_NAVIGABLE,
                        B_FANCY_BORDER) {}

  void MessageDropped(BMessage *message, BPoint) override {
    BMessage drop(*message);
    drop.what = kDropped;
    Window()->PostMessage(&drop);
  }
};

class ShelfWindow : public BWindow {
  BMessenger owner;
  std::shared_ptr<Shelf> shelf;
  ShelfList *rows;
  BButton *remove, *relink;
  BStringView *status;
  InlineAction *question;
  BMenu *loadCollections, *deleteCollections;
  BMenuItem *clear, *copy, *move, *archive, *saveCollection;
  const std::string collectionsFile;
  std::map<std::string, ShelfRow *> displayed;
  ShelfSnapshot rendered;
  std::unique_ptr<BFilePanel> picker;
  BMessenger archiveWindow;
  Action pickerAction = Action::Move;
  size_t pickerRevision = 0;
  int64 pickerSerial = 0;
  std::string relinking;
  bool resolving = false;

  void Status(const std::string &text) {
    status->SetText(text.c_str());
    status->SetToolTip(text.c_str());
  }

  std::set<std::string> Selected() const {
    std::set<std::string> selected;
    for (auto row = rows->CurrentSelection(); row;
         row = rows->CurrentSelection(row))
      selected.insert(static_cast<ShelfRow *>(row)->path);
    return selected;
  }

  void UpdateSelection(const ShelfSnapshot &snapshot) {
    const auto selected = Selected();
    remove->SetEnabled(!selected.empty());
    relink->SetEnabled(selected.size() == 1 &&
        std::any_of(snapshot.entries.begin(), snapshot.entries.end(),
                    [&](const auto &entry) {
                      return entry.path == *selected.begin() &&
                             entry.DisplayState() != ShelfState::Ready;
                    }));
  }

  void ShelfChanged() {
    BMessage changed(kShelfChanged);
    owner.SendMessage(&changed, (BHandler *)nullptr, 0);
  }

  void Render() {
    auto snapshot = shelf->Snapshot();
    const auto selected = Selected();
    const auto retained = snapshot.RetainSelection(rendered, selected);
    if (retained != selected)
      rows->DeselectAll();
    std::set<std::string> present;
    bool layoutChanged = false;
    for (const auto &entry : snapshot.entries) {
      present.insert(entry.path);
      auto existing = displayed.find(entry.path);
      if (existing != displayed.end()) {
        if (existing->second->UpdateState(entry.DisplayState()))
          rows->UpdateRow(existing->second);
      } else {
        auto row = new ShelfRow(entry);
        displayed.emplace(entry.path, row);
        rows->AddRow(row);
        layoutChanged = true;
      }
    }
    // Remove through the control so it adjusts its focus and selection pointers.
    for (auto it = displayed.begin(); it != displayed.end();) {
      if (present.count(it->first)) {
        ++it;
      } else {
        rows->RemoveRow(it->second);
        layoutChanged = true;
        delete it->second;
        it = displayed.erase(it);
      }
    }
    if (layoutChanged)
      rows->InvalidateLayout();
    if (retained != selected) {
      for (const auto &path : retained)
        rows->AddToSelection(displayed.at(path));
      if (retained.size() == 1)
        rows->SetFocusRow(displayed.at(*retained.begin()), false);
    }
    auto removeMessage = new BMessage(kRemove);
    removeMessage->AddInt64("revision", snapshot.revision);
    remove->SetMessage(removeMessage);
    UpdateSelection(snapshot);
    clear->SetEnabled(!snapshot.entries.empty());
    copy->SetEnabled(!snapshot.entries.empty());
    move->SetEnabled(!snapshot.entries.empty());
    archive->SetEnabled(!snapshot.entries.empty());
    saveCollection->SetEnabled(!snapshot.entries.empty());
    rendered = std::move(snapshot);
  }

  void RenderCollections(const ShelfCollections &collections) {
    for (auto menu : {loadCollections, deleteCollections}) {
      while (auto item = menu->RemoveItem(int32(0)))
        delete item;
      for (const auto &collection : collections.Items()) {
        auto message = new BMessage(menu == loadCollections
                                        ? kLoadCollection : kDeleteCollection);
        message->AddString("collection", collection.name.c_str());
        auto item = new BMenuItem(collection.name.c_str(), message);
        menu->AddItem(item);
        item->SetTarget(this);
      }
      menu->SetEnabled(menu->CountItems() != 0);
    }
  }

  void RefreshCollections() {
    try {
      RenderCollections(ShelfCollections::Load(collectionsFile));
    } catch (const std::exception &e) {
      RenderCollections({});
      Status(e.what());
    }
  }

  bool Confirm(const char *title, const std::string &text,
                const char *accept) {
    auto alert = new BAlert(title, text.c_str(), "Cancel", accept, nullptr,
                            B_WIDTH_AS_USUAL, B_WARNING_ALERT);
    alert->SetDefaultButton(alert->ButtonAt(0));
    alert->SetShortcut(0, B_ESCAPE);
    return alert->Go() == 1;
  }

  void AskCollectionName() {
    const auto snapshot = shelf->Snapshot();
    if (snapshot.entries.empty())
      throw std::runtime_error("Add entries to the Shelf first.");
    BMessage named(kCollectionNamed);
    named.AddBool("placeLabel", true);
    named.AddInt64("revision", snapshot.revision);
    question->Ask("Name this saved set of Shelf references.", "Save", named,
                  "");
  }

  void SaveCollection(const BMessage &message) {
    const auto snapshot = shelf->Snapshot();
    if (snapshot.revision != size_t(message.GetInt64("revision", -1)))
      throw std::runtime_error("The Shelf changed. Start the save again.");
    const auto name = ShelfCollections::Name(message.GetString("name", ""));
    const auto collections = ShelfCollections::Load(collectionsFile);
    const auto *existing = collections.Find(name);
    if (existing &&
        !Confirm("Replace collection",
                 "Replace the saved references in “" + name + "”?",
                 "Replace"))
      return;
    if (shelf->Snapshot().revision != snapshot.revision)
      throw std::runtime_error("The Shelf changed. Start the save again.");
    RenderCollections(
        ShelfCollections::SaveCurrent(collectionsFile, name, snapshot, existing));
    Status("Saved collection “" + name + "”. Original files were not changed.");
  }

  void LoadCollection(const std::string &name) {
    if (resolving)
      throw std::runtime_error("Wait for the Shelf refresh to finish.");
    const auto collections = ShelfCollections::Load(collectionsFile);
    const auto item = collections.Find(name);
    if (!item)
      throw std::runtime_error("That saved collection no longer exists.");
    const auto snapshot = shelf->Snapshot();
    if (!snapshot.entries.empty() &&
        !Confirm("Load collection",
                 "Replace the current Shelf with “" + name +
                     "”? This does not change files or saved collections.",
                 "Load"))
      return;
    BeginResolve(item->entries, snapshot.revision, name);
  }

  void BeginResolve(std::vector<ShelfEntry> entries, size_t revision,
                    std::string name) {
    if (resolving)
      return;
    if (name.empty()) {
      const auto snapshot = shelf->Snapshot();
      if (std::none_of(snapshot.entries.begin(), snapshot.entries.end(),
                       [](const auto &entry) { return entry.restored; }))
        return;
    }
    const BMessenger target(this);
    std::thread worker([target, working = shelf, entries = std::move(entries),
                        revision, name = std::move(name)]() mutable {
      BMessage done(kResolveFinished);
      try {
        if (name.empty()) {
          working->RefreshSavedReferences();
        } else {
          working->Load(std::move(entries), revision);
          done.AddString("loaded", name.c_str());
        }
      } catch (const std::exception &e) {
        done.AddString("error", e.what());
      }
      target.SendMessage(&done);
    });
    worker.detach();
    resolving = true;
    Status("Checking saved references...");
  }

  void DeleteCollection(const std::string &name) {
    const auto collections = ShelfCollections::Load(collectionsFile);
    const auto *existing = collections.Find(name);
    if (!existing)
      throw std::runtime_error("That saved collection no longer exists.");
    if (!Confirm("Delete collection",
                 "Delete saved collection “" + name +
                     "”? This does not change original files or the current Shelf.",
                 "Delete"))
      return;
    RenderCollections(ShelfCollections::DeleteSaved(collectionsFile, *existing));
    Status("Deleted saved collection “" + name + "”.");
  }

  void Add(const BMessage &message) {
    const auto added = AddShelfDrop(*shelf, message);
    Render();
    ShelfChanged();
    Status(added ? "Added " + std::to_string(added) + " to the Shelf."
                 : "Already on the Shelf.");
  }

  void PickFolder(Action action) {
    if (picker) {
      picker->Show();
      return;
    }
    const auto snapshot = shelf->Snapshot();
    CheckShelfSources(snapshot, action);
    pickerAction = action;
    relinking.clear();
    pickerRevision = snapshot.revision;
    BMessenger here(this);
    BMessage chosen(kFolderChosen);
    chosen.AddInt64("shelf picker", ++pickerSerial);
    picker = std::make_unique<BFilePanel>(B_OPEN_PANEL, &here, nullptr,
                                          B_DIRECTORY_NODE, false, &chosen);
    const bool copying = action == Action::Copy;
    picker->SetButtonLabel(B_DEFAULT_BUTTON,
                           copying ? "Copy" : "Choose folder");
    picker->Window()->SetTitle(copying ? "Listi: Copy Shelf to folder"
                                       : "Listi: Move Shelf to folder");
    picker->Show();
  }

  void ConfirmFolder(const BMessage &message) {
    if (!picker || message.GetInt64("shelf picker", -1) != pickerSerial)
      return;
    picker.reset();
    const auto snapshot = shelf->Snapshot();
    if (snapshot.revision != pickerRevision)
      throw std::runtime_error(
          "The Shelf changed. Choose a destination again.");
    entry_ref ref;
    if (message.FindRef("refs", &ref) != B_OK)
      throw std::runtime_error("Choose a destination folder.");
    auto job = PrepareShelfTransfer(snapshot, pickerAction, RefPath(ref, true));
    if (job.action == Action::Move) {
      const auto question = "Move all " + std::to_string(job.sources.size()) +
                            " Shelf entries to this folder?\n\n" +
                            job.destination;
      if (!Confirm("Move Shelf entries", question, "Move"))
        return;
      if (shelf->Snapshot().revision != snapshot.revision)
        throw std::runtime_error(
            "The Shelf changed. Choose a destination again.");
      CheckShelfSources(snapshot, job.action);
    }
    // Queue validation checks the same source and destination identities again.
    auto request = EncodeJob(job, kSubmitShelfTransfer);
    const std::string action = job.action == Action::Move ? "Move" : "Copy";
    if (owner.SendMessage(&request, (BHandler *)nullptr, 0) != B_OK)
      throw std::runtime_error(
          "Could not submit the transfer. The Shelf is unchanged.");
    Status(action + " submitted. See Operations for progress.");
  }

  void PickRelink() {
    if (picker) {
      picker->Show();
      return;
    }
    const auto selected = Selected();
    if (selected.size() != 1)
      throw std::runtime_error("Select one unavailable reference to relink.");
    const auto snapshot = shelf->Snapshot();
    if (snapshot.revision != rendered.revision)
      throw std::runtime_error("The Shelf changed. Select the reference again.");
    relinking = *selected.begin();
    pickerRevision = rendered.revision;
    BMessenger here(this);
    BMessage chosen(kRelinkChosen);
    chosen.AddInt64("shelf picker", ++pickerSerial);
    picker = std::make_unique<BFilePanel>(
        B_OPEN_PANEL, &here, nullptr,
        B_FILE_NODE | B_DIRECTORY_NODE | B_SYMLINK_NODE, false, &chosen);
    picker->SetButtonLabel(B_DEFAULT_BUTTON, "Relink");
    const auto title = "Listi: Relink " + LocationLeaf(relinking);
    picker->Window()->SetTitle(title.c_str());
    picker->Show();
  }

  void ConfirmRelink(const BMessage &message) {
    if (!picker || message.GetInt64("shelf picker", -1) != pickerSerial)
      return;
    picker.reset();
    entry_ref ref;
    if (message.FindRef("refs", &ref) != B_OK)
      throw std::runtime_error("Choose a replacement reference.");
    if (shelf->Snapshot().revision != pickerRevision)
      throw std::runtime_error("The Shelf changed. Choose the reference again.");
    const auto chosen = RefPath(ref, false);
    BEntry candidate(&ref, false);
    struct stat st;
    if (candidate.GetStat(&st) != B_OK)
      throw std::runtime_error("The chosen entry is no longer available.");
    if (!Confirm("Relink Shelf reference",
                 "Relink this reference?\n\nFrom: " + relinking +
                     "\nTo: " + chosen +
                     "\n\nThis does not move, rename or change any file.",
                 "Relink"))
      return;
    shelf->Relink(relinking, chosen, pickerRevision,
                  NodeIdentity{int32(st.st_dev), int64(st.st_ino)},
                  ShelfNodeStamp{int64(st.st_ino), int64(st.st_crtim.tv_sec),
                                 int32(st.st_crtim.tv_nsec)});
    Render();
    ShelfChanged();
    Status("Reference relinked. Save the collection to keep this change.");
  }

  void CreateArchive() {
    if (archiveWindow.SendMessage('arav') == B_OK)
      return;
    const auto snapshot = shelf->Snapshot();
    auto job = PrepareShelfArchive(snapshot);
    ArchiveWindowOptions options;
    options.suggestedName = "Shelf";
    options.appendExtension = true;
    options.validate = [collection = shelf, revision = snapshot.revision] {
      if (collection->Snapshot().revision != revision)
        throw std::runtime_error(
            "The Shelf changed. Close this window and choose Create archive again.");
    };
    archiveWindow = OpenArchiveWindow(job, ParentLocation(job.sources.front()),
                                      owner, std::move(options));
  }

public:
  ShelfWindow(BMessenger receiver, std::shared_ptr<Shelf> collection,
              const std::string &settingsFile)
      : BWindow(BRect(150, 115, 780, 455), "Listi: Shelf", B_TITLED_WINDOW,
                B_AUTO_UPDATE_SIZE_LIMITS),
        owner(receiver), shelf(std::move(collection)),
        collectionsFile(settingsFile.empty() ? CollectionsFile() : settingsFile) {
    auto menu = new BMenuBar("shelf menu");
    auto actions = new BMenu("Actions");
    copy = new BMenuItem("Copy all...", new BMessage(kPickCopy));
    move = new BMenuItem("Move all...", new BMessage(kPickMove));
    archive = new BMenuItem("Create archive...", new BMessage(kCreateArchive));
    clear = new BMenuItem("Clear Shelf", new BMessage(kClear));
    for (auto item : {copy, move, archive}) {
      actions->AddItem(item);
      item->SetTarget(this);
    }
    actions->AddSeparatorItem();
    actions->AddItem(clear);
    clear->SetTarget(this);
    menu->AddItem(actions);
    auto collections = new BMenu("Collections");
    saveCollection = new BMenuItem("Save current Shelf...",
                                   new BMessage(kSaveCollection));
    collections->AddItem(saveCollection);
    saveCollection->SetTarget(this);
    collections->AddSeparatorItem();
    loadCollections = new BMenu("Load");
    deleteCollections = new BMenu("Delete");
    collections->AddItem(loadCollections);
    collections->AddItem(deleteCollections);
    menu->AddItem(collections);
    // A normal titled window can remain independent or join Stack & Tile.
    rows = new ShelfList();
    rows->AddColumn(new BStringColumn("Name", 145, 72, 500, B_TRUNCATE_MIDDLE),
                    0);
    rows->AddColumn(
        new BStringColumn("Original folder", 246, 120, 1000, B_TRUNCATE_MIDDLE),
        1);
    rows->AddColumn(new BStringColumn("Status", 96, 80, 200, B_TRUNCATE_END),
                    2);
    rows->SetExplicitMinSize(BSize(300, 130));
    rows->SetSelectionMode(B_MULTIPLE_SELECTION_LIST);
    rows->SetSelectionMessage(new BMessage(kSelect));
    rows->SetTarget(this);
    status = new BStringView(
        "shelf status", "Drop local entries, or use Files > Add to Shelf.");
    status->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
    status->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    status->SetTruncation(B_TRUNCATE_END);
    remove = new BButton("Remove from Shelf", new BMessage(kRemove));
    relink = new BButton("Relink...", new BMessage(kPickRelink));
    question = new InlineAction(BMessenger(this));
    question->SetName("shelf collection prompt");
    question->Hide();
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .Add(menu)
        .AddGroup(B_VERTICAL, 8)
        .SetInsets(10)
        .Add(rows)
        .Add(status)
        .Add(question)
        .AddGroup(B_HORIZONTAL, 8)
        .Add(new BButton("Refresh", new BMessage(kRefreshShelf)))
        .Add(relink)
        .Add(remove)
        .AddGlue()
        .End()
        .End();
    AddShortcut('W', B_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
    Render();
    RefreshCollections();
    BeginResolve({}, 0, {});
  }

  void WindowActivated(bool active) override {
    BWindow::WindowActivated(active);
    if (active)
      Render();
  }

  void MenusBeginning() override {
    BWindow::MenusBeginning();
    RefreshCollections();
  }

  void MessageReceived(BMessage *message) override {
    try {
      switch (message->what) {
      case kShowShelf:
        Activate();
        [[fallthrough]];
      case kRefreshShelf:
        Render();
        BeginResolve({}, 0, {});
        return;
      case kResolveFinished:
        resolving = false;
        Render();
        ShelfChanged();
        if (const char *error = message->GetString("error", nullptr))
          Status(error);
        else if (const char *loaded = message->GetString("loaded", nullptr))
          Status(std::string("Loaded “") + loaded +
                 "”. Relink any unavailable references.");
        else
          Status("Shelf refreshed.");
        return;
      case kSelect:
        UpdateSelection(shelf->Snapshot());
        return;
      case kSaveCollection:
        AskCollectionName();
        return;
      case kCollectionNamed:
        SaveCollection(*message);
        return;
      case kLoadCollection:
        LoadCollection(message->GetString("collection", ""));
        return;
      case kDeleteCollection:
        DeleteCollection(message->GetString("collection", ""));
        return;
      case kPickRelink:
        PickRelink();
        return;
      case kRelinkChosen:
        ConfirmRelink(*message);
        return;
      case kInlineClosed:
        return;
      case kRemove:
        shelf->Remove(Selected(), size_t(message->GetInt64("revision", -1)));
        Render();
        ShelfChanged();
        Status("Removed references only. Original files were not changed.");
        return;
      case kClear:
        shelf->Clear();
        Render();
        ShelfChanged();
        Status("Shelf cleared. Original files were not changed.");
        return;
      case kPickCopy:
        PickFolder(Action::Copy);
        return;
      case kPickMove:
        PickFolder(Action::Move);
        return;
      case kFolderChosen:
        ConfirmFolder(*message);
        return;
      case kCreateArchive:
        CreateArchive();
        return;
      case B_CANCEL:
        if (message->GetInt64("shelf picker", -1) == pickerSerial)
          picker.reset();
        return;
      case kDropped:
        Add(*message);
        return;
      default:
        if (message->WasDropped()) {
          Add(*message);
          return;
        }
        BWindow::MessageReceived(message);
      }
    } catch (const std::exception &e) {
      Render();
      Status(e.what());
    }
  }
};
} // namespace

void OpenShelfWindow(BMessenger owner, const std::shared_ptr<Shelf> &shelf,
                     BMessenger &window, const std::string &settingsFile) {
  if (window.SendMessage(kShowShelf) == B_OK)
    return;
  auto shelfWindow = new ShelfWindow(owner, shelf, settingsFile);
  window = BMessenger(shelfWindow);
  shelfWindow->Show();
}
} // namespace Listi
