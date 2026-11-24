// SPDX-License-Identifier: MIT
#include "ReadWorker.h"
#include "TerminalReplies.h"
#include "FileDrop.h"
#include "FolderDropTarget.h"
#include "FileGrid.h"
#include "PanelAppearance.h"
#include "RubberBand.h"
#include "TypeToSelect.h"
#include "Pane.h"
#include "PaneModel.h"
#include "ImagePreview.h"
#include "BitmapMailbox.h"
#include "VirtualFile.h"
#include <Bitmap.h>
#include <Entry.h>
#include <ScrollBar.h>
#include <Window.h>
#include <algorithm>
#include <set>
namespace {
constexpr uint32 kThumbnail = 'thum', kThumbnailReady = 'thrd';
class ThumbnailLoader : public Listi::ReadWorker {
  BMessenger target;
  std::shared_ptr<TerminalReplies> terminal;
  std::shared_ptr<BitmapMailbox> replies;
  std::shared_ptr<std::atomic<int64>> generation;

public:
  ThumbnailLoader(BMessenger t, std::shared_ptr<std::atomic<int64>> g,
                  std::shared_ptr<BitmapMailbox> r, std::shared_ptr<TerminalReplies> queue)
      : ReadWorker("thumbnail reader"), target(t), terminal(queue), replies(r), generation(g) {}
  void MessageReceived(BMessage *m) override {
    if (m->what != kThumbnail) {
      BLooper::MessageReceived(m);
      return;
    }
    int64 serial = m->GetInt64("serial", -1);
    Listi::TransferControl c;
    c.proceed = [&] { return generation->load() == serial; };
    BBitmap *bitmap = nullptr;
    try {
      c.Check();
      bitmap = LoadPreviewBitmap(m->GetString("path", ""), c, 128);
    } catch (const Listi::Cancelled &) {
      return;
    } catch (const std::exception &) {
    }
    BMessage done(kThumbnailReady);
    done.AddString("path", m->GetString("path", ""));
    done.AddInt64("serial", serial);
    auto reply = replies->Put(bitmap);
    done.AddInt64("bitmap", reply);
    terminal->Deliver(done, *generation, [&] {
      return target.SendMessage(&done, (BHandler *)nullptr, 50000);
    });
  }
};
} // namespace
class GridView : public FileGrid {
  std::shared_ptr<TerminalReplies> terminal = std::make_shared<TerminalReplies>();
  Pane *pane;
  PaneModel *model;
  int mode = 1;
  float cellWidth = 112, cellHeight = 100;
  Listi::ReadWorker *loader = nullptr;
  std::shared_ptr<BitmapMailbox> replies = std::make_shared<BitmapMailbox>(64);
  std::shared_ptr<std::atomic<int64>> generation;
  std::map<std::string, std::unique_ptr<BBitmap>> thumbnails;
  std::set<std::string> pending;
  bool retrySubmission = false;
  bigtime_t nextReaderAttempt = 0;
  RubberBand *band;
  Listi::TypeToSelect typeSelect;
  std::set<std::string> paintedSelection;
  std::string paintedFocus;
  BPoint pressed;
  int pressedIndex = -1;
  bool dragging = false, deferredPick = false;
  int Columns() const {
    return std::max(1, int((Bounds().Width() + 1) / cellWidth));
  }
  BRect Cell(int index) const {
    int cols = Columns();
    float width = (Bounds().Width() + 1) / cols;
    float left = index % cols * width, top = index / cols * cellHeight;
    return BRect(left, top, left + width - 1, top + cellHeight - 1);
  }
  bool Selected(PaneEntry *row) const { return model->IsSelected(row->path); }
  void Pick(int index, bool extend, bool toggle = false) {
    if (index < 0 || index >= int(model->Visible().size()))
      return;
    model->Pick(index, extend, toggle);
    BRect cell = Cell(index);
    if (cell.top < Bounds().top)
      ScrollTo(0, cell.top);
    else if (cell.bottom > Bounds().bottom)
      ScrollTo(0, cell.bottom - Bounds().Height());
    pane->SendSelection();
    Invalidate();
  }

public:
  PaneEntry *ScrollRow(const std::string *path, float &top) override {
    const auto &rows = model->Visible();
    for (size_t i = 0; i < rows.size(); ++i) {
      top = Cell(i).top;
      if (path ? rows[i]->path == *path : top + cellHeight > Bounds().top)
        return rows[i];
    }
    return nullptr;
  }
  void SetActive(bool active) override {
    const auto background =
        Listi::PanelBackground(B_DOCUMENT_BACKGROUND_COLOR, active);
    if (ViewColor() == background)
      return;
    SetViewColor(background);
    SetLowColor(background);
    Invalidate();
  }
  GridView(Pane *p, PaneModel *state)
      : FileGrid("file grid", B_PULSE_NEEDED | B_WILL_DRAW | B_NAVIGABLE | B_FRAME_EVENTS |
                                  B_FULL_UPDATE_ON_RESIZE),
        pane(p), model(state),
        generation(std::make_shared<std::atomic<int64>>(0)) {
    // The scroll container owns geometry. Current viewport size must not
    // become a growing layout minimum when an unnecessary bar is reclaimed.
    SetExplicitMinSize(BSize(0, 0));
    SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    band = new RubberBand(this);
    band->ready = [this] { return !pane->IsScanning(); };
    band->path = [this] { return std::string(pane->Path()); };
    band->selection = [this] { return model->Selected(); };
    band->restoreSelection = [this](const auto &paths) {
      model->SetSelection(paths, paths.empty() ? "" : paths.back());
      pane->SendSelection(true);
    };
    band->finished = [this] { pane->SendSelection(); };
    band->activate = [this] {
      MakeFocus();
      pressedIndex = -1;
      deferredPick = false;
    };
    band->outline = [this](BRect rect) {
      PushState();
      SetDrawingMode(B_OP_INVERT);
      StrokeRect(rect);
      PopState();
    };
    band->empty = [this](BPoint p) {
      for (int i = 0; i < int(model->Visible().size()); ++i)
        if (Cell(i).InsetByCopy(4, 4).Contains(p))
          return false;
      return true;
    };
    band->select = [this](BRect rect, const auto &base) {
      auto paths = base;
      for (int i = 0; i < int(model->Visible().size()); ++i) {
        auto entry = model->Visible()[i];
        if (!entry->parent && rect.Intersects(Cell(i).InsetByCopy(4, 4)))
          paths.push_back(entry->path);
      }
      const std::string focus = paths.empty() ? "" : paths.back();
      std::sort(paths.begin(), paths.end());
      paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
      auto current = model->Selected();
      std::sort(current.begin(), current.end());
      if (paths != current) {
        model->SetSelection(paths, focus);
        pane->SendSelection(true);
      }
    };
    AddFilter(band);
    SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
    SetLowUIColor(B_DOCUMENT_BACKGROUND_COLOR);
    SetHighUIColor(B_DOCUMENT_TEXT_COLOR);
  }
  ~GridView() {
    generation->store(-1);
    Listi::RetireReadWorker(loader);
  }
  void AttachedToWindow() override {
    loader = new ThumbnailLoader(BMessenger(this), generation, replies, terminal);
    nextReaderAttempt = Listi::StartReadWorker(loader) ? 0 : system_time() + 250000;
    Update();
  }
  void DetachedFromWindow() override {
    ++*generation;
    Listi::RetireReadWorker(loader);
    pending.clear();
    terminal->Take();
    replies->Clear();
    BView::DetachedFromWindow();
  }
  bool CancelSelectionGesture() override { return band->Escape(); }
  void Reset() {
    band->Cancel();
    typeSelect.Reset();
    paintedSelection.clear();
    paintedFocus.clear();
    pressedIndex = -1;
    dragging = deferredPick = false;
    ++*generation;
    terminal->Take();
    replies->Clear();
    pending.clear();
    thumbnails.clear();
    ScrollTo(0, 0);
    Update();
  }
  void SetMode(int value) {
    if (mode != value) {
      mode = value;
      Reset();
    }
    cellWidth = mode == 1 ? 112 : 156;
    cellHeight = mode == 1 ? 100 : 154;
    Update();
  }
  void SyncSelection() override {
    auto focused = model->Focused();
    std::string focus = focused ? focused->path : "";
    for (int i = 0; i < int(model->Visible().size()); ++i) {
      const auto &path = model->Visible()[i]->path;
      bool selected = model->IsSelected(path);
      if (selected != bool(paintedSelection.count(path)) ||
          (focus != paintedFocus && (path == focus || path == paintedFocus)))
        Invalidate(Cell(i));
      if (selected)
        paintedSelection.insert(path);
      else
        paintedSelection.erase(path);
    }
    paintedFocus = focus;
  }
  float ContentHeight(float width) const override {
    const int columns = std::max(1, int((width + 1) / cellWidth));
    return ((int(model->Visible().size()) + columns - 1) / columns) *
           cellHeight;
  }
  void Update() {
    if (Parent())
      Parent()->InvalidateLayout();
    if (auto bar = ScrollBar(B_VERTICAL)) {
      float height =
          ((int(model->Visible().size()) + Columns() - 1) / Columns()) *
          cellHeight;
      float visible = Bounds().Height() + 1;
      bar->SetRange(0, std::max(0.f, height - visible));
      bar->SetProportion(height > 0 ? std::min(1.f, visible / height) : 1.f);
      bar->SetSteps(cellHeight, visible);
    }
    Invalidate();
  }
  void RedrawEntry(const std::string &path) override {
    for (size_t i = 0; i < model->Visible().size(); ++i)
      if (model->Visible()[i]->path == path) {
        Invalidate(Cell(i));
        return;
      }
  }
  void FrameResized(float, float) override {
    band->Cancel();
    Update();
  }
  void MakeFocus(bool focus = true) override {
    if (!focus)
      typeSelect.Reset();
    BView::MakeFocus(focus);
    if (focus)
      pane->Activate();
    Invalidate();
  }
  void MouseDown(BPoint point) override {
    typeSelect.Reset();
    MakeFocus();
    int index = int(point.y / cellHeight) * Columns() +
                std::min(Columns() - 1,
                         int(point.x / ((Bounds().Width() + 1) / Columns())));
    if (Window()->CurrentMessage()->GetInt32("buttons", 0) &
        B_SECONDARY_MOUSE_BUTTON) {
      pressedIndex = -1;
      dragging = deferredPick = false;
      if (index >= 0 && index < int(model->Visible().size()) &&
          !model->Visible()[index]->parent) {
        const auto path = model->Visible()[index]->path;
        pane->ContextMenu(path, ConvertToScreen(point));
      } else
        pane->ContextMenu("", ConvertToScreen(point));
      return;
    }
    int32 mods = modifiers();
    int32 clicks = Window()->CurrentMessage()->GetInt32("clicks", 1);
    pressed = point;
    pressedIndex = index;
    dragging = false;
    deferredPick = index >= 0 && index < int(model->Visible().size()) &&
                   model->IsSelected(model->Visible()[index]->path) &&
                   !(mods & (B_SHIFT_KEY | B_COMMAND_KEY | B_CONTROL_KEY)) &&
                   clicks == 1;
    if (!deferredPick)
      Pick(index, mods & B_SHIFT_KEY, mods & (B_COMMAND_KEY | B_CONTROL_KEY));
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    if (clicks == 2 && index >= 0 && index < int(model->Visible().size())) {
      BMessage invoke(kInvoke);
      pane->MessageReceived(&invoke);
    }
  }
  std::string hoverPath;
  std::string DropTarget(BPoint point) const {
    if (!Bounds().Contains(point) || pane->IsSearchList())
      return {};
    const int index = int(point.y / cellHeight) * Columns() +
        int(point.x / ((Bounds().Width() + 1) / Columns()));
    const auto &rows = model->Visible();
    return Listi::FolderDropTarget(pane->Path(),
        index >= 0 && index < int(rows.size()) ? rows[index] : nullptr);
  }
  void MouseMoved(BPoint point, uint32 transit, const BMessage *payload) override {
    Listi::SetFolderHover(this, hoverPath,
        Listi::IsFileHover(transit, payload) ? DropTarget(point) : "");
    if (payload)
      return;
    if (dragging || pressedIndex < 0 ||
        pressedIndex >= int(model->Visible().size()) ||
        (std::abs(point.x - pressed.x) < 5 &&
         std::abs(point.y - pressed.y) < 5))
      return;
    BPoint current;
    uint32 buttons;
    GetMouse(&current, &buttons);
    if (!(buttons & B_PRIMARY_MOUSE_BUTTON))
      return;
    BMessage drag;
    try {
      drag = Listi::MakeFileDrag(
          model->Selected(), model->SelectedLocalIdentities(Listi::kFileSelectionLimit));
    } catch (const std::exception &e) {
      pane->ShowStatus(e.what());
      return;
    }
    dragging = true;
    DragMessage(&drag, BRect(point.x, point.y, point.x + 90, point.y + 22));
  }
  void MouseUp(BPoint) override {
    if (deferredPick && !dragging)
      Pick(pressedIndex, false);
    pressedIndex = -1;
    dragging = deferredPick = false;
  }
  void KeyDown(const char *bytes, int32 length) override {
    const auto &visible = model->Visible();
    const uint32 eventModifiers = Listi::TypeSelectModifiers(*this);
    int match;
    if (length > 0 && !pane->IsScanning() &&
        typeSelect.KeyDown({bytes, size_t(length)},
                           eventModifiers, system_time(),
                           pane->Path(), visible.size(), model->Focused(),
                           [&](int index) { return visible[index]; }, match)) {
      Pick(match, false);
      return;
    }
    if (length == 1) {
      int index = 0;
      auto focused = model->Focused();
      for (int i = 0; i < int(model->Visible().size()); ++i)
        if (model->Visible()[i] == focused) {
          index = i;
          break;
        }
      int change = 0;
      switch (bytes[0]) {
      case B_LEFT_ARROW:
        change = -1;
        break;
      case B_RIGHT_ARROW:
        change = 1;
        break;
      case B_UP_ARROW:
        change = -Columns();
        break;
      case B_DOWN_ARROW:
        change = Columns();
        break;
      case B_HOME:
        Pick(0, false);
        return;
      case B_END:
        Pick(int(model->Visible().size()) - 1, false);
        return;
      case B_ENTER: {
        BMessage invoke(kInvoke);
        pane->MessageReceived(&invoke);
        return;
      }
      case B_TAB:
        Window()->PostMessage(kSwitch);
        return;
      case B_BACKSPACE: {
        BMessage parent(kParent);
        pane->MessageReceived(&parent);
        return;
      }
      case ' ':
        Pick(index, true, true);
        return;
      }
      if (change) {
        Pick(std::max(
                 0, std::min(int(model->Visible().size()) - 1, index + change)),
             eventModifiers & B_SHIFT_KEY);
        return;
      }
    }
    BView::KeyDown(bytes, length);
  }
  void Pulse() override {
    if (retrySubmission && pending.size() < 64 &&
        (loader || system_time() >= nextReaderAttempt)) {
      retrySubmission = false;
      Invalidate();
    }
    for (auto &reply : terminal->Take())
      MessageReceived(&reply);
  }
  void MessageReceived(BMessage *m) override {
    if (m->what == kThumbnailReady) {
      auto owned = replies->Take(m->GetInt64("bitmap", -1));
      auto bitmap = owned.release();
      if (m->GetInt64("serial", -1) != generation->load()) {
        delete bitmap;
        return;
      }
      std::string path = m->GetString("path", "");
      pending.erase(path);
      if (thumbnails.size() >= 256)
        thumbnails.erase(thumbnails.begin());
      thumbnails[path].reset(bitmap);
      RedrawEntry(path);
      return;
    }
    if (m->WasDropped()) {
      BMessage dropped(*m);
      dropped.what = kDrop;
      dropped.RemoveName("destination");
      const auto target = DropTarget(ConvertFromScreen(m->DropPoint()));
      Listi::SetFolderHover(this, hoverPath, "");
      dropped.AddString("destination", target.empty() ? pane->Path() : target.c_str());
      Window()->PostMessage(&dropped);
      return;
    }
    BView::MessageReceived(m);
  }
  void Draw(BRect update) override {
    int first = std::max(0, int(update.top / cellHeight) * Columns()),
        last = std::min(int(model->Visible().size()),
                        (int(update.bottom / cellHeight) + 1) * Columns());
    auto focused = model->Focused();
    for (int i = first; i < last; ++i) {
      auto row = model->Visible()[i];
      auto cell = Cell(i).InsetByCopy(4, 4);
      bool selected = Selected(row);
      if (selected)
        SetHighUIColor(B_LIST_SELECTED_BACKGROUND_COLOR);
      else
        SetHighColor(ViewColor());
      FillRoundRect(cell, 4, 4);
      pane->RequestVisibleIcon(*row);
      BBitmap *picture = row->icon.get();
      if (mode == 3 && !row->directory && !row->parent) {
        auto found = thumbnails.find(row->path);
        if (found != thumbnails.end()) {
          if (found->second)
            picture = found->second.get();
        } else if (!pending.count(row->path)) {
          std::string lower = row->name.c_str();
          std::transform(lower.begin(), lower.end(), lower.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          auto dot = lower.find_last_of('.');
          auto ext = dot == std::string::npos ? "" : lower.substr(dot);
          if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
              ext == ".gif" || ext == ".bmp" || ext == ".webp" ||
              ext == ".tiff" || ext == ".tif" || ext == ".avif" ||
              ext == ".heif" || ext == ".svg" || ext == ".ico" ||
              ext == ".tga") {
            if (pending.size() >= 64) {
              // A completion may belong to a cell no longer on screen.
              // Retry the current viewport when the queue has room again.
              retrySubmission = true;
            } else if (!loader && Window() && system_time() >= nextReaderAttempt) {
              loader = new ThumbnailLoader(BMessenger(this), generation, replies, terminal);
              if (!Listi::StartReadWorker(loader))
                nextReaderAttempt = system_time() + 250000;
            }
            if (loader && pending.size() < 64) {
              BMessage job(kThumbnail);
              job.AddString("path", row->path.c_str());
              job.AddInt64("serial", generation->load());
              if (loader->PostMessage(&job) == B_OK)
                pending.insert(row->path);
              else
                retrySubmission = true;
            } else {
              retrySubmission = true;
            }
          }
        }
      }
      if (picture) {
        float available = mode == 1 ? 48 : 104;
        auto src = picture->Bounds();
        float scale = std::min(available / (src.Width() + 1),
                               available / (src.Height() + 1));
        float w = (src.Width() + 1) * scale, h = (src.Height() + 1) * scale;
        BRect dest(cell.left + (cell.Width() - w) / 2,
                   cell.top + 4 + (available - h) / 2, 0, 0);
        dest.right = dest.left + w - 1;
        dest.bottom = dest.top + h - 1;
        SetDrawingMode(B_OP_ALPHA);
        DrawBitmap(picture, src, dest);
        SetDrawingMode(B_OP_COPY);
      }
      SetHighUIColor(selected ? B_LIST_SELECTED_ITEM_TEXT_COLOR
                              : B_DOCUMENT_TEXT_COLOR);
      BString label(row->name.c_str());
      TruncateString(&label, B_TRUNCATE_MIDDLE, cell.Width() - 8);
      DrawString(label,
                 BPoint(cell.left + (cell.Width() - StringWidth(label)) / 2,
                        cell.bottom - 10));
      if (row->transferState) {
        SetHighUIColor(row->transferState == 2   ? B_SUCCESS_COLOR
                       : row->transferState == 3 ? B_FAILURE_COLOR
                                                 : B_STATUS_BAR_COLOR);
        float width =
            std::max(5.f, (cell.Width() - 8) *
                              (row->transferState == 1 ? row->fraction : 1.f));
        FillRect(BRect(cell.left + 4, cell.bottom - 3, cell.left + 4 + width,
                       cell.bottom - 1));
      }
      if (row->path == hoverPath)
        Listi::DrawFolderHover(this, cell);
      if (IsFocus() && focused == row) {
        SetHighUIColor(B_KEYBOARD_NAVIGATION_COLOR);
        StrokeRoundRect(cell, 4, 4);
      }
    }
    if (band->Active()) {
      SetDrawingMode(B_OP_INVERT);
      StrokeRect(band->Rect());
      SetDrawingMode(B_OP_COPY);
    }
  }
};

FileGrid *CreateFileGrid(Pane *pane, PaneModel *model) {
  return new GridView(pane, model);
}
