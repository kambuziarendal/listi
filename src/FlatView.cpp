// SPDX-License-Identifier: MIT
#include "ColumnRows.h"
#include "AdaptiveColumnList.h"
#include "FlatView.h"
#include "CopyTextMenu.h"
#include "FileClipboard.h"
#include "FileSearch.h"
#include "FileDrop.h"
#include "JobSnapshot.h"
#include "MainWindow.h"
#include "MainWindowMenu.h"
#include "NameFilter.h"
#include "SearchWorker.h"
#include "ReadWorker.h"
#include "Shelf.h"
#include "ShelfWindow.h"
#include <Button.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include "WorkspaceLayout.h"
#include <Message.h>
#include <MessageFilter.h>
#include <MessageRunner.h>
#include <StorageDefs.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>
#include <deque>
#include <exception>
#include <stdexcept>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Listi {
namespace {
constexpr uint32 kFilterChanged = 'flfc', kCollect = 'flcl', kDeliveryCheck = 'fldc';

class FlatRow : public BRow {
public:
  const SearchHit &hit;
  FlatRow(const SearchHit &file, const std::string &name)
      : BRow(24), hit(file) {
    SetField(new BStringField(name.c_str()), 0);
    SetField(new BStringField(hit.relativeFolder.c_str()), 1);
  }
};

class FlatContextClick : public BMessageFilter {
  BColumnListView &list;
  const int32 pane;

public:
  FlatContextClick(BColumnListView &list, int32 pane)
      : BMessageFilter(B_MOUSE_DOWN), list(list), pane(pane) {}
  filter_result Filter(BMessage *click, BHandler **) override {
    if (!(click->GetInt32("buttons", 0) & B_SECONDARY_MOUSE_BUTTON))
      return B_DISPATCH_MESSAGE;
    BPoint point;
    if (click->FindPoint("where", &point) != B_OK)
      return B_SKIP_MESSAGE;
    if (auto *row = list.RowAt(point)) {
      list.MakeFocus();
      if (!row->IsSelected()) {
        list.DeselectAll();
        list.AddToSelection(row);
      }
      list.SetFocusRow(row);
      list.SelectionChanged();
      BMessage context(kFlatContext);
      context.AddInt32("pane", pane);
      context.AddPoint("point", list.ScrollView()->ConvertToScreen(point));
      list.Window()->MessageReceived(&context);
    }
    return B_SKIP_MESSAGE;
  }
};

class FlatResults : public AdaptiveColumnList {
  BButton *collect;
  const int32 pane;

public:
  FlatResults(BButton *action, int32 panel)
      : AdaptiveColumnList("flat results", B_WILL_DRAW | B_NAVIGABLE,
                        B_FANCY_BORDER),
        collect(action), pane(panel) {
    AddColumn(new BStringColumn("Name", 230, 100, 1000, B_TRUNCATE_MIDDLE), 0);
    AddColumn(
        new BStringColumn("Relative folder", 360, 120, 1600, B_TRUNCATE_MIDDLE),
        1);
    SetSelectionMode(B_MULTIPLE_SELECTION_LIST);
    SetInvocationMessage(new BMessage(kInvoke));
    ScrollView()->AddFilter(new FlatContextClick(*this, pane));
  }

  void NotifyPreview() {
    if (Window()) {
      BMessage changed(kPreviewSelection);
      changed.AddInt32("pane", pane);
      Window()->PostMessage(&changed);
    }
  }

  Job SelectedEntries() const {
    Job selection;
    for (auto row = CurrentSelection(); row; row = CurrentSelection(row)) {
      const auto &hit = static_cast<const FlatRow *>(row)->hit;
      selection.sources.push_back(hit.path);
      selection.expectedSources.emplace(hit.path, NodeIdentity{int32(hit.device), hit.node});
    }
    return selection;
  }

  void SelectVisible(uint32 command) {
    if (command == kNone)
      DeselectAll();
    else {
      for (int32 i = 0; i < CountRows(); ++i) {
        auto *row = RowAt(i);
        if (command == kInvert && row->IsSelected())
          Deselect(row);
        else
          AddToSelection(row);
      }
    }
    SelectionChanged();
  }

  bool InitiateDrag(BPoint point, bool) override {
    try {
      const auto selected = SelectedEntries();
      BMessage drag = MakeFileDrag(selected.sources, selected.expectedSources);
      DragMessage(&drag, BRect(point.x, point.y, point.x + 90, point.y + 22));
      return true;
    } catch (const std::exception &e) {
      PostMainWindowNotice(e.what());
      return false;
    }
  }

  float ContentHeight() override {
    return CountRows() > 0 ? (RowAt(0)->Height() + 1) * CountRows() : 0;
  }
  void SelectionChanged() override {
    BColumnListView::SelectionChanged();
    collect->SetEnabled(CurrentSelection() != nullptr);
    NotifyPreview();
  }
};

class FlatView : public BGroupView {
  struct File {
    SearchHit hit;
    std::string name;
    FlatRow *row = nullptr;
  };

  const std::string rootPath;
  BMessenger owner;
  std::shared_ptr<Shelf> shelf;
  std::shared_ptr<std::atomic<int64>> generation =
      std::make_shared<std::atomic<int64>>(1);
  Listi::ReadWorker *worker = nullptr;
  std::shared_ptr<std::atomic<int64>> failedSerial;
  std::unique_ptr<BMessageRunner> deliveryTimer;
  // Rows refer to these immutable scan hits. Growth must not invalidate them.
  std::deque<File> files;
  BTextControl *filter;
  BStringView *status, *shelfStatus;
  BButton *stop, *collect;
  FlatResults *results;
  std::string needle, scanState = "Scanning…";
  bool scanning = false;

  static BStringView *Line(const char *name, const char *text) {
    auto line = new BStringView(name, text);
    line->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
    line->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    line->SetTruncation(B_TRUNCATE_MIDDLE);
    line->SetToolTip(text);
    return line;
  }

  void UpdateRow(File &file) {
    const bool visible =
        MatchesNameFilter(file.name.c_str(), needle.c_str()) ||
        MatchesNameFilter(file.hit.relativeFolder.c_str(), needle.c_str());
    if (visible && !file.row) {
      file.row = new FlatRow(file.hit, file.name);
      results->AddRow(file.row);
    } else if (!visible && file.row) {
      results->RemoveRow(file.row);
      delete file.row;
      file.row = nullptr;
    }
  }

  void UpdateStatus() {
    results->InvalidateLayout();
    std::string text = std::to_string(results->CountRows()) + " of " +
                       std::to_string(files.size()) + " files shown. " +
                       scanState;
    status->SetText(text.c_str());
    status->SetToolTip(text.c_str());
    collect->SetEnabled(results->CurrentSelection() != nullptr);
    stop->SetEnabled(scanning);
    if (!scanning) deliveryTimer.reset();
  }

  void StartScan() {
    generation->fetch_add(1);
    Listi::ClearColumnRows(*results);
    results->NotifyPreview();
    files.clear();
    shelfStatus->SetText("");
    shelfStatus->SetToolTip("");
    scanState = "Scanning…";

    RetireReadWorker(worker);
    failedSerial = std::make_shared<std::atomic<int64>>(-1);
    worker = NewSearchWorker(BMessenger(this), generation, failedSerial);
    StartReadWorker(worker);
    BMessage check(kDeliveryCheck);
    deliveryTimer.reset(new BMessageRunner(BMessenger(this), &check, 250000));
    BMessage request(kSearchStart);
    request.AddInt64("serial", generation->load());
    request.AddString("root", rootPath.c_str());
    request.AddBool("flat", true);
    scanning = worker && deliveryTimer->InitCheck() == B_OK &&
               worker->PostMessage(&request) == B_OK;
    if (!scanning)
      scanState = "Incomplete: could not start scan.";
    UpdateStatus();
  }

  void Open(const std::string &path) {
    BMessage open(kOpenPaths);
    open.AddString("path", path.c_str());
    open.AddString("mode", "system");
    owner.SendMessage(&open, (BHandler *)nullptr, 0);
  }

  void Collect() {
    auto selected = results->SelectedEntries();
    if (selected.sources.empty())
      return;
    std::string text;
    try {
      // Do not re-capture identities after the scan or consult the active pane.
      // Shelf validates the entire selected set before adding any references.
      auto count = shelf->Add(selected.sources, selected.expectedSources);
      text = count ? std::to_string(count) + " added to Shelf."
                   : "Selection is already on the Shelf.";
      BMessage refresh(kRefreshShelf);
      owner.SendMessage(&refresh, (BHandler *)nullptr, 0);
    } catch (const std::exception &e) {
      text = e.what();
    }
    shelfStatus->SetText(text.c_str());
    shelfStatus->SetToolTip(text.c_str());
  }

  void Colors() {
    results->SetColor(B_COLOR_BACKGROUND,
                      ui_color(B_DOCUMENT_BACKGROUND_COLOR));
    results->SetColor(B_COLOR_TEXT, ui_color(B_DOCUMENT_TEXT_COLOR));
    results->SetColor(B_COLOR_SELECTION, ui_color(B_CONTROL_HIGHLIGHT_COLOR));
    results->SetColor(B_COLOR_SELECTION_TEXT, ui_color(B_CONTROL_TEXT_COLOR));
    results->SetColor(B_COLOR_NON_FOCUS_SELECTION,
                      tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), .8f));
    results->Invalidate();
  }

public:
  FlatView(BMessenger target, std::shared_ptr<Shelf> collection,
           const char *path, int32 pane)
      : BGroupView("flat view", B_VERTICAL, 5), rootPath(path), owner(target),
        shelf(std::move(collection)) {
    auto root = Line("flat root", (std::string("Folder: ") + path).c_str());
    auto *modes = new BPopUpMenu("View");
    for (int32 i = 0; i <= FlatMode; ++i) {
      auto *choice = new BMessage(kGridMode);
      choice->AddInt32("mode", i);
      auto *item = new BMenuItem(ViewModeLabel(i), choice);
      modes->AddItem(item);
      item->SetTarget(owner);
      item->SetMarked(i == FlatMode);
    }
    auto *display = new BMenuField("flat display mode", "View:", modes);
    filter = new BTextControl("flat filter", "Filter:", "", nullptr);
    filter->SetToolTip("Filter by file name or relative folder");
    filter->SetModificationMessage(new BMessage(kFilterChanged));
    filter->TextView()->SetMaxBytes(B_PATH_NAME_LENGTH);
    status = Line("flat status", "");
    shelfStatus = Line("flat Shelf status", "");
    stop =
        new BButton("cancel flat scan", "Stop scan", new BMessage(kSearchStop));
    collect = new BButton("add flat selection", "Add to Shelf",
                          new BMessage(kCollect));
    results = new FlatResults(collect, pane);
    BLayoutBuilder::Group<>(this)
        .SetInsets(5)
        .AddGroup(B_HORIZONTAL, 8)
        .Add(root)
        .Add(display)
        .End()
        .AddGroup(B_HORIZONTAL, 8)
        .Add(filter)
        .Add(stop)
        .End()
        .Add(results)
        .Add(status)
        .AddGroup(B_HORIZONTAL, 8)
        .Add(shelfStatus)
        .Add(collect)
        .End();
    Colors();
  }

  std::string SelectedFile() const {
    auto *row = static_cast<FlatRow *>(results->CurrentSelection());
    if (!row || results->CurrentSelection(row) || !SameSearchEntry(row->hit))
      return {};
    return row->hit.path;
  }

  Job SelectedEntries() const { return results->SelectedEntries(); }

  void AllAttached() override {
    BGroupView::AllAttached();
    filter->SetTarget(this);
    stop->SetTarget(this);
    results->SetTarget(this);
    collect->SetTarget(this);
    if (!worker)
      StartScan();
  }

  ~FlatView() {
    generation->fetch_add(1);
    RetireReadWorker(worker);
    results->Clear();
  }

  void MessageReceived(BMessage *message) override {
    switch (message->what) {
    case kFlatView:
      filter->MakeFocus();
      break;
    case kRefresh:
      StartScan();
      break;
    case kFilterChanged:
      needle = filter->Text();
      for (auto &file : files)
        UpdateRow(file);
      results->NotifyPreview();
      UpdateStatus();
      break;
    case kAll:
    case kNone:
    case kInvert:
      results->SelectVisible(message->what);
      break;
    case kFlatContext: {
      auto *first = results->CurrentSelection();
      if (!first)
        break;
      BPopUpMenu menu("Flat View file actions", false, false);
      auto *open = new BMenuItem("Open", new BMessage(kOpenSystem));
      open->SetEnabled(results->CurrentSelection(first) == nullptr);
      menu.AddItem(open);
      menu.AddItem(new BMenuItem("Copy", new BMessage(kClipboardCopy)));
      menu.AddItem(new BMenuItem("Cut", new BMessage(kClipboardCut)));
      AddCopyTextMenu(menu, true, true, true);
      menu.AddSeparatorItem();
      menu.AddItem(new BMenuItem("Add to Shelf", new BMessage(kAddToShelf)));
      if (auto *choice = menu.Go(message->GetPoint("point", BPoint()), false, true, false))
        {
          const uint32 action = choice->Message()->what;
          if (action == kCopyNames || action == kCopyPaths || action == kCopyRelative)
            MessageReceived(choice->Message());
          else
            Window()->MessageReceived(choice->Message());
        }
      break;
    }
    case kInvoke: {
      auto *row = static_cast<FlatRow *>(results->FocusRow());
      if (!row)
        break;
      if (!SameSearchEntry(row->hit)) {
        shelfStatus->SetText("The selected file changed. Refresh the view.");
        break;
      }
      Open(row->hit.path);
      break;
    }
    case kOpenSystem:
    case kClipboardCopy:
    case kClipboardCut:
    case kCopyNames:
    case kCopyRelative:
    case kCopyPaths: {
      try {
        auto selected = results->SelectedEntries();
        if (message->what == kOpenSystem) {
          if (selected.sources.size() != 1)
            throw std::runtime_error("Select exactly one file.");
          FreezeLocalJob(selected);
          Open(selected.sources.front());
        } else {
          const bool text = message->what != kClipboardCopy && message->what != kClipboardCut;
          if (text)
            CopySelectionText(message->what, selected.sources, rootPath);
          else
            PutFileClipboard(selected.sources, message->what == kClipboardCut,
                             be_clipboard, selected.expectedSources);
          const char *notice = text ? "Selection copied as text."
                               : message->what == kClipboardCut ? "Files marked for moving."
                                                               : "Files copied to clipboard.";
          shelfStatus->SetText(notice);
          shelfStatus->SetToolTip(notice);
        }
      } catch (const std::exception &e) {
        shelfStatus->SetText(e.what());
        shelfStatus->SetToolTip(e.what());
      }
      break;
    }
    case kAddToShelf:
    case kCollect:
      Collect();
      break;
    case kDeliveryCheck:
      if (scanning && failedSerial->load() == generation->load()) {
        generation->fetch_add(1);
        scanning = false;
        scanState = "Incomplete: results could not be delivered.";
        UpdateStatus();
      }
      break;
    case kSearchStop:
      if (scanning) {
        generation->fetch_add(1);
        scanning = false;
        scanState = "Incomplete: scan stopped.";
        UpdateStatus();
      }
      break;
    case kSearchBegin:
      break; // No revisions or live updates in Flat View.
    case kSearchRows: {
      if (!scanning || message->GetInt64("serial", -1) != generation->load())
        break;
      BMessage item;
      for (int32 i = 0; message->FindMessage("hit", i, &item) == B_OK; ++i) {
        SearchHit hit{item.GetString("path", ""), item.GetInt64("device", -1),
                      item.GetInt64("node", -1), item.GetString("folder", "")};
        std::string name = hit.path.substr(hit.path.find_last_of('/') + 1);
        auto &file = files.emplace_back(File{std::move(hit), std::move(name)});
        UpdateRow(file);
      }
      UpdateStatus();
      break;
    }
    case kSearchDone: {
      if (!scanning || message->GetInt64("serial", -1) != generation->load())
        break;
      scanning = false;
      const std::string error = message->GetString("error", "");
      const int64 skipped = message->GetInt64("skipped", 0);
      if (!error.empty())
        scanState = "Incomplete: " + error;
      else if (message->GetBool("cancelled", false))
        scanState = "Incomplete: scan interrupted.";
      else if (message->GetBool("limited", false))
        scanState = "Incomplete: scan limit reached.";
      else if (skipped)
        scanState = "Incomplete: " + std::to_string(skipped) +
                    " entries or folders skipped.";
      else
        scanState = "Scan complete.";
      UpdateStatus();
      break;
    }
    case B_COLORS_UPDATED:
      BGroupView::MessageReceived(message);
      Colors();
      break;
    default:
      BGroupView::MessageReceived(message);
      break;
    }
  }
};
}

Job FlatSelection(const BView &view) {
  return static_cast<const FlatView &>(view).SelectedEntries();
}

BView *CreateFlatView(BMessenger owner, std::shared_ptr<Shelf> shelf,
                      const char *root, int32 pane) {
  return new FlatView(owner, std::move(shelf), root, pane);
}

std::string FlatSelectedFile(const BView &view) {
  return static_cast<const FlatView &>(view).SelectedFile();
}
}
