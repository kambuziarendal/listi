// SPDX-License-Identifier: MIT
#include "MillerColumns.h"
#include "AdaptiveScrollView.h"
#include "DirectoryScanner.h"
#include "ReadWorker.h"
#include "FileDrop.h"
#include "FolderDropTarget.h"
#include "Pane.h"
#include "PanelAppearance.h"
#include "TypeToSelect.h"
#include "VirtualFile.h"

#include <Bitmap.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <StringView.h>
#include <Window.h>
#include <algorithm>
#include <cmath>

namespace {
constexpr uint32 kCheckColumnRead = 'mcrd';

std::string ColumnLabel(const std::string &path) {
  if (path == "@disks")
    return "Disks";
  const auto location = Listi::ParseLocation(path);
  auto name = Listi::LocationLeaf(path);
  if (!name.empty())
    return name;
  if (location.Archive())
    return Listi::LocationLeaf(location.archive);
  return location.Local() ? "/" : location.host;
}

std::string BranchRow(const std::string &path, const std::string &next) {
  const auto location = Listi::ParseLocation(next);
  if (location.Archive() && location.path == "/" && !Listi::IsVirtual(path))
    return location.archive;
  return next;
}

class ColumnRows : public BView {
public:
  ColumnRows(Pane *owner, PaneModel *model, std::string location)
      : BView("Miller rows", B_WILL_DRAW | B_FRAME_EVENTS |
                                 B_FULL_UPDATE_ON_RESIZE |
                                 (model ? B_NAVIGABLE : 0)),
        location(std::move(location)), pane(owner), model(model) {
    SetExplicitMinSize(BSize(0, 0));
    SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    SetActive(false);
  }

  const std::string location;
  std::string branch;
  PaneModel entries;
  bool complete = false;
  bool failed = false;
  bool partial = false;

  bool Current() const { return model != nullptr; }

  void AttachedToWindow() override { Refresh(); }
  void FrameResized(float, float) override { Refresh(); }

  void SetActive(bool active) {
    auto color = Listi::PanelBackground(B_LIST_BACKGROUND_COLOR, active);
    SetViewColor(color);
    SetLowColor(color);
    Invalidate();
  }

  PaneEntry *ScrollRow(const std::string *path, float &top) {
    const auto &rows = Visible();
    for (size_t i = 0; i < rows.size(); ++i) {
      top = float(i) * height;
      if (path ? rows[i]->path == *path : top + height > Bounds().top)
        return rows[i];
    }
    return nullptr;
  }
  float ContentHeight() const { return float(Visible().size() + (partial ? 1 : 0)) * height; }
  void Refresh(bool reveal = false) {
    font_height font;
    GetFontHeight(&font);
    height = std::max(24.f, std::ceil(font.ascent + font.descent) + 8);
    const float content = ContentHeight();
    if (Parent())
      Parent()->InvalidateLayout();
    if (auto bar = ScrollBar(B_VERTICAL)) {
      const float visible = Bounds().Height() + 1;
      bar->SetRange(0, std::max(0.f, content - visible));
      bar->SetProportion(content > 0 ? std::min(1.f, visible / content) : 1.f);
      bar->SetSteps(height, std::max(height, visible - height));
    }
    if (reveal) {
      const auto focused = Current() ? model->Focused() : nullptr;
      for (size_t i = 0; i < Visible().size(); ++i) {
        if (Visible()[i] != focused && Visible()[i]->path != branch)
          continue;
        const float top = float(i) * height;
        if (top < Bounds().top)
          ScrollTo(0, top);
        else if (top + height > Bounds().bottom)
          ScrollTo(0, top + height - Bounds().Height());
        break;
      }
    }
    Invalidate();
  }

  void MakeFocus(bool focused = true) override {
    if (!focused)
      typeSelect.Reset();
    const bool changed = IsFocus() != focused;
    BView::MakeFocus(focused);
    if (changed && focused && Current())
      pane->Activate();
    Invalidate();
  }

  void Draw(BRect update) override {
    const auto &rows = Visible();
    const auto focused = Current() ? model->Focused() : nullptr;
    font_height font;
    GetFontHeight(&font);
    if (partial) {
      SetHighUIColor(B_LIST_ITEM_TEXT_COLOR);
      DrawString("Incomplete listing", BPoint(8, rows.size() * height + font.ascent + 5));
    } else if (rows.empty() && !Current()) {
      SetHighUIColor(B_LIST_ITEM_TEXT_COLOR);
      DrawString(!complete ? "Reading…"
                 : failed  ? "Cannot open folder"
                           : "Empty",
                 BPoint(8, font.ascent + 5));
    }
    BFont textFont;
    GetFont(&textFont);
    const int first = std::max(0, int(update.top / height));
    const int last =
        std::min(int(rows.size()), int(update.bottom / height) + 1);
    for (int i = first; i < last; ++i) {
      const auto row = rows[i];
      const float top = float(i) * height;
      const BRect rect(1, top + 1, Bounds().right - 1, top + height - 1);
      const bool selected =
          Current() ? model->IsSelected(row->path) : row->path == branch;
      if (selected) {
        SetHighUIColor(B_LIST_SELECTED_BACKGROUND_COLOR);
        FillRoundRect(rect, 3, 3);
        SetLowUIColor(B_LIST_SELECTED_BACKGROUND_COLOR);
      } else {
        SetLowColor(ViewColor());
      }
      SetHighUIColor(selected ? B_LIST_SELECTED_ITEM_TEXT_COLOR
                              : B_LIST_ITEM_TEXT_COLOR);
      const float baseline =
          top + std::floor((height + font.ascent - font.descent) / 2);
      float left = 8;
      if (Current())
        pane->RequestVisibleIcon(*row);
      if (row->icon) {
        const float y = top + (height - 16) / 2;
        SetDrawingMode(B_OP_ALPHA);
        DrawBitmap(row->icon.get(), row->icon->Bounds(),
                   BRect(5, y, 20, y + 15));
        SetDrawingMode(B_OP_COPY);
        left = 25;
      }
      const bool arrow = row->directory && !row->parent;
      BString label(row->transferState == 3 ? "Failed: " : "");
      label << row->name.c_str();
      textFont.TruncateString(
          &label, B_TRUNCATE_MIDDLE,
          std::max(0.f, rect.right - left - (arrow ? 20 : 8)));
      DrawString(label.String(), BPoint(left, baseline));
      if (arrow) {
        const float x = rect.right - 12, y = top + height / 2;
        StrokeLine(BPoint(x, y - 4), BPoint(x + 4, y));
        StrokeLine(BPoint(x + 4, y), BPoint(x, y + 4));
      }
      if (row->path == hoverPath)
        Listi::DrawFolderHover(this, rect);
      if (focused == row && IsFocus()) {
        SetHighUIColor(B_KEYBOARD_NAVIGATION_COLOR);
        StrokeRoundRect(rect, 3, 3);
      }
      if (row->transferState != 0 && !row->parent) {
        SetHighUIColor(row->transferState == 3 ? B_FAILURE_COLOR
                                               : B_KEYBOARD_NAVIGATION_COLOR);
        const float fraction =
            row->transferState == 1 ? std::clamp(row->fraction, 0.f, 1.f) : 1.f;
        StrokeLine(rect.LeftBottom(),
                   BPoint(rect.left + rect.Width() * fraction, rect.bottom));
      }
    }
    SetLowColor(ViewColor());
  }

  void MouseDown(BPoint point) override {
    typeSelect.Reset();
    pane->Activate();
    const auto row = At(int(point.y / height));
    const auto message = Window()->CurrentMessage();
    const bool secondary =
        message->GetInt32("buttons", 0) & B_SECONDARY_MOUSE_BUTTON;
    const bool doubleClick = message->GetInt32("clicks", 1) == 2;
    if (!Current()) {
      if (row && row->directory && !secondary)
        pane->NavigateFromColumn(row->path, {}, false, false, {}, row, entries.archiveVersion);
      else
        pane->NavigateFromColumn(location, row ? row->path : "",
                                 row && doubleClick && !secondary, secondary,
                                 ConvertToScreen(point), row, entries.archiveVersion);
      return;
    }
    MakeFocus();
    if (pane->IsScanning())
      return;
    if (secondary) {
      pane->ContextMenu(row && !row->parent ? row->path : "",
                        ConvertToScreen(point));
      return;
    }
    const auto keys = modifiers();
    pressed = point;
    pressedPath = row && !row->parent ? row->path : "";
    dragging = false;
    deferredPick = row && model->IsSelected(row->path) && !doubleClick &&
                   !(keys & (B_SHIFT_KEY | B_COMMAND_KEY | B_CONTROL_KEY));
    if (row && !deferredPick)
      Pick(int(point.y / height), keys & B_SHIFT_KEY,
           keys & (B_COMMAND_KEY | B_CONTROL_KEY));
    if (!row && !(keys & (B_SHIFT_KEY | B_COMMAND_KEY | B_CONTROL_KEY))) {
      model->Select(0);
      pane->SendSelection();
    }
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    if (row && doubleClick)
      Invoke();
  }

  std::string hoverPath;
  std::string DropTarget(BPoint point) const {
    if (!Bounds().Contains(point) || (Current() && pane->IsSearchList()))
      return {};
    return Listi::FolderDropTarget(location, At(int(point.y / height)));
  }
  void MouseMoved(BPoint point, uint32 transit, const BMessage *payload) override {
    Listi::SetFolderHover(this, hoverPath,
        Listi::IsFileHover(transit, payload) ? DropTarget(point) : "");
    if (payload)
      return;
    if (pressedPath.empty() || dragging ||
        (std::abs(point.x - pressed.x) < 5 &&
         std::abs(point.y - pressed.y) < 5) ||
        !model->IsSelected(pressedPath))
      return;
    BPoint mouse;
    uint32 buttons;
    GetMouse(&mouse, &buttons);
    if (!(buttons & B_PRIMARY_MOUSE_BUTTON))
      return;
    try {
      auto drag = Listi::MakePaneFileDrag(*model);
      dragging = true;
      DragMessage(&drag, BRect(point.x, point.y, point.x + 90, point.y + 22));
    } catch (const std::exception &error) {
      pane->ShowStatus(error.what());
    }
  }

  void MouseUp(BPoint) override {
    if (deferredPick && !dragging)
      for (size_t i = 0; i < Visible().size(); ++i)
        if (Visible()[i]->path == pressedPath) {
          Pick(i, false);
          break;
        }
    CancelPress();
  }

  void CancelPress() {
    pressedPath.clear();
    dragging = deferredPick = false;
  }

  void KeyDown(const char *bytes, int32 length) override {
    int match;
    if (length > 0 && Current() && !pane->IsScanning() &&
        typeSelect.KeyDown({bytes, size_t(length)},
                           Listi::TypeSelectModifiers(*this), system_time(),
                           location.c_str(), Visible().size(), model->Focused(),
                           [this](int index) { return At(index); }, match)) {
      Pick(match, false);
      return;
    }
    if (length != 1 || !Current()) {
      BView::KeyDown(bytes, length);
      return;
    }
    switch (bytes[0]) {
    case B_TAB:
      Window()->PostMessage(kSwitch);
      return;
    case B_LEFT_ARROW:
    case B_BACKSPACE: {
      try {
        auto parent =
            Listi::ParentLocation(location == "@disks" ? "/" : location);
        if (parent != location)
          pane->NavigateFromColumn(parent, BranchRow(parent, location), false, false, {},
                                   nullptr, model->archiveVersion);
      } catch (const std::exception &error) {
        pane->ShowStatus(error.what());
      }
      return;
    }
    }
    if (pane->IsScanning())
      return;
    const int count = int(Visible().size());
    const auto focused = model->Focused();
    const auto it = std::find(Visible().begin(), Visible().end(), focused);
    const int index = it == Visible().end() ? -1 : int(it - Visible().begin());
    const bool extend = modifiers() & B_SHIFT_KEY;
    switch (bytes[0]) {
    case B_UP_ARROW:
      Pick(index < 0 ? count - 1 : std::max(0, index - 1), extend);
      return;
    case B_DOWN_ARROW:
      Pick(std::min(count - 1, index + 1), extend);
      return;
    case B_HOME:
      Pick(0, extend);
      return;
    case B_END:
      Pick(count - 1, extend);
      return;
    case ' ':
      Pick(std::max(0, index), false, true);
      return;
    case B_RIGHT_ARROW:
      if (!focused || !focused->directory || focused->parent)
        return;
      [[fallthrough]];
    case B_ENTER:
      Invoke();
      return;
    }
    BView::KeyDown(bytes, length);
  }

  void MessageReceived(BMessage *message) override {
    if (message->WasDropped()) {
      BMessage dropped(*message);
      dropped.what = kDrop;
      dropped.RemoveName("destination");
      const auto target = DropTarget(ConvertFromScreen(message->DropPoint()));
      Listi::SetFolderHover(this, hoverPath, "");
      dropped.AddString("destination", target.empty() ? location.c_str() : target.c_str());
      Window()->PostMessage(&dropped);
      return;
    }
    BView::MessageReceived(message);
  }

private:
  Pane *pane;
  PaneModel *model;
  Listi::TypeToSelect typeSelect;
  float height = 24;
  BPoint pressed;
  std::string pressedPath;
  bool dragging = false, deferredPick = false;

  const std::vector<PaneEntry *> &Visible() const {
    return Current() ? model->Visible() : entries.Visible();
  }

  PaneEntry *At(int index) const {
    return index >= 0 && index < int(Visible().size()) ? Visible()[index]
                                                       : nullptr;
  }

  void Pick(int index, bool extend, bool toggle = false) {
    if (!At(index))
      return;
    model->Pick(index, extend, toggle);
    pane->SendSelection();
  }

  void Invoke() {
    BMessage invoke(kInvoke);
    invoke.AddString("column", location.c_str());
    if (auto focused = model->Focused())
      invoke.AddString("selection", focused->path.c_str());
    Window()->PostMessage(&invoke, pane);
  }
};
} // namespace

struct MillerColumns::State {
  struct Column {
    BGroupView *container;
    ColumnRows *rows;
  };

  Pane *pane;
  PaneModel *model;
  std::vector<Column> columns;
  size_t current = 0;
  BFont font;
  bool active = false, enabled = false, attached = false;
  std::string location, reading;
  PaneModel incoming;
  Listi::ScanRows receivedRows;
  Listi::ReadWorker *scanner = nullptr;
  std::shared_ptr<std::atomic<int64>> generation =
      std::make_shared<std::atomic<int64>>(0);
  std::shared_ptr<DirectoryScanDelivery> delivery =
      std::make_shared<DirectoryScanDelivery>();
  std::unique_ptr<BMessageRunner> timer;

  State(Pane *owner, PaneModel *entries) : pane(owner), model(entries) {}

  void Add(MillerColumns *owner, const std::string &path, bool currentColumn) {
    auto rows = new ColumnRows(pane, currentColumn ? model : nullptr, path);
    rows->SetFont(&font);
    rows->SetActive(active);
    auto label = new BStringView("Miller heading", ColumnLabel(path).c_str());
    if (currentColumn)
      label->SetFont(be_bold_font);
    label->SetTruncation(B_TRUNCATE_MIDDLE);
    label->SetToolTip(path.c_str());
    label->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
    auto scroll = new Listi::AdaptiveScrollView("Miller vertical scroll", rows, false, true,
        [rows](float) { return BSize(0, rows->ContentHeight()); });
    auto container = new BGroupView(B_VERTICAL, 2);
    BLayoutBuilder::Group<>(container).Add(label, 0).Add(scroll);
    owner->AddChild(container);
    columns.push_back({container, rows});
  }

  void Pop() {
    auto container = columns.back().container;
    columns.pop_back();
    container->RemoveSelf();
    delete container;
  }

  void CancelRead() {
    ++*generation;
    if (!reading.empty())
      Listi::RetireReadWorker(scanner);
    timer.reset();
    reading.clear();
    incoming.Clear();
  }

  void ReadNext(MillerColumns *owner) {
    if (!attached || !enabled || !reading.empty())
      return;
    // Prefer the preview, then the nearest ancestors.
    for (auto it = columns.rbegin(); it != columns.rend(); ++it) {
      auto rows = it->rows;
      if (rows->Current() || rows->complete)
        continue;
      if (!scanner) {
        scanner =
            CreateDirectoryScanner(BMessenger(owner), generation, delivery);
        Listi::StartReadWorker(scanner);
      }
      receivedRows = {};
      reading = rows->location;
      const auto serial = ++*generation;
      BMessage scan(kScan);
      scan.AddString("path", reading.c_str());
      scan.AddInt64("serial", serial);
      scan.AddBool("icons", false);
      BMessage check(kCheckColumnRead);
      check.AddInt64("serial", serial);
      timer.reset(new BMessageRunner(BMessenger(owner), &check, 250000));
      if (timer->InitCheck() == B_OK && scanner &&
          scanner->PostMessage(&scan) == B_OK)
        return;
      Finish(rows, true);
    }
  }

  void Finish(ColumnRows *rows, bool failed) {
    timer.reset();
    reading.clear();
    rows->complete = true;
    rows->failed = failed;
    if (failed)
      incoming.Clear();
    else
      incoming.Filter("");
    rows->entries = std::move(incoming);
    incoming.Clear();
    rows->Refresh(true);
  }
};

MillerColumns::MillerColumns(Pane *pane, PaneModel *model)
    : BView("Miller columns", B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
      state(new State(pane, model)) {
  SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
  SetExplicitMinSize(BSize(0, 0));
  SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
}

MillerColumns::~MillerColumns() {
  state->CancelRead();
  state->generation->store(-1);
  Listi::RetireReadWorker(state->scanner);
}

void MillerColumns::AttachedToWindow() {
  state->attached = true;
  state->ReadNext(this);
}

void MillerColumns::SetEnabled(bool enabled) {
  if (state->enabled == enabled)
    return;
  state->enabled = enabled;
  if (!enabled) {
    state->CancelRead();
    while (!state->columns.empty())
      state->Pop();
    state->location.clear();
  }
}

void MillerColumns::SetLocation(const std::string &path) {
  if (!state->enabled || path == state->location)
    return;
  const bool focus = Window() && !state->columns.empty() &&
                     state->columns[state->current].rows->IsFocus();
  state->CancelRead();
  state->location = path;
  std::vector<std::string> paths{path};
  if (path != "@disks") {
    try {
      auto parent = Listi::ParentLocation(paths.back());
      while (parent != paths.back()) {
        paths.push_back(parent);
        parent = Listi::ParentLocation(paths.back());
      }
    } catch (const std::exception &) {
      // A failed/noncanonical main location still has a current column.
    }
  }
  std::reverse(paths.begin(), paths.end());
  // Keep shared ancestor views attached, including their rows and scroll
  // positions, while the scanner revalidates their directory contents.
  size_t shared = 0;
  while (shared + 1 < paths.size() && shared < state->columns.size()) {
    auto rows = state->columns[shared].rows;
    if (rows->Current() || rows->location != paths[shared])
      break;
    const auto branch = BranchRow(paths[shared], paths[shared + 1]);
    if (rows->branch != branch) {
      rows->branch = branch;
      rows->Refresh(true);
    }
    rows->complete = false;
    rows->partial = false;
    ++shared;
  }
  while (state->columns.size() > shared)
    state->Pop();
  state->current = paths.size() - 1;
  for (size_t i = shared; i < paths.size(); ++i) {
    state->Add(this, paths[i], i == state->current);
    if (i < state->current)
      state->columns.back().rows->branch = BranchRow(paths[i], paths[i + 1]);
  }
  LayoutColumns();
  state->ReadNext(this);
  if (focus)
    MakeFocus();
}

void MillerColumns::Refresh() {
  if (!state->columns.empty()) {
    auto rows = state->columns[state->current].rows;
    rows->CancelPress();
    rows->Refresh();
  }
}
void MillerColumns::RedrawRows() {
  if (!state->columns.empty())
    state->columns[state->current].rows->Invalidate();
}

void MillerColumns::SyncSelection() {
  if (state->columns.empty())
    return;
  auto focused = state->model->Focused();
  std::string preview;
  if (!state->pane->IsScanning() && focused && focused->directory &&
      !focused->parent && state->model->IsSelected(focused->path) &&
      state->model->Summary().selected == 1)
    preview = focused->path;
  auto current = state->columns[state->current].rows;
  current->Refresh(true);
  const bool hasPreview = state->columns.size() > state->current + 1;
  if (hasPreview && state->columns.back().rows->location == preview)
    return;
  if (hasPreview) {
    if (state->reading == state->columns.back().rows->location)
      state->CancelRead();
    state->Pop();
  }
  if (!preview.empty()) {
    if (!state->reading.empty())
      state->CancelRead();
    state->Add(this, preview, false);
  }
  if (hasPreview || !preview.empty()) {
    LayoutColumns();
    state->ReadNext(this);
  }
}

void MillerColumns::SetActive(bool active) {
  state->active = active;
  for (const auto &column : state->columns)
    column.rows->SetActive(active);
}

void MillerColumns::ApplyFont(const BFont &font) {
  state->font = font;
  for (const auto &column : state->columns) {
    column.rows->SetFont(&font);
    column.rows->Refresh(true);
  }
}

void MillerColumns::FrameResized(float, float) { LayoutColumns(); }

void MillerColumns::MakeFocus(bool focused) {
  if (!state->columns.empty())
    state->columns[state->current].rows->MakeFocus(focused);
}

static float ColumnWidth(float viewport) {
  return std::clamp(std::floor(viewport / 2), 130.f, 225.f);
}

float MillerColumns::ContentWidth(float viewport) const {
  return ColumnWidth(viewport) * state->columns.size();
}

PaneEntry *MillerColumns::ScrollRow(const std::string *path, float &top) {
  return state->current < state->columns.size()
      ? state->columns[state->current].rows->ScrollRow(path, top) : nullptr;
}
BPoint MillerColumns::ScrollPosition() const {
  return {Bounds().left, state->current < state->columns.size()
                            ? state->columns[state->current].rows->Bounds().top : 0};
}
void MillerColumns::RestoreScroll(BPoint position) {
  if (auto *bar = ScrollBar(B_HORIZONTAL)) bar->SetValue(position.x);
  if (state->current < state->columns.size())
    if (auto *bar = state->columns[state->current].rows->ScrollBar(B_VERTICAL))
      bar->SetValue(position.y);
}
void MillerColumns::LayoutColumns() {
  if (Parent())
    Parent()->InvalidateLayout();
  const float viewport = Bounds().Width() + 1;
  const float width = ColumnWidth(viewport);
  for (size_t i = 0; i < state->columns.size(); ++i) {
    auto view = state->columns[i].container;
    view->MoveTo(float(i) * width, 0);
    view->ResizeTo(width - 3, std::max(0.f, Bounds().Height()));
  }
  if (auto bar = ScrollBar(B_HORIZONTAL)) {
    const float content = width * state->columns.size();
    const float range = std::max(0.f, content - viewport);
    bar->SetRange(0, range);
    bar->SetProportion(content > 0 ? std::min(1.f, viewport / content) : 1.f);
    bar->SetSteps(width, std::max(width, viewport - width));
    bar->SetValue(range);
  }
}

void MillerColumns::MessageReceived(BMessage *message) {
  if (message->what != kRows && message->what != kScanDone &&
      message->what != kCheckColumnRead) {
    BView::MessageReceived(message);
    return;
  }
  const auto serial = message->GetInt64("serial", -1);
  if (state->reading.empty() || serial != state->generation->load())
    return;
  auto it = std::find_if(state->columns.begin(), state->columns.end(),
                         [&](const auto &column) {
                           return !column.rows->Current() &&
                                  column.rows->location == state->reading;
                         });
  if (it == state->columns.end())
    return;
  auto rows = it->rows;
  if (message->what == kRows) {
    state->receivedRows.Read(*message, "entry", [&](const BMessage &item) {
      if (item.GetBool("parent", false))
        return;
      auto row = std::make_unique<PaneEntry>();
      row->name = item.GetString("name", "");
      row->path = item.GetString("path", "");
      row->directory = item.GetBool("directory", false);
      row->kind = item.GetInt32("kind", 0);
      row->device = item.GetInt64("device", -1);
      row->node = item.GetInt64("node", -1);
      state->incoming.Add(std::move(row));
    });
    return;
  }
  const bool deliveryFailed = state->delivery->failedSerial.load() == serial;
  if (message->what == kScanDone || deliveryFailed) {
    state->incoming.archiveVersion = message->GetString("archiveVersion", "");
    const bool missingVersion = Listi::ParseLocation(rows->location).Archive() &&
                                state->incoming.archiveVersion.empty();
    const bool failed = missingVersion || deliveryFailed || message->HasString("error") ||
                        !state->receivedRows.Complete(*message);
    rows->partial = !failed && message->HasString("inspectionWarning");
    rows->SetToolTip(rows->partial ? message->GetString("inspectionWarning", "") : "");
    state->Finish(rows, failed);
    state->ReadNext(this);
  }
}
