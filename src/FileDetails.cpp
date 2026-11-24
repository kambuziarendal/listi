// SPDX-License-Identifier: MIT
#include "FileDrop.h"
#include "FolderDropTarget.h"
#include "AdaptiveColumnList.h"
// Only this renderer depends on Haiku's private ColumnListView API.
#include "FileDetails.h"
#include "PanelAppearance.h"
#include "RubberBand.h"
#include "TypeToSelect.h"
#include <Region.h>
#include <StringForSize.h>
#include "Pane.h"
#include "PaneModel.h"
#include "VirtualFile.h"
#include <Bitmap.h>
#include <MessageFilter.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <Entry.h>
#include <LayoutBuilder.h>
#include <Window.h>
#include <algorithm>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <strings.h>
namespace {
struct NameField : BStringField {
  PaneEntry *entry;
  explicit NameField(PaneEntry *e) : BStringField(e->name.c_str()), entry(e) {}
};
struct FileRow : BRow {
  PaneEntry *entry;
  FileRow(PaneEntry *e, float height) : BRow(height), entry(e) {}
};
struct AttributeField : BStringField {
  enum Kind { Unavailable, Number, Text };
  Kind kind = Unavailable;
  long double number = 0;

  template <class T> void ReadNumber(const std::string &text) {
    T value{};
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    if (input >> value) {
      number = value;
      kind = Number;
    }
  }

  explicit AttributeField(const AttributeCellPreview &cell)
      : BStringField(cell.text.c_str()) {
    // Parse once in the stored precision, not during sorting. Haiku x86_64's
    // long double can compare finite doubles and exact 64-bit integers.
    switch (cell.type) {
    case B_INT32_TYPE:
    case B_INT64_TYPE:
      ReadNumber<int64>(cell.text);
      break;
    case B_UINT32_TYPE:
    case B_UINT64_TYPE:
      ReadNumber<uint64>(cell.text);
      break;
    case B_FLOAT_TYPE:
      ReadNumber<float>(cell.text);
      break;
    case B_DOUBLE_TYPE:
      ReadNumber<double>(cell.text);
      break;
    case B_STRING_TYPE:
    case B_BOOL_TYPE:
      kind = Text;
      break;
    }
  }
};

class AttributeColumn : public BStringColumn {
public:
  explicit AttributeColumn(const char *name)
      : BStringColumn(name, 150, 55, 800, B_TRUNCATE_END) {}

  int CompareFields(BField *a, BField *b) override {
    const auto &left = *static_cast<AttributeField *>(a);
    const auto &right = *static_cast<AttributeField *>(b);
    if (left.kind != right.kind)
      return left.kind < right.kind ? -1 : 1;
    if (left.kind == AttributeField::Number)
      return (left.number > right.number) - (left.number < right.number);
    return BStringColumn::CompareFields(a, b);
  }
};

class NameColumn : public BStringColumn {
public:
  Preferences prefs;
  std::string hoverPath;
  BView *batchTarget = nullptr;
  Pane *pane;
  NameColumn(Pane *owner, const Preferences &p)
      : BStringColumn("Name", 260, 80, 1200, B_TRUNCATE_MIDDLE), prefs(p), pane(owner) {}
  void DrawField(BField *f, BRect r, BView *v) override {
    const BRect fieldRect = r;
    v->SetFont(&prefs.font);
    auto n = static_cast<NameField *>(f);
    rgb_color old = v->HighColor();
    if (n->entry->parent)
      v->SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
    else
      v->SetHighColor(prefs.Color(n->entry->kind == 1   ? prefs.folders
                                  : n->entry->kind == 2 ? prefs.links
                                                        : prefs.files));
    if (prefs.icons) {
      pane->RequestVisibleIcon(*n->entry);
      if (n->entry->icon) {
        v->SetDrawingMode(B_OP_ALPHA);
        float top = r.top + (r.Height() - 16) / 2;
        BRect destination(r.left + 3, top, r.left + 18, top + 15);
        if (v == batchTarget)
          v->DrawBitmapAsync(n->entry->icon.get(), n->entry->icon->Bounds(),
                             destination);
        else
          v->DrawBitmap(n->entry->icon.get(), n->entry->icon->Bounds(),
                        destination);
        v->SetDrawingMode(B_OP_COPY);
      }
      r.left += 23;
    }
    if (n->entry->transferState == 3) {
      const char *failed = "Failed";
      const float width = v->StringWidth(failed) + 8;
      v->DrawString(failed, BPoint(r.right - width, r.top + r.Height() / 2 + prefs.font.Size() / 3));
      r.right -= width;
    }
    BStringColumn::DrawField(f, r, v);
    if (hoverPath == n->entry->path)
      Listi::DrawFolderHover(v, fieldRect);
    if (n->entry->transferState) {
      rgb_color color =
          ui_color(n->entry->transferState == 2   ? B_SUCCESS_COLOR
                   : n->entry->transferState == 3 ? B_FAILURE_COLOR
                                                  : B_STATUS_BAR_COLOR);
      v->SetHighColor(
          tint_color(color, n->entry->transferState == 1 ? 1.2f : 1.f));
      float width = std::max(5.f, r.Width() * (n->entry->transferState == 1
                                                   ? n->entry->fraction
                                                   : 1.f));
      v->FillRect(BRect(r.left, r.bottom - 2, r.left + width, r.bottom));
    }
    v->SetHighColor(old);
  }
  int CompareFields(BField *a, BField *b) override {
    auto x = static_cast<NameField *>(a), y = static_cast<NameField *>(b);
    if (x->entry->parent != y->entry->parent)
      return x->entry->parent ? -1 : 1;
    if ((x->entry->kind == 1) != (y->entry->kind == 1))
      return x->entry->kind == 1 ? -1 : 1;
    return strcasecmp(x->String(), y->String());
  }
};
struct DateField : BDateField {
  uint64 fontRevision = 0;
  explicit DateField(time_t *value) : BDateField(value) {}
};
class DateColumn : public BDateColumn {
public:
  BFont font;
  uint64 fontRevision = 1;
  explicit DateColumn(const BFont &value)
      : BDateColumn("Modified", 160, 80, 280), font(value) {}
  void DrawField(BField *field, BRect rect, BView *view) override {
    auto date = static_cast<DateField *>(field);
    view->SetFont(&font);
    if (date->fontRevision != fontRevision)
      date->SetWidth(-1);
    BDateColumn::DrawField(field, rect, view);
    // Haiku's date column tests the full cell width, but caches the width
    // minus its text margins. Normalize the key so selection repainting does
    // not rerun locale formatting for every visible date on every frame.
    date->SetWidth(rect.Width());
    date->fontRevision = fontRevision;
  }
};
struct SizeField : BSizeField {
  BString label, clipped;
  uint64 fontRevision = 0;
  float width = -1;
  bool formatted = false;
  explicit SizeField(off_t size) : BSizeField(size) {}
};
class SizeColumn : public BSizeColumn {
public:
  BFont font;
  uint64 fontRevision = 1;
  explicit SizeColumn(const BFont &value)
      : BSizeColumn("Size", 85, 50, 200), font(value) {}
  void DrawField(BField *field, BRect rect, BView *view) override {
    auto size = static_cast<SizeField *>(field);
    if (size->Size() < 0)
      return;
    if (!size->formatted) {
      char text[128];
      size->label = string_for_size(size->Size(), text, sizeof(text));
      size->formatted = true;
    }
    view->SetFont(&font);
    if (size->width != rect.Width() || size->fontRevision != fontRevision) {
      size->clipped = size->label;
      view->TruncateString(&size->clipped, B_TRUNCATE_END,
                           std::max(0.f, rect.Width() - 16));
      size->width = rect.Width();
      size->fontRevision = fontRevision;
    }
    DrawString(size->clipped.String(), view, rect);
  }
};

class ContextClick : public BMessageFilter {
  BColumnListView *list;
  Pane *pane;
  Listi::TypeToSelect &typeSelect;

public:
  ContextClick(BColumnListView *view, Pane *owner, Listi::TypeToSelect &typing)
      : BMessageFilter(B_MOUSE_DOWN), list(view), pane(owner),
        typeSelect(typing) {}
  filter_result Filter(BMessage *message, BHandler **) override {
    typeSelect.Reset();
    if (!(message->GetInt32("buttons", 0) & B_SECONDARY_MOUSE_BUTTON))
      return B_DISPATCH_MESSAGE;
    BPoint point;
    if (message->FindPoint("where", &point) != B_OK)
      return B_SKIP_MESSAGE;
    auto row = static_cast<FileRow *>(list->RowAt(point));
    if (row && !row->entry->parent) {
      const auto path = row->entry->path;
      list->MakeFocus();
      pane->ContextMenu(path, list->ScrollView()->ConvertToScreen(point));
    } else {
      list->MakeFocus();
      pane->ContextMenu("", list->ScrollView()->ConvertToScreen(point));
    }
    return B_SKIP_MESSAGE;
  }
};
std::string DetailsDropTarget(BColumnListView *list, Pane *pane, BPoint point) {
  auto row = static_cast<FileRow *>(list->RowAt(point));
  return pane->IsSearchList() || !list->ColumnAt(point) ? std::string()
      : Listi::FolderDropTarget(pane->Path(), row ? row->entry : nullptr);
}

class FolderHoverFilter : public BMessageFilter {
  BColumnListView *list;
  Pane *pane;
  NameColumn *name;

public:
  FolderHoverFilter(BColumnListView *list, Pane *pane, NameColumn *name)
      : BMessageFilter(B_MOUSE_MOVED), list(list), pane(pane), name(name) {}
  filter_result Filter(BMessage *message, BHandler **) override {
    BMessage drag;
    BPoint point;
    bool located = message->FindPoint("be:view_where", &point) == B_OK;
    if (!located && message->FindPoint("screen_where", &point) == B_OK) {
      point = list->ScrollView()->ConvertFromScreen(point);
      located = true;
    }
    const bool hover = located && list->ScrollView()->Bounds().Contains(point) &&
        message->FindMessage("be:drag_message", &drag) == B_OK &&
        Listi::IsFileHover(message->GetInt32("be:transit", B_INSIDE_VIEW), &drag);
    Listi::SetFolderHover(list->ScrollView(), name->hoverPath,
                          hover ? DetailsDropTarget(list, pane, point) : "");
    return B_DISPATCH_MESSAGE;
  }
};
class DetailsList : public Listi::AdaptiveColumnList {
  Listi::TypeToSelect typeSelect;

public:
  Pane *pane;
  PaneModel *model;
  NameColumn *name;
  SizeColumn *size;
  DateColumn *date;
  bool syncing = false, showModified = true;
  // Row heights may differ after a font change. Maintain their sum instead
  // of walking the growing native list on every incoming scan batch.
  double rowsHeight = 0;
  float ContentHeight() override { return static_cast<float>(rowsHeight); }
  RubberBand *band;
  DetailsList(Pane *p, PaneModel *state, const Preferences &prefs)
      : AdaptiveColumnList("files", B_WILL_DRAW | B_NAVIGABLE | B_DRAW_ON_CHILDREN,
                        B_FANCY_BORDER),
        pane(p), model(state) {
    name = new NameColumn(p, prefs);
    name->batchTarget = ScrollView();
    AddColumn(name, 0);
    size = new SizeColumn(prefs.font);
    date = new DateColumn(prefs.font);
    AddColumn(size, 1);
    AddColumn(date, 2);
    SetSelectionMode(B_MULTIPLE_SELECTION_LIST);
    SetSortColumn(name, false, true);
    SetInvocationMessage(new BMessage(kInvoke));
    ScrollView()->AddFilter(new ContextClick(this, pane, typeSelect));
    ScrollView()->AddFilter(new FolderHoverFilter(this, pane, name));
    band = new RubberBand(ScrollView());
    band->ready = [this] { return !pane->IsScanning(); };
    band->path = [this] { return std::string(pane->Path()); };
    band->selection = [this] { return model->Selected(); };
    band->restoreSelection = [this](const auto &paths) {
      model->SetSelection(paths, paths.empty() ? "" : paths.back());
      pane->SendSelection(true);
    };
    band->finished = [this] { pane->SendSelection(); };
    band->activate = [this] { MakeFocus(); };
    band->outline = [this](BRect rect) {
      auto view = ScrollView();
      view->PushState();
      view->SetDrawingMode(B_OP_INVERT);
      view->StrokeRect(rect);
      view->PopState();
    };
    band->empty = [this](BPoint point) {
      return RowAt(point) == nullptr || ColumnAt(point) == nullptr;
    };
    band->select = [this](BRect rect, const auto &base) {
      auto paths = base;
      // Flat rows in current native sort order. GetRowRect walks from the
      // first row, so calling it for every row makes each frame quadratic.
      float top = 0;
      auto bounds = ScrollView()->Bounds();
      for (int32 i = 0; i < CountRows(); ++i) {
        auto row = static_cast<FileRow *>(RowAt(i));
        BRect frame(bounds.left, top, bounds.right, top + row->Height());
        top = frame.bottom + 1;
        if (!row->entry->parent && rect.Intersects(frame))
          paths.push_back(row->entry->path);
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
    ScrollView()->AddFilter(band);
  }
  void DoLayout() override {
    const float room = Bounds().Width() - B_V_SCROLL_BAR_WIDTH - 12;
    const float needed = name->Width() + date->Width() +
                         (size->IsVisible() ? size->Width() : 0);
    date->SetVisible(showModified && room >= needed);
    AdaptiveColumnList::DoLayout();
  }
  void DrawAfterChildren(BRect) override {
    // One completion barrier for all row icons, before model-owned bitmaps can
    // be replaced. Native resize-buffer drawing remains synchronous.
    Sync();
    if (!band->Active())
      return;
    auto view = ScrollView();
    BRect clip = ConvertFromScreen(view->ConvertToScreen(view->Bounds()));
    BRegion region(clip);
    ConstrainClippingRegion(&region);
    SetDrawingMode(B_OP_INVERT);
    StrokeRect(ConvertFromScreen(view->ConvertToScreen(band->Rect())));
    SetDrawingMode(B_OP_COPY);
    ConstrainClippingRegion(nullptr);
  }
  void AttachedToWindow() override {
    BColumnListView::AttachedToWindow();
    SetTarget(this);
  }
  void MessageReceived(BMessage *message) override {
    if (message->what == kInvoke) {
      auto row = static_cast<FileRow *>(FocusRow());
      model->SetSelection(model->Selected(), row ? row->entry->path : "");
      pane->MessageReceived(message);
      return;
    }
    AdaptiveColumnList::MessageReceived(message);
  }
  void MakeFocus(bool focus = true) override {
    if (!focus)
      typeSelect.Reset();
    BColumnListView::MakeFocus(focus);
    if (focus)
      pane->Activate();
  }
  void SelectionChanged() override {
    BColumnListView::SelectionChanged();
    if (syncing)
      return;
    std::vector<std::string> selected;
    for (auto r = CurrentSelection(); r; r = CurrentSelection(r)) {
      auto row = static_cast<FileRow *>(r);
      if (!row->entry->parent)
        selected.push_back(row->entry->path);
    }
    auto focus = static_cast<FileRow *>(FocusRow());
    model->SetSelection(selected, focus ? focus->entry->path : "");
    if (!pane->IsScanning()) {
      pane->Activate();
      pane->SendSelection();
    }
  }
  void KeyDown(const char *bytes, int32 length) override {
    int match;
    if (length > 0 && !pane->IsScanning() &&
        typeSelect.KeyDown(
            {bytes, size_t(length)}, Listi::TypeSelectModifiers(*this),
            system_time(), pane->Path(), CountRows(), model->Focused(),
            [this](int index) {
              const auto row = static_cast<FileRow *>(RowAt(index));
              return row ? row->entry : nullptr;
            },
            match)) {
      if (match >= 0) {
        auto row = static_cast<FileRow *>(RowAt(match));
        model->SetSelection({row->entry->path}, row->entry->path);
        pane->SendSelection();
        ScrollTo(row);
      }
      return;
    }
    if (length == 1 && bytes[0] == B_TAB) {
      Window()->PostMessage(kSwitch);
      return;
    }
    if (length == 1 && bytes[0] == B_BACKSPACE) {
      BMessage parent(kParent);
      pane->MessageReceived(&parent);
      return;
    }
    if (length == 1 && bytes[0] == ' ') {
      auto row = static_cast<FileRow *>(FocusRow());
      if (row) {
        if (model->IsSelected(row->entry->path))
          Deselect(row);
        else
          AddToSelection(row);
      }
      return;
    }
    BColumnListView::KeyDown(bytes, length);
  }
  bool InitiateDrag(BPoint point, bool) override {
    BMessage drag;
    try {
      drag = Listi::MakeFileDrag(
          model->Selected(), model->SelectedLocalIdentities(Listi::kFileSelectionLimit));
    } catch (const std::exception &e) {
      pane->ShowStatus(e.what());
      return false;
    }
    DragMessage(&drag, BRect(point.x, point.y, point.x + 90, point.y + 22));
    return true;
  }
  void MessageDropped(BMessage *message, BPoint point) override {
    Listi::SetFolderHover(ScrollView(), name->hoverPath, "");
    if (pane->IsSearchList()) {
      pane->ShowStatus("A search list is not a destination folder.");
      return;
    }
    BMessage drop(*message);
    drop.what = kDrop;
    drop.RemoveName("destination");
    const auto target = DetailsDropTarget(this, pane, point);
    drop.AddString("destination", target.empty() ? pane->Path() : target.c_str());
    Window()->PostMessage(&drop);
  }
};
class DetailsView : public FileDetails {
  PaneModel *model;
  DetailsList *list;
  std::map<std::string, FileRow *> rows;
  Preferences prefs;
  bool active = false;
  std::vector<BColumn *> attributes;

public:
  DetailsView(Pane *pane, PaneModel *state, const Preferences &p)
      : FileDetails("file details", B_WILL_DRAW | B_SUPPORTS_LAYOUT),
        model(state), prefs(p) {
    list = new DetailsList(pane, state, p);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0).Add(list);
  }
  ~DetailsView() { list->syncing = true; }
  PaneSort Sorting() override {
    // SaveState is the native API for inspecting header-selected sort order.
    BMessage state;
    list->SaveState(&state);
    return {state.GetInt32("sortID", 0), state.GetBool("sortascending", true)};
  }
  void Sort(PaneSort value) override {
    BColumn *column = value.column == 1   ? static_cast<BColumn *>(list->size)
                      : value.column == 2 ? static_cast<BColumn *>(list->date)
                                          : list->name;
    list->SetSortColumn(column, false, value.ascending);
    SyncSelection();
  }
  bool CancelSelectionGesture() override { return list->band->Escape(); }
  void MakeFocus(bool focus = true) override { list->MakeFocus(focus); }
  PaneEntry *ScrollRow(const std::string *path, float &top) override {
    top = 0;
    for (int32 i = 0; i < list->CountRows(); ++i) {
      auto row = static_cast<FileRow *>(list->RowAt(i));
      const float next = top + row->Height() + 1;
      if (path ? row->entry->path == *path : next > ScrollPosition().y)
        return row->entry;
      top = next;
    }
    return nullptr;
  }
  BPoint ScrollPosition() override { return list->ScrollView()->Bounds().LeftTop(); }
  void RestoreScroll(BPoint p) override {
    static_cast<BScrollBar *>(list->FindView("horizontal_scroll_bar"))->SetValue(p.x);
    static_cast<BScrollBar *>(list->FindView("vertical_scroll_bar"))->SetValue(p.y);
  }
  void Redraw() override { list->ScrollView()->Invalidate(); }
  void RedrawEntry(const std::string &path) override {
    auto found = rows.find(path);
    if (found != rows.end())
      list->UpdateRow(found->second);
  }
  void SetActive(bool value) override {
    active = value;
    const auto color = Listi::PanelBackground(B_PANEL_BACKGROUND_COLOR, active);
    if (list->Color(B_COLOR_BACKGROUND) != color)
      list->SetColor(B_COLOR_BACKGROUND, color);
  }
  void Apply(const Preferences &p) override {
    prefs = p;
    list->name->prefs = p;
    if (list->size->font != p.font) {
      list->size->font = p.font;
      list->date->font = p.font;
      ++list->size->fontRevision;
      ++list->date->fontRevision;
    }
    list->SetFont(&p.font);
    list->size->SetVisible(p.size);
    list->showModified = p.date;
    list->SetColor(B_COLOR_BACKGROUND,
                   Listi::PanelBackground(B_PANEL_BACKGROUND_COLOR, active));
    list->SetColor(B_COLOR_TEXT, ui_color(B_PANEL_TEXT_COLOR));
    list->SetColor(B_COLOR_SELECTION,
                   tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), 0.80));
    list->SetColor(B_COLOR_SELECTION_TEXT, ui_color(B_PANEL_TEXT_COLOR));
    list->SetColor(B_COLOR_NON_FOCUS_SELECTION,
                   tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), 0.80));
    list->UpdateScrollBars();
    Redraw();
  }
  void SetAttributes(const std::vector<std::string> &names) override {
    if (!attributes.empty()) {
      // Native RemoveColumn does not remove pointers from the sort list.
      list->ClearSortColumns();
      list->SetSortColumn(list->name, false, true);
    }
    while (!attributes.empty()) {
      auto column = attributes.back();
      list->RemoveColumn(column);
      delete column;
      attributes.pop_back();
    }
    for (auto &name : names) {
      auto column = new AttributeColumn(name.c_str());
      list->AddColumn(column, 3 + attributes.size());
      attributes.push_back(column);
    }
  }
  void Refresh() override {
    list->syncing = true;
    if (model->Visible().empty() && !rows.empty()) {
      // Remove one row through the normal API to release the native focus
      // and hover bookkeeping; Clear owns and destroys the remaining rows.
      auto *first = list->FocusRow();
      if (!first)
        first = list->RowAt(0);
      list->rowsHeight = 0;
      list->RemoveRow(first);
      delete first;
      list->Clear();
      rows.clear();
      list->syncing = false;
      list->UpdateScrollBars();
      return;
    }
    std::set<std::string> visible;
    for (auto entry : model->Visible())
      visible.insert(entry->path);
    for (auto it = rows.begin(); it != rows.end();) {
      if (!visible.count(it->first)) {
        list->rowsHeight -= it->second->Height() + 1;
        list->RemoveRow(it->second);
        delete it->second;
        it = rows.erase(it);
      } else
        ++it;
    }
    Append(0);
    SyncSelection();
  }
  void Append(size_t first) override {
    list->syncing = true;
    const auto &visible = model->Visible();
    BList added;
    std::vector<BRow *> selected;
    for (size_t i = first; i < visible.size(); ++i) {
      auto entry = visible[i];
      if (rows.count(entry->path))
        continue;
      auto row = new FileRow(entry, std::max(22.f, prefs.font.Size() + 9));
      row->SetField(new NameField(entry), 0);
      row->SetField(new SizeField(entry->directory || !entry->sizeKnown
                                      ? -1
                                      : entry->size),
                    1);
      time_t date = entry->modified;
      row->SetField(new DateField(&date), 2);
      for (size_t i = 0; i < attributes.size(); ++i)
        row->SetField(new AttributeField(i < entry->attributeCells.size()
                                            ? entry->attributeCells[i]
                                            : AttributeCellPreview{}),
                      3 + i);
      rows.emplace(entry->path, row);
      added.AddItem(row);
      list->rowsHeight += row->Height() + 1;
      if (model->IsSelected(entry->path))
        selected.push_back(row);
    }
    list->AddRows(&added, -1);
    for (auto *row : selected)
      list->AddToSelection(row);
    list->syncing = false;
    list->UpdateScrollBars();
  }
  void SyncSelection() override {
    if (list->syncing)
      return;
    list->syncing = true;
    auto focused = model->Focused();
    std::set<BRow *> selected;
    for (auto row = list->CurrentSelection(); row;
         row = list->CurrentSelection(row))
      selected.insert(row);
    for (auto &item : rows) {
      bool want = model->IsSelected(item.first);
      if (want && !selected.count(item.second))
        list->AddToSelection(item.second);
      else if (!want && selected.count(item.second))
        list->Deselect(item.second);
      if (focused && focused->path == item.first)
        list->SetFocusRow(item.second);
    }
    list->syncing = false;
    // Native row APIs already invalidate changed rows.
  }
};
} // namespace
FileDetails *CreateFileDetails(Pane *pane, PaneModel *model,
                               const Preferences &prefs) {
  return new DetailsView(pane, model, prefs);
}
