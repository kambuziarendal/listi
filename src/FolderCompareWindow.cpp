// SPDX-License-Identifier: MIT
#include "AdaptiveScrollView.h"
#include "ColumnRows.h"
#include "FolderCompare.h"
#include "ThemeTextView.h"
#include <Layout.h>
#include <Application.h>
#include <Button.h>
#include <CheckBox.h>
#include "AdaptiveColumnList.h"
#include <ColumnTypes.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <Path.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
namespace {
constexpr uint32 kPathsChanged = 'cmpc';
constexpr uint32 kQuick = 'cmqu', kHash = 'cmha', kDone = 'cmdn',
                 kTick = 'cmtk', kCancel = 'cmca', kFilter = 'cmfi',
                 kSelected = 'cmse', kBrowse = 'cmbr', kChosen = 'cmch';
BTextView *Text(const char *name, float height) {
  auto v =
      new ThemeTextView(name, B_PANEL_BACKGROUND_COLOR, B_PANEL_TEXT_COLOR);
  v->MakeEditable(false);
  v->SetWordWrap(true);
  v->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
  v->SetExplicitMinSize(BSize(0, height));
  return v;
}
class ByteField : public BStringField {
public:
  const int64_t bytes;
  explicit ByteField(int64_t value)
      : BStringField(value < 0 ? "" : std::to_string(value).c_str()),
        bytes(value) {}
};

class ByteColumn : public BStringColumn {
public:
  explicit ByteColumn(const char *label)
      : BStringColumn(label, 100, 60, 300, B_TRUNCATE_END) {}

  int CompareFields(BField *a, BField *b) override {
    const auto left = static_cast<ByteField *>(a)->bytes;
    const auto right = static_cast<ByteField *>(b)->bytes;
    return (left > right) - (left < right);
  }
};

struct Row : BRow {
  size_t index;
  Row(const Listi::CompareEntry &e, size_t i) : index(i) {
    SetField(new BStringField(e.relative.c_str()), 0);
    SetField(new BStringField(Listi::CompareLabel(e.state)), 1);
    SetField(new ByteField(e.leftBytes), 2);
    SetField(new ByteField(e.rightBytes), 3);
  }
};
struct WorkState {
  std::atomic<bool> cancel{false};
  std::atomic<uint64_t> bytes{0};
  std::mutex mutex;
  Listi::CompareResult result;
};
class CompareWindow : public BWindow {
  BTextControl *left, *right;
  BButton *quick, *hash, *cancel, *browseLeft, *browseRight;
  BCheckBox *matches;
  BTextView *summary, *detail;
  BColumnListView *rows;
  std::unique_ptr<BFilePanel> picker;
  std::unique_ptr<BMessageRunner> timer;
  std::shared_ptr<WorkState> work;
  Listi::CompareResult result;
  bool busy = false, closing = false, haveResults = false;
  std::string heading;
  void Controls() {
    for (auto b : {quick, hash, browseLeft, browseRight})
      b->SetEnabled(!busy);
    left->SetEnabled(!busy);
    right->SetEnabled(!busy);
    matches->SetEnabled(!busy && haveResults);
    cancel->SetEnabled(busy);
  }
  void ClearRows() {
    Listi::ClearColumnRows(*rows);
    detail->SetText("");
  }
  void Render() {
    ClearRows();
    const auto visible =
        Listi::SelectCompareRows(result, matches->Value(), 5000);
    for (const auto index : visible.indices)
      rows->AddRow(new Row(result.entries[index], index));
    const auto displayed = visible.indices.size();
    const auto wanted = visible.total;
    std::string text = heading;
    if (!result.complete)
      text += "Incomplete: " + result.error;
    else if (result.different)
      text += "Differences found.";
    else if (result.checksum && result.verified)
      text += "No differences found. File contents match by SHA-256.";
    else if (result.checksum)
      text += "No differences found. No regular file pairs to hash.";
    else
      text += "Names and sizes match. Contents have NOT been checked.";
    text += "\n" + std::to_string(result.different) + " different, " +
            std::to_string(result.verified) + " SHA-256 matches, " +
            std::to_string(result.sizeOnly) + " size-only matches, " +
            std::to_string(result.unverified) + " unverified. ";
    if (displayed < wanted)
      text += "Showing " + std::to_string(displayed) + " of " +
              std::to_string(wanted) +
              " rows, prioritizing differences and unverified entries.";
    else
      text += std::to_string(displayed) + " rows shown.";
    summary->SetText(text.c_str());
  }
  void Start(bool checksum) {
    if (busy)
      return;
    picker.reset();
    result = {};
    haveResults = false;
    ClearRows();
    busy = true;
    Controls();
    Listi::CompareRequest request{left->Text(), right->Text(), checksum};
    heading = checksum ? "SHA-256 comparison. " : "Quick size comparison. ";
    summary->SetText((heading + "Reading folders recursively...").c_str());
    work = std::make_shared<WorkState>();
    auto state = work;
    BMessenger target(this);
    std::thread([state, target, request] {
      Listi::TransferControl control;
      control.proceed = [state] { return !state->cancel.load(); };
      control.progress = [state](uint64_t n) { state->bytes += n; };
      auto result = Listi::CompareFolders(request, control);
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->result = std::move(result);
      }
      target.SendMessage(kDone);
    }).detach();
    BMessage tick(kTick);
    timer = std::make_unique<BMessageRunner>(BMessenger(this), &tick, 500000);
  }

public:
  CompareWindow(const std::string &a, const std::string &b)
      : BWindow(BRect(100, 100, 1160, 690), "Listi: Compare folders",
                B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS) {
    left = new BTextControl("compare left", "Left folder:", a.c_str(), nullptr);
    right =
        new BTextControl("compare right", "Right folder:", b.c_str(), nullptr);
    left->SetModificationMessage(new BMessage(kPathsChanged));
    right->SetModificationMessage(new BMessage(kPathsChanged));
    auto lm = new BMessage(kBrowse);
    lm->AddBool("right", false);
    auto rm = new BMessage(kBrowse);
    rm->AddBool("right", true);
    browseLeft = new BButton("Choose...", lm);
    browseRight = new BButton("Choose...", rm);
    quick = new BButton("Compare names and sizes", new BMessage(kQuick));
    hash = new BButton("Compare with SHA-256", new BMessage(kHash));
    cancel = new BButton("Cancel", new BMessage(kCancel));
    matches = new BCheckBox("Show matching entries", new BMessage(kFilter));
    matches->SetName("compare show matches");
    rows = new Listi::AdaptiveColumnList("folder comparison results",
                               B_WILL_DRAW | B_NAVIGABLE, B_FANCY_BORDER);
    rows->AddColumn(
        new BStringColumn("Relative path", 280, 120, 2000, B_TRUNCATE_MIDDLE),
        0);
    rows->AddColumn(new BStringColumn("Result", 240, 120, 1000, B_TRUNCATE_END),
                    1);
    rows->AddColumn(new ByteColumn("Left bytes"), 2);
    rows->AddColumn(new ByteColumn("Right bytes"), 3);
    rows->SetSelectionMode(B_SINGLE_SELECTION_LIST);
    rows->SetSelectionMessage(new BMessage(kSelected));
    rows->SetTarget(this);
    rows->SetExplicitMinSize(BSize(760, 180));
    summary = Text("compare summary", 70);
    summary->SetText(
        "Compare recursively without changing either folder. Quick mode checks "
        "sizes only; SHA-256 also reads file contents.");
    detail = Text("compare detail", 85);
    auto scroll =
        new Listi::AdaptiveScrollView("compare detail scroll", detail, false);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 8)
        .SetInsets(10)
        .AddGroup(B_HORIZONTAL, 8)
        .Add(left)
        .Add(browseLeft)
        .End()
        .AddGroup(B_HORIZONTAL, 8)
        .Add(right)
        .Add(browseRight)
        .End()
        .AddGroup(B_HORIZONTAL, 8)
        .Add(quick)
        .Add(hash)
        .AddGlue()
        .Add(cancel)
        .End()
        .Add(summary)
        .Add(rows)
        .Add(matches)
        .Add(scroll)
        .Add(new BStringView("compare scope",
                             "Read-only comparison. Includes subfolders "
                             "and hidden files; links are not followed. "
                             "Attributes and permissions are not compared."))
        .AddGroup(B_HORIZONTAL, 8)
        .AddGlue()
        .Add(new BButton("Close", new BMessage(B_QUIT_REQUESTED)))
        .End();
    AddShortcut('W', B_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
    AddShortcut(B_ESCAPE, B_NO_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
    GetLayout()->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    Controls();
    Show();
  }
  ~CompareWindow() override {
    if (work)
      work->cancel = true;
    timer.reset();
  }
  bool QuitRequested() override {
    if (busy) {
      closing = true;
      work->cancel = true;
      summary->SetText("Cancelling comparison before closing...");
      return false;
    }
    return true;
  }
  void MessageReceived(BMessage *m) override {
    switch (m->what) {
    case kPathsChanged:
      if (!busy) {
        haveResults = false;
        result = {};
        ClearRows();
        summary->SetText(
            "Folders changed. Run a new comparison for these paths.");
        Controls();
      }
      return;
    case kQuick:
      Start(false);
      return;
    case kHash:
      Start(true);
      return;
    case kCancel:
      if (work && busy) {
        work->cancel = true;
        summary->SetText(
            "Cancelling. Partial results will not be marked complete.");
      }
      return;
    case kTick:
      if (busy && !work->cancel) {
        std::string s = heading + "Scanning/checking, " +
                        std::to_string(work->bytes.load()) +
                        " bytes read. No files are changed.";
        summary->SetText(s.c_str());
      }
      return;
    case kDone:
      if (work) {
        std::lock_guard<std::mutex> lock(work->mutex);
        result = std::move(work->result);
        haveResults = true;
        busy = false;
        timer.reset();
        Controls();
        Render();
        if (closing)
          PostMessage(B_QUIT_REQUESTED);
      }
      return;
    case kFilter:
      if (!busy && haveResults)
        Render();
      return;
    case kSelected: {
      auto row = static_cast<Row *>(rows->CurrentSelection());
      if (!row || row->index >= result.entries.size()) {
        detail->SetText("");
        return;
      }
      auto &e = result.entries[row->index];
      std::string s = e.relative + "\nLeft: " + e.left + "\nRight: " + e.right;
      if (!e.leftHash.empty())
        s += "\nLeft SHA-256: " + e.leftHash;
      if (!e.rightHash.empty())
        s += "\nRight SHA-256: " + e.rightHash;
      if (!e.detail.empty())
        s += "\n" + e.detail;
      detail->SetText(s.c_str());
      return;
    }
    case kBrowse: {
      if (busy)
        return;
      bool isRight = m->GetBool("right", false);
      BMessage chosen(kChosen);
      chosen.AddBool("right", isRight);
      BMessenger target(this);
      entry_ref directory;
      const char *path = isRight ? right->Text() : left->Text();
      if (get_ref_for_path(path, &directory) != B_OK)
        get_ref_for_path("/boot/home", &directory);
      picker = std::make_unique<BFilePanel>(B_OPEN_PANEL, &target, &directory,
                                            B_DIRECTORY_NODE, false, &chosen);
      picker->Window()->SetTitle(isRight ? "Listi: Choose right folder"
                                         : "Listi: Choose left folder");
      picker->Show();
      return;
    }
    case kChosen: {
      if (busy)
        return;
      entry_ref ref;
      if (m->FindRef("refs", &ref) == B_OK) {
        BPath path(&ref);
        if (path.InitCheck() == B_OK)
          (m->GetBool("right", false) ? right : left)->SetText(path.Path());
      }
      return;
    }
    }
    BWindow::MessageReceived(m);
  }
};
} // namespace
void OpenFolderCompare(const std::string &left, const std::string &right) {
  new CompareWindow(left, right);
}
