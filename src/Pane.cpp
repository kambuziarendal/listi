// SPDX-License-Identifier: MIT
#include "Pane.h"
#include "UnavailableVolume.h"
#include "HistoryPaneHint.h"
#include "SearchWorker.h"
#include "AdaptiveScrollView.h"
#include "FlatView.h"
#include "FolderHistory.h"
#include "PanelAppearance.h"
#include "DirectoryScanner.h"
#include "ReadWorker.h"
#include "PropertiesWindow.h"
#include "FileDetails.h"
#include "FileGrid.h"
#include "FileOperation.h"
#include "PaneSidebar.h"
#include "ImagePreview.h"
#include "MillerColumns.h"
#include "MainWindow.h"
#include "NameFilter.h"
#include "Trash.h"
#include "VirtualFile.h"
#include "ViewVisibility.h"
#include "WrappedLabel.h"
#include <Bitmap.h>
#include <CardLayout.h>
#include <CardView.h>
#include <Directory.h>
#include <Entry.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageFilter.h>
#include <MessageRunner.h>
#include <NodeMonitor.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <SplitView.h>
#include <StringForSize.h>
#include <TextView.h>
#include <Window.h>
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
constexpr uint32 kRefreshDirectory = 'rdir';
constexpr bigtime_t kRefreshInterval = 250000;

bool ColumnEntryUnchanged(const BMessage &action) {
  const char *path = action.GetString("clickedPath", nullptr);
  if (!path)
    return true;
  return Listi::SameSearchEntry(
      {path, action.GetInt64("device", -1), action.GetInt64("node", -1)});
}

class PaneInput : public BMessageFilter {
  Pane *pane;
  BTextControl *pathControl;

public:
  PaneInput(Pane *p, BTextControl *path = nullptr)
      : BMessageFilter(B_ANY_DELIVERY, B_ANY_SOURCE), pane(p),
        pathControl(path) {}
  filter_result Filter(BMessage *message, BHandler **) override {
    if (message->what == B_MOUSE_DOWN || message->what == B_KEY_DOWN)
      pane->Activate();
    if (message->what == B_KEY_DOWN) {
      const char *bytes = message->GetString("bytes", "");
      if (pathControl && bytes[0] == B_ENTER) {
        BString text(pathControl->Text());
        // BTextControl only invokes on Enter when its text changed. Reset
        // its focus baseline so the following focus loss cannot submit again,
        // even if navigation fails before updating the control.
        pathControl->SetText(text);
        BMessage navigate(kNavigate);
        navigate.AddString("path", text);
        pane->MessageReceived(&navigate);
        return B_SKIP_MESSAGE;
      }
      if (!pathControl && bytes[0] == B_ESCAPE) {
        BMessage clear(kClearFilter);
        pane->MessageReceived(&clear);
        return B_SKIP_MESSAGE;
      }
    }
    return B_DISPATCH_MESSAGE;
  }
};
constexpr uint32 kExpireProgress = 'expr', kActivityFrame = 'actf',
                 kCheckScanDelivery = 'scdl', kColumnNavigate = 'mnav';
constexpr int32 kRememberViewItem = FlatMode + 2;
constexpr int32 kForgetViewItem = FlatMode + 3;
}
Pane::Pane(int32 number, const Preferences &p,
           std::shared_ptr<Listi::Shelf> sharedShelf)
    : BView("pane", B_WILL_DRAW | B_SUPPORTS_LAYOUT | B_FULL_UPDATE_ON_RESIZE),
      id(number), prefs(p),
      generation(std::make_shared<std::atomic<int64>>(0)) {
  SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
  auto menu = new BPopUpMenu("Go");
  const char *names[] = {"Parent",  "Root",  "Home",
                         "Desktop", "Disks", "Trash (this volume)"};
  uint32 codes[] = {kParent, kRoot, kHome, kDesktop, kDisks, kTrashFolder};
  for (int i = 0; i < 6; i++)
    menu->AddItem(new BMenuItem(names[i], new BMessage(codes[i])));
  menu->AddSeparatorItem();
  edit = new BTextControl("path", "", path, new BMessage(kNavigate));
  back = new BButton("back", "<", new BMessage(kBack));
  forward = new BButton("forward", ">", new BMessage(kForward));
  for (auto button : {back, forward}) {
    button->SetExplicitMinSize(BSize(32, B_SIZE_UNSET));
    button->SetExplicitMaxSize(BSize(32, B_SIZE_UNSET));
  }
  back->SetToolTip("Back in this panel (Command+[ while browsing)");
  forward->SetToolTip("Forward in this panel (Command+] while browsing)");
  UpdateNavigation();
  edit->SetExplicitMinSize(BSize(120, B_SIZE_UNSET));
  filter = new BTextControl("filter", "Filter this folder:", "", new BMessage(kFilterDone));
  filter->SetModificationMessage(new BMessage(kFilterChanged));
  filter->SetExplicitMinSize(BSize(120, B_SIZE_UNSET));
  filter->SetToolTip("Show names containing this text. Enter returns to the "
                     "list; Escape clears.");
  clearFilter =
      new BButton("clear filter", "Clear", new BMessage(kClearFilter));
  clearFilter->SetEnabled(false);
  list = CreateFileDetails(this, &entries, p);
  list->SetExplicitMinSize(BSize(220, 96));
  grid = CreateFileGrid(this, &entries);
  auto gridScroll = new Listi::AdaptiveScrollView("icons scroll", grid, false, true,
      [this](float width) { return BSize(0, grid->ContentHeight(width - 1)); });
  columns = new MillerColumns(this, &entries);
  auto columnScroll = new Listi::AdaptiveScrollView("Miller horizontal scroll", columns, true, false,
      [this](float width) { return BSize(columns->ContentWidth(width), 0); }, B_NO_BORDER);
  cards = new BCardView("panel views");
  cards->AddChild(list);
  cards->AddChild(gridScroll);
  cards->AddChild(columnScroll);
  auto modes = new BPopUpMenu("View");
  for (int32 i = 0; i <= FlatMode; ++i) {
    auto message = new BMessage(kGridMode);
    message->AddInt32("mode", i);
    modes->AddItem(new BMenuItem(ViewModeLabel(i), message));
  }
  modes->AddSeparatorItem();
  modes->AddItem(new BMenuItem("Remember view for this folder",
                               new BMessage(kRememberView)));
  modes->AddItem(new BMenuItem("Use pane view for this folder",
                               new BMessage(kForgetView)));
  auto workspaceView = new BMessage(kGridMode);
  workspaceView->AddInt32("mode", -1);
  modes->AddItem(new BMenuItem("Use workspace default", workspaceView));
  modes->AddItem(new BMenuItem("Clear attribute columns",
                               new BMessage(kClearAttributeColumns)));
  modes->SetRadioMode(false);
  modes->SetLabelFromMarked(false);
  display = new BMenuField("display mode", "View:", modes);
  modes->AddSeparatorItem();
  sidebar = new PaneSidebar(*this, id, *menu, *modes, history,
                            std::move(sharedShelf));
  auto field = new BMenuField("go", "", menu);
  menu->SetRadioMode(false);
  menu->SetLabelFromMarked(false);
  content = new BSplitView(B_HORIZONTAL, 4);
  contentSplitterSize = content->SplitterSize();
  content->AddChild(sidebar);
  content->AddChild(cards);
  content->SetItemWeight(0, 0.24f, false);
  content->SetItemWeight(1, 0.76f, true);
  content->SetCollapsible(0, false);
  content->SetCollapsible(1, false);
  status = new WrappedLabel("status", "Ready");
  clearEmptyFilter = new BButton("clear unmatched filter", "Clear filter",
                                 new BMessage(kClearFilter));
  clearEmptyFilter->Hide();
  auto *historyAction = new BMessage(Listi::kFolderHistory);
  historyAction->AddInt32("pane", id);
  historyButton = new BButton("history indicator", "History…", historyAction);
  historyButton->SetFlat(true);
  historyButton->Hide();
  header = new BGroupView(B_HORIZONTAL, 5);
  header->SetName("panel toolbar");
  header->AddChild(field);
  header->AddChild(display);
  filterRow = new BGroupView(B_HORIZONTAL, 5);
  filterRow->SetName("filter toolbar");
  filterRow->AddChild(filter);
  filterRow->AddChild(clearFilter);
  BLayoutBuilder::Group<>(this, B_VERTICAL, 5)
      .SetInsets(6)
      .Add(header, 0)
      .AddGroup(B_HORIZONTAL, 3, 0)
      .Add(back)
      .Add(forward)
      .Add(edit)
      .End()
      .Add(filterRow, 0)
      .Add(content)
      .AddGroup(B_HORIZONTAL, 5, 0)
      .Add(status)
      .Add(historyButton)
      .Add(clearEmptyFilter)
      .End();
  SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
}
Pane::~Pane() {
  generation->store(-1);
  Listi::RetireReadWorker(queryWorker);
  if (directoryWatch)
    directoryWatch->Begin(-1, false, BMessenger(this));
  scanTimer.reset();
  stop_watching(this);
  Listi::RetireReadWorker(scanner);
  Listi::RetireReadWorker(iconReader);
}
bool Pane::HasHistoryHint() const { return !historyButton->IsHidden(historyButton); }
void Pane::SetHistoryHint(bool visible, bool enabled, int32 versions) {
  if (visible && (!HasHistoryHint() || historyEnabled != enabled || historyVersions != versions)) {
    historyEnabled = enabled;
    historyVersions = versions;
    std::string label = "History";
    if (versions > 0) label += " (" + std::to_string(versions) + ")";
    else if (versions == 0) label += " · No versions yet";
    else if (versions == Listi::kUnavailableVersions) label += " · Unavailable";
    label += "…";
    historyButton->SetLabel(label.c_str());
    historyButton->SetToolTip(enabled ? "Automatic history enabled · Browse versions"
                                     : "Automatic saving paused · Browse history");
  }
  Listi::SetViewVisible(historyButton, visible);
}
void Pane::AttachedToWindow() {
  display->Menu()->SetTargetForItems(this);
  edit->SetTarget(this);
  back->SetTarget(this);
  forward->SetTarget(this);
  filter->SetTarget(this);
  clearFilter->SetTarget(this);
  clearEmptyFilter->SetTarget(this);
  filter->TextView()->AddFilter(new PaneInput(this));
  edit->TextView()->AddFilter(new PaneInput(this, edit));
  auto field = dynamic_cast<BMenuField *>(FindView("go"));
  field->Menu()->SetTargetForItems(this);
  Apply(prefs);
  Navigate(path);
}
void Pane::StartDirectoryReader() {
  // About 2,560 ordinary rows: enough for fast grid scans to batch paints
  // without letting a large Details scan fill the UI queue for seconds.
  scanDelivery = std::make_shared<DirectoryScanDelivery>(32);
  if (!directoryWatch)
    directoryWatch = std::make_shared<DirectoryWatch>();
  scanner = CreateDirectoryScanner(BMessenger(this), generation, scanDelivery, directoryWatch);
  Listi::StartReadWorker(scanner);
}
void Pane::AfterInterruptedCopy() {
  auto &volumes = Listi::InterruptedVolumes();
  if (!volumes.Contains(SearchRoot()) && !volumes.ContainsDevice(folderIdentity.first)) {
    const auto &rows = entries.Visible();
    if (!mediaReadSuspended &&
        (path == "@disks" || std::any_of(rows.begin(), rows.end(), [&](const auto *row) {
          return volumes.Contains(row->path) || volumes.ContainsDevice(row->device);
        })))
      Reload();
    return;
  }
  if (mediaReadSuspended)
    return;
  history.Complete(serial, path.String(), false);
  generation->store(++serial);
  Listi::RetireReadWorker(scanner);
  Listi::RetireReadWorker(queryWorker);
  Listi::RetireReadWorker(iconReader);
  scanTimer.reset();
  directoryRefreshTimer.reset();
  scanning = refreshPending = directoryRefreshPending = false;
  if (directoryWatch)
    directoryWatch->Begin(-1, false, BMessenger(this));
  mediaReadSuspended = true;
  locationValid = false;
  folderIdentity = {-1, -1};
  ClearRows();
  UpdateNavigation();
  SetNavigationError(Listi::kUnavailableVolumeText);
  SendSelection();
}
void Pane::Activate() {
  if (Window()) {
    BMessage m(kActivate);
    m.AddInt32("pane", id);
    Window()->PostMessage(&m);
  }
}
void Pane::InvalidateActivityBorder() {
  const auto b = Bounds();
  // Only the empty outer inset is animated, never the file views or controls.
  Invalidate(BRect(b.left, b.top, b.right, b.top + 3));
  Invalidate(BRect(b.left, b.bottom - 3, b.right, b.bottom));
  Invalidate(BRect(b.left, b.top + 4, b.left + 3, b.bottom - 4));
  Invalidate(BRect(b.right - 3, b.top + 4, b.right, b.bottom - 4));
}
void Pane::Draw(BRect update) {
  // Resizing moves the frame through the exposed gaps between child controls.
  // Repaint those gaps instead of leaving previous frame edges behind.
  PushState();
  SetHighColor(ViewColor());
  FillRect(update);
  if (!prefs.SinglePane()) {
    const auto neutral = Listi::PanelBlend(ui_color(B_PANEL_BACKGROUND_COLOR),
                                           ui_color(B_PANEL_TEXT_COLOR), 0.12f);
    SetHighColor(Listi::PanelBlend(
        neutral, ui_color(B_KEYBOARD_NAVIGATION_COLOR), activityLevel));
    SetPenSize(2);
    StrokeRect(Bounds().InsetByCopy(1, 1));
  }
  PopState();
}
void Pane::UpdateActivityAppearance() {
  const bool highlighted = active && !prefs.SinglePane();
  list->SetActive(highlighted);
  grid->SetActive(highlighted);
  columns->SetActive(highlighted);
  auto text = edit->TextView();
  const auto background = Listi::PanelBlend(
      ui_color(B_DOCUMENT_BACKGROUND_COLOR),
      ui_color(B_KEYBOARD_NAVIGATION_COLOR), highlighted ? 0.075f : 0.f);
  if (text->ViewColor() != background) {
    text->SetViewColor(background);
    text->SetLowColor(background);
    text->Invalidate();
  }
  edit->SetToolTip(prefs.SinglePane() ? "Folder path"
                   : active           ? "Active file panel"
                                      : "Inactive file panel");
}
void Pane::SetSource(bool source) {
  if (active == source)
    return;
  active = source;
  UpdateActivityAppearance();
  activityTimer.reset();
  ++activitySerial;
  if (!prefs.SinglePane() && Window()) {
    activityFrom = activityLevel;
    activityStarted = system_time();
    activitySteps = 0;
    BMessage frame(kActivityFrame);
    frame.AddInt64("serial", activitySerial);
    activityTimer.reset(new BMessageRunner(BMessenger(this), &frame, 25000, 6));
    if (activityTimer->InitCheck() == B_OK)
      return;
    activityTimer.reset();
  }
  activityLevel = active ? 1.f : 0.f;
  InvalidateActivityBorder();
}
PaneSort Pane::Sorting() {
  return mode == 0 ? list->Sorting() : entries.Sorting();
}
void Pane::Sort(PaneSort value) {
  CancelSelectionGesture();
  entries.Sort(value);
  list->Sort(value);
  grid->Update();
  columns->Refresh();
  SendSelection();
}
int32 Pane::LocationMode() const {
  if (IsSearchList())
    return 0;
  if (attributeDetails && path[0] == '/' && !Listi::IsVirtual(path.String()))
    return 0;
  if (workingSetView >= 0)
    return workingSetView;
  auto remembered = prefs.folderModes.find(path.String());
  return remembered == prefs.folderModes.end() ? (restoredView >= 0 ? restoredView : prefs.PaneMode(id))
                                               : remembered->second;
}
void Pane::SetDisplayMode(int32 value, bool appendedDetails) {
  auto focus = Window() ? Window()->CurrentFocus() : nullptr;
  while (focus && focus != cards)
    focus = focus->Parent();
  if (mode == 0)
    entries.Sort(list->Sorting());
  mode = std::clamp(value, int32(0), ViewModeCount - 1);
  cards->CardLayout()->SetVisibleItem(mode == 0            ? 0
                                      : mode == MillerMode ? 2
                                                           : 1);
  columns->SetEnabled(mode == MillerMode);
  if (mode == 0) {
    if (!appendedDetails)
      list->Refresh();
  } else if (mode == MillerMode) {
    columns->SetLocation(path.String());
    columns->Refresh();
  } else {
    grid->SetMode(mode);
    grid->SyncSelection();
  }
  sidebar->UpdateMode(prefs);
  if (focus)
    FocusList();
  auto menu = display->Menu();
  for (int i = 0; i < ViewModeCount; ++i)
    menu->ItemAt(i)->SetMarked(mode == i);
  menu->ItemAt(FlatMode)->SetEnabled(path[0] == '/' && !Listi::IsVirtual(path.String()));
  menu->Superitem()->SetLabel(menu->ItemAt(mode)->Label());
  menu->ItemAt(kRememberViewItem)
      ->SetMarked(prefs.folderModes.count(path.String()));
  menu->ItemAt(kForgetViewItem)
      ->SetEnabled(prefs.folderModes.count(path.String()));
}
void Pane::PublishDisplay(bool folder, bool forget) {
  BMessage changed(kPaneDisplay);
  if (folder) {
    changed.AddString("folder", path);
    changed.AddInt32("mode", mode);
    changed.AddBool("forget", forget);
  }
  Window()->PostMessage(&changed);
}
void Pane::FocusList() {
  if (mode == 0)
    list->MakeFocus(true);
  else if (mode == MillerMode)
    columns->MakeFocus(true);
  else
    grid->MakeFocus(true);
}
void Pane::ToggleTreeFocus() { sidebar->ToggleFocus(); }
bool Pane::CanToggleTree() const {
  return !einn && (prefs.HasConfigurableSidebar() || mode != MillerMode);
}
void Pane::FocusFilter() {
  if (einn)
    return;
  Activate();
  filter->MakeFocus(true);
  filter->TextView()->SelectAll();
}
void Pane::ClearRows() {
  SetFilterEmptyState(false);
  ++selectionRevision;
  pendingIcons.clear();
  entries.Clear();
  icons.Clear();
  list->Refresh();
  grid->Reset();
  columns->Refresh();
  columns->SyncSelection();
}
void Pane::ApplyFilter() {
  ++selectionRevision;
  entries.Filter(einn ? "" : filter->Text());
  list->Refresh();
  grid->Update();
  columns->Refresh();
  clearFilter->SetEnabled(filter->Text()[0] != 0);
  if (!scanning)
    SendSelection();
}
void Pane::SetWorkspace(const Preferences &settings) {
  static_cast<WorkspaceLayout &>(prefs) = settings;
  const bool value = prefs.IsEinn();
  const bool changed = einn != value;
  einn = value;
  if (treeOnRight != prefs.treeRight) {
    treeOnRight = prefs.treeRight;
    content->GetLayout()->RemoveView(sidebar);
    content->AddChild(treeOnRight ? 1 : 0, sidebar, 0.24f);
    content->SetCollapsible(0, false);
    content->SetCollapsible(1, false);
  }
  if (filterAtBottom != prefs.filterBottom) {
    filterAtBottom = prefs.filterBottom;
    auto layout = static_cast<BGroupLayout *>(GetLayout());
    layout->RemoveView(filterRow);
    // Keep the filter directly adjacent to the file list on either side.
    layout->AddView(filterAtBottom ? 3 : 2, filterRow, 0.0f);
  }
  if (chromeVisible == einn) {
    chromeVisible = !einn;
    for (BView *control :
         {static_cast<BView *>(header), static_cast<BView *>(filterRow),
          static_cast<BView *>(back), static_cast<BView *>(forward)}) {
      if (chromeVisible)
        control->Show();
      else
        control->Hide();
    }
  }
  auto layout = static_cast<BGroupLayout *>(GetLayout());
  layout->SetInsets(einn ? 2 : 6);
  layout->SetSpacing(einn ? 2 : 5);
  content->SetSpacing(einn ? 0 : 4);
  content->SetSplitterSize(einn ? 0 : contentSplitterSize);
  // A hidden filter must not silently hide entries in the minimal workspace.
  // Keep its text for Tviskipt, but suspend its effect while it is hidden.
  if (changed)
    ApplyFilter();
  SetDisplayMode(LocationMode());
  activityTimer.reset();
  ++activitySerial;
  activityLevel = active ? 1.f : 0.f;
  UpdateActivityAppearance();
  InvalidateActivityBorder();
}
bool Pane::SidebarCanBeRestored() const {
  return prefs.HasConfigurableSidebar() && Sidebar() == SidebarMode::Hidden;
}
void Pane::RefreshShelf() { sidebar->RefreshShelf(); }
void Pane::Apply(const Preferences &p) {
  prefs = p;
  SetWorkspace(p);
  sidebar->ShowTreePath();
  list->Apply(p);
  grid->SetFont(&p.font);
  grid->Update();
  columns->ApplyFont(p.font);
  UpdateActivityAppearance();
}
void Pane::Navigate(const char *p) {
  if (!Window())
    return;
  std::string request = p;
  if (IsSearchList() && request == LocationLabel())
    request = path.String();
  if (request.empty())
    return;
  try {
    if (!queries.count(request) && IsSearchList() && request[0] != '/' && request != "@disks" && !Listi::IsVirtual(request)) {
      SetStatus("Enter an absolute folder path.");
      return;
    }
    if (!queries.count(request) && request[0] != '/' && request != "@disks" && !Listi::IsVirtual(request))
      request = Listi::IsVirtual(path.String())
                    ? Listi::ChildLocation(path.String(), request)
                    : Listi::Join(path.String(), request);
    if (Listi::IsVirtual(request))
      request = Listi::ParseLocation(request).URL();
  } catch (const std::exception &error) {
    SetNavigationError(error.what());
    return;
  }
  StartNavigation(request);
}
void Pane::NavigateFromColumn(const std::string &folder,
                              const std::string &selection, bool invoke,
                              bool context, BPoint at, const PaneEntry *clicked) {
  BMessage message(kColumnNavigate);
  if (clicked && !Listi::IsVirtual(clicked->path)) {
    message.AddString("clickedPath", clicked->path.c_str());
    message.AddInt64("device", clicked->device);
    message.AddInt64("node", clicked->node);
  }
  message.AddString("path", folder.c_str());
  message.AddString("selection", selection.c_str());
  message.AddBool("invoke", invoke);
  message.AddBool("context", context);
  message.AddPoint("screen", at);
  message.AddInt64("serial", serial);
  Window()->PostMessage(&message, this);
}
void Pane::SetFilterEmptyState(bool noMatches) {
  Listi::SetViewVisible(clearEmptyFilter, noMatches);
  Listi::SetViewVisible(clearFilter, !noMatches);
}
void Pane::SetStatus(const char *text) {
  if (!navigationFailed)
    status->SetMessage(text);
}
void Pane::SetNavigationError(const char *text) {
  navigationFailed = true;
  status->SetMessage(text);
}
void Pane::SetPlaces(const Listi::SavedPlaces &value, bool writable) {
  sidebar->SetPlaces(value, writable);
}
void Pane::UpdateNavigation() {
  back->SetEnabled(history.Back() >= 0);
  forward->SetEnabled(history.Forward() >= 0);
}
void Pane::StartNavigation(const std::string &request, int target, bool trash,
                           bool restore, bool refresh) {
  try {
    const auto query = queries.find(request);
    if (Listi::InterruptedVolumes().Contains(
            query == queries.end() ? request : query->second.request.root) ||
        (mediaReadSuspended && path == request.c_str()))
      throw std::runtime_error(Listi::kUnavailableVolumeText);
    if (Listi::IsVirtual(request))
      Listi::RequireConnection(request);
  } catch (const std::exception &error) {
    restoring.reset();
    SetNavigationError(error.what());
    return;
  }
  if (!restore && locationValid && !scanning)
    history.SavePosition(CaptureVisit());
  pendingVisit = restore ? restoring->pendingVisit
                         : target >= 0 ? history.PositionAt(target) : std::nullopt;
  if (refresh && locationValid)
    pendingVisit = CaptureVisit(entries.Visible().size());
  if (!restore) {
    restoring.reset();
    if (path != request.c_str())
      workingSetView = -1;
  }
  const bool wasQuery = IsSearchList();
  Listi::RetireReadWorker(queryWorker);
  queryRows.clear();
  const bool sameDirectory = !trash && path == request.c_str();
  directoryRefreshPending = false;
  mediaReadSuspended = false;
  reveal = {};
  columnAction.MakeEmpty();
  savedSelection.clear();
  if (refresh && pendingVisit) {
    savedSelection = pendingVisit->selected;
  } else if (target < 0 && !trash && path == request.c_str()) {
    for (auto entry : entries.Visible())
      if (entries.IsSelected(entry->path))
        savedSelection.emplace(entry->path,
                               std::make_pair(entry->device, entry->node));
  }
  if (trash || path != request.c_str()) {
    filter->SetText("");
    clearFilter->SetEnabled(false);
  }
  path = request.c_str();
  if (restoring) {
    filter->SetText(restoring->filter.c_str());
    savedSelection = restoring->selection;
  }
  edit->SetText(LocationLabel().c_str());
  if (!pendingIcons.empty())
    Listi::RetireReadWorker(iconReader);
  if (scanning)
    Listi::RetireReadWorker(scanner);
  if (!scanner && !IsSearchList())
    StartDirectoryReader();
  receivedRows = {};
  scanning = true;
  locationValid = false;
  folderIdentity = {-1, -1};
  sidebar->UpdateLocation();
  refreshPending = false;
  serial++;
  history.Begin(serial, request, target, trash);
  UpdateNavigation();
  generation->store(serial);
  directoryWatch->Begin(serial, sameDirectory, BMessenger(this));
  ClearRows();
  if (IsSearchList() != wasQuery || restore)
    list->SetAttributes(IsSearchList() ? std::vector<std::string>{"Original folder"} : attributeColumns);
  SetDisplayMode(LocationMode());
  navigationFailed = false;
  SetStatus("Reading directory...");
  BMessage m(kScan);
  m.AddString("path", path);
  m.AddInt64("serial", serial);
  m.AddBool("icons", false);
  m.AddBool("firstScreen", true);
  m.AddInt32("iconSize", mode == 0 || mode == MillerMode || mode == MiniIconMode ? 16 : 96);
  m.AddBool("trash", trash);
  if (mode == 0)
    for (auto &name : attributeColumns)
      m.AddString("attribute", name.c_str());
  BMessage check(kCheckScanDelivery);
  check.AddInt64("serial", serial);
  scanTimer.reset(new BMessageRunner(BMessenger(this), &check, 250000));
  bool started = false;
  if (scanTimer->InitCheck() == B_OK) {
    if (IsSearchList()) {
      SetStatus("Reading search results...");
      queryWorker = Listi::NewSearchWorker(BMessenger(this), generation, queryFailure);
      BMessage search(Listi::kSearchStart);
      queries.at(request).WriteTo(search);
      search.AddInt64("serial", serial);
      search.AddBool("paneRows", true);
      started = Listi::StartReadWorker(queryWorker) && queryWorker->PostMessage(&search) == B_OK;
    } else {
      started = scanner && scanner->PostMessage(&m) == B_OK;
    }
  }
  if (!started) {
    BMessage failed(kScanDone);
    failed.AddInt64("serial", serial);
    failed.AddString("error",
                     "Cannot start directory reader. Reload to try again.");
    MessageReceived(&failed);
  }
}
void Pane::ScheduleDirectoryRefresh() {
  if (mediaReadSuspended)
    return;
  directoryRefreshPending = true;
  // Arm once per burst rather than postponing on every event: a long
  // transfer should still show newly published files as it runs.
  if (!directoryRefreshTimer) {
    BMessage refresh(kRefreshDirectory);
    directoryRefreshTimer.reset(new BMessageRunner(BMessenger(this),
                                                    &refresh, kRefreshInterval, 1));
    if (directoryRefreshTimer->InitCheck() != B_OK) {
      directoryRefreshTimer.reset();
      directoryRefreshPending = false;
      Reload();
    }
  }
}
void Pane::RequestVisibleIcon(PaneEntry &row) {
  if (row.unmountedDisk || row.iconRequested || pendingIcons.size() >= 64 ||
      Listi::InterruptedVolumes().ContainsDevice(row.device) ||
      Listi::InterruptedVolumes().Contains(row.path) ||
      ((mode == 0 || mode == MillerMode) && !prefs.icons))
    return;
  if (!iconReader) {
    iconReader = CreateDirectoryScanner(BMessenger(this), generation);
    if (!Listi::StartReadWorker(iconReader))
      return;
  }
  BMessage request(kReadVisibleIcon);
  request.AddInt64("serial", serial);
  request.AddString("path", row.path.c_str());
  request.AddString("name", row.name.c_str());
  request.AddBool("directory", row.directory);
  request.AddInt64("device", row.device);
  request.AddInt64("node", row.node);
  request.AddInt32("iconSize", mode == 0 || mode == MillerMode || mode == MiniIconMode ? 16 : 96);
  if (iconReader->PostMessage(&request) == B_OK) {
    row.iconRequested = true;
    pendingIcons[row.path] = &row;
  }
}
void Pane::Reload() {
  if (!Window())
    return;
  if (scanning) {
    refreshPending = true;
    return;
  }
  StartNavigation(path.String(), -1, false, false, true);
}
std::vector<std::string> Pane::Selected() const {
  return entries.SelectedFiles();
}

const PaneEntry *Pane::SelectedDisk() const {
  return path == "@disks" && !scanning ? entries.OnlySelected() : nullptr;
}

void Pane::ChangeDiskMount(const PaneEntry &disk, bool mount) {
  if (!disk.partition || mount != disk.unmountedDisk)
    return;
  const BMessenger target(this);
  try {
    std::thread([target, partition = *disk.partition, mount, path = disk.path,
                 device = disk.device, node = disk.node] {
      const auto result = mount ? Listi::MountDisk(partition)
                                : Listi::UnmountDisk(partition, path, device, node);
      BMessage done(Listi::kDiskActionDone);
      done.AddBool("mount", mount);
      done.AddInt32("status", result);
      target.SendMessage(&done);
    }).detach();
    SetStatus(mount ? "Mounting disk..." : "Unmounting disk...");
  } catch (const std::exception &error) {
    PostMainWindowNotice(error.what());
  }
}
void Pane::ContextMenu(const std::string &path, BPoint screen) {
  if (scanning)
    return;
  if (path.empty()) {
    BMessage request(kContextMenu);
    request.AddInt32("pane", id);
    request.AddPoint("screen", screen);
    request.AddBool("folderContext", true);
    if (auto *entry = entries.OnlySelected())
      request.AddBool("directory", entry->directory);
    Window()->MessageReceived(&request);
    return;
  }
  for (auto entry : entries.Visible()) {
    if (entry->path != path || entry->parent)
      continue;
    auto selected = entries.IsSelected(path) ? entries.Selected()
                                             : std::vector<std::string>{path};
    entries.SetSelection(selected, path);
    SendSelection();
    BMessage request(kContextMenu);
    request.AddInt32("pane", id);
    request.AddPoint("screen", screen);
    request.AddBool("directory", entry->directory);
    // Same window thread: no queued navigation can change this click's owner.
    Window()->MessageReceived(&request);
    return;
  }
}
bool Pane::CancelSelectionGesture() {
  bool a = list->CancelSelectionGesture();
  bool b = grid->CancelSelectionGesture();
  return a || b;
}
void Pane::SendSelection(bool gesture) {
  BString s;
  const auto summary = entries.Summary();
  s << uint64(summary.selected) << " selected";
  if (summary.selected) {
    s << ": ";
    bool part = false;
    if (summary.files) {
      if (summary.files == summary.unknown)
        s << "size unknown";
      else {
        char text[64];
        if (summary.overflow)
          s << "at least ";
        s << string_for_size(summary.bytes, text, sizeof(text));
        if (summary.unknown)
          s << " + unknown size";
      }
      part = true;
    }
    if (summary.folders) {
      if (part)
        s << " + ";
      s << uint64(summary.folders)
        << (summary.folders == 1 ? " folder" : " folders");
      part = true;
    }
    if (summary.links) {
      if (part)
        s << " + ";
      s << uint64(summary.links) << (summary.links == 1 ? " link" : " links");
    }
  }
  s << " / " << uint64(summary.visible);
  if (!einn && filter->Text()[0])
    s << " of " << uint64(summary.visible + entries.HiddenCount())
      << " entries (filtered)";
  else
    s << " entries";
  if (!scanning && locationValid) {
    const bool noMatches = !summary.visible && !einn && filter->Text()[0] &&
                           entries.HiddenCount() != 0;
    SetFilterEmptyState(noMatches);
    if (noMatches)
      s = "No names match the filter";
    else if (!summary.visible)
      s = IsSearchList() ? "No search results" : "This folder is empty";
    if (IsSearchList()) s << " · " << LocationLabel().c_str();
  }
  SetStatus(s);
  if (!navigationFailed && summary.selected) {
    BString tip(s);
    if (summary.files && !summary.overflow)
      tip << "\nKnown file sizes: " << uint64(summary.bytes) << " bytes.";
    if (summary.folders || summary.links)
      tip << "\nFolder contents and link targets are not counted.";
    if (summary.unknown)
      tip << "\nSize unavailable for " << uint64(summary.unknown)
          << " selected file(s).";
    status->SetToolTip(tip);
  }
  if (mode == 0)
    list->SyncSelection();
  else if (mode == MillerMode)
    columns->SyncSelection();
  else
    grid->SyncSelection();
  // Preview decoding is useful after the gesture, not for transient rows.
  if (Window() && !gesture) {
    BMessage changed(kPreviewSelection);
    changed.AddInt32("pane", id);
    if (auto *entry = entries.OnlySelected()) {
      auto found = transfers.find(entry->path);
      if (found != transfers.end() && !found->second.error.empty()) {
        changed.AddString("operationError", found->second.error.c_str());
        changed.AddInt64("operation", found->second.operation);
        changed.AddString("path", entry->path.c_str());
      }
    }
    Window()->PostMessage(&changed);
  }
}
void Pane::MessageReceived(BMessage *m) {
  if (HandleQueryReply(*m))
    return;
  switch (m->what) {
  case Listi::kOpenSearchList:
    try {
      OpenSearchList(Listi::SavedSearch::FromMessage(*m));
    } catch (const std::exception &error) {
      SetStatus(error.what());
    }
    break;
  case kActivityFrame: {
    if (m->GetInt64("serial", -1) != activitySerial || !activityTimer)
      break;
    const bool finished =
        ++activitySteps >= 6 || system_time() - activityStarted >= 150000;
    const float fraction =
        finished
            ? 1.f
            : std::min(1.f, float(system_time() - activityStarted) / 150000.f);
    activityLevel =
        activityFrom + ((active ? 1.f : 0.f) - activityFrom) * fraction;
    InvalidateActivityBorder();
    if (finished)
      activityTimer.reset();
    break;
  }
  case kExpireProgress: {
    std::vector<std::string> expired;
    bool pending = false;
    for (auto &item : transfers)
      if (item.second.state == 2) {
        if (item.second.expires <= system_time())
          expired.push_back(item.first);
        else
          pending = true;
      }
    for (auto &file : expired) {
      BMessage clear(kFileProgress);
      clear.AddString("path", file.c_str());
      clear.AddInt32("state", 0);
      MessageReceived(&clear);
    }
    if (!pending)
      progressTimer.reset();
    break;
  }
  case kRefreshDirectory:
    directoryRefreshTimer.reset();
    if (directoryRefreshPending) {
      directoryRefreshPending = false;
      Reload();
    }
    break;
  case kFileProgress: {
    if (const char *destination = m->GetString("destination", nullptr)) {
      if (Listi::ParentLocation(destination) == path.String())
        ScheduleDirectoryRefresh();
    }
    std::string file = m->GetString("path", "");
    auto &transfer = transfers[file];
    transfer.state = m->GetInt32("state", 0);
    transfer.operation = m->GetInt64("operation", 0);
    transfer.fraction = m->GetFloat("fraction", 0);
    transfer.error = m->GetString("error", "");
    if (entries.Transfer(file, transfer.state, transfer.fraction)) {
      list->RedrawEntry(file);
      grid->RedrawEntry(file);
      columns->RedrawRows();
    }
    if (transfer.state == 2) {
      transfer.expires = system_time() + 2500000;
      if (!progressTimer) {
        BMessage expire(kExpireProgress);
        progressTimer.reset(
            new BMessageRunner(BMessenger(this), &expire, 250000));
      }
    }
    if (!transfer.state)
      transfers.erase(file);
    if (transfers.size() > 4096)
      transfers.erase(transfers.begin());
    break;
  }
  case kVolumeUnavailable:
    AfterInterruptedCopy();
    break;
  case kOperationErrorDismissed: {
    const auto operation = m->GetInt64("operation", 0);
    if (!operation)
      break;
    const char *path = m->GetString("path", "");
    std::vector<std::string> acknowledged;
    for (const auto &[file, transfer] : transfers)
      if (transfer.operation == operation && (!*path || file == path) &&
          (transfer.state == 1 || transfer.state == 3))
        acknowledged.push_back(file);
    for (const auto &file : acknowledged) {
      BMessage clear(kFileProgress);
      clear.AddString("path", file.c_str());
      MessageReceived(&clear);
    }
    break;
  }
  case kJobDone:
    if (m->GetBool("beforeStart", false))
      break;
    directoryRefreshPending = false;
    if (m->GetBool("errorDismissed", false)) {
      BMessage dismissed(kOperationErrorDismissed);
      dismissed.AddInt64("operation", m->GetInt64("operation", 0));
      MessageReceived(&dismissed);
    } else {
      for (auto &[file, transfer] : transfers)
        if (transfer.state == 1 && transfer.operation == m->GetInt64("operation", 0)) {
          transfer.state = 3;
          transfer.error = "Transfer did not complete. See Operations for details.";
        }
    }
    if (m->HasString("interruptedCopyFolder"))
      AfterInterruptedCopy();
    else if (!mediaReadSuspended)
      Reload();
    break;
  case kToggleTree: {
    if (!CanToggleTree())
      break;
    sidebar->ResetTree();
    auto choice = SidebarMode::FolderTree;
    switch (Sidebar()) {
    case SidebarMode::FolderTree:
      choice = SidebarMode::Hidden;
      break;
    case SidebarMode::ShortcutsAndTree:
      choice = SidebarMode::Favorites;
      break;
    case SidebarMode::Favorites:
      choice = SidebarMode::ShortcutsAndTree;
      break;
    case SidebarMode::Hidden:
      if (prefs.HasConfigurableSidebar() && SidebarHasTree(prefs.RestoreSidebar(id)))
        choice = prefs.RestoreSidebar(id);
      break;
    default:
      break;
    }
    prefs.SetSidebar(id, choice);
    sidebar->UpdateMode(prefs);
    PublishDisplay();
    break;
  }
  case kRestoreSidebar:
  case kSidebarMode: {
    const int32 choice = m->what == kRestoreSidebar
                             ? static_cast<int32>(prefs.RestoreSidebar(id))
                             : m->GetInt32("mode", -1);
    if (!prefs.HasConfigurableSidebar() ||
        (choice != static_cast<int32>(SidebarMode::Hidden) && !IsVisibleSidebar(choice)))
      break;
    prefs.SetSidebar(id, static_cast<SidebarMode>(choice));
    sidebar->UpdateMode(prefs);
    PublishDisplay();
    break;
  }
  case kShowAttributeColumn:
  case kClearAttributeColumns: {
    std::string name = m->GetString("attribute", "");
    BMessage reply(B_REPLY);
    if (m->what == kClearAttributeColumns) {
      if (attributeColumns.empty())
        break;
      attributeColumns.clear();
      attributeDetails = false;
    } else if (path[0] != '/' || Listi::IsVirtual(path.String())) {
      reply.AddString("status", "Attribute columns require a local folder.");
      m->SendReply(&reply);
      break;
    } else if (name.empty() || name.size() >= B_ATTR_NAME_LENGTH ||
               (attributeColumns.size() >= 4 &&
                std::find(attributeColumns.begin(), attributeColumns.end(),
                          name) == attributeColumns.end())) {
      reply.AddString(
          "status",
          "Use up to four columns; clear them in the pane view menu.");
      m->SendReply(&reply);
      break;
    } else {
      if (std::find(attributeColumns.begin(), attributeColumns.end(), name) ==
          attributeColumns.end())
        attributeColumns.push_back(name);
      attributeDetails = true;
    }
    if (!IsSearchList()) {
      list->SetAttributes(attributeColumns);
      StartNavigation(path.String());
    }
    if (m->IsSourceWaiting())
      m->SendReply(&reply);
    break;
  }
  case kGridMode: {
    auto requestedMode = m->GetInt32("mode", 0);
    if (requestedMode == FlatMode) {
      Activate();
      Window()->PostMessage(Listi::kFlatView);
      break;
    }
    if (IsSearchList()) {
      SetStatus("Search results use Details to show original folders.");
      break;
    }
    workingSetView = -1;
    restoredView = -1;
    attributeDetails = false;
    prefs.SetPaneMode(id, requestedMode);
    const bool remembered = prefs.folderModes.count(path.String());
    if (remembered) {
      if (requestedMode < 0)
        prefs.folderModes.erase(path.String());
      else
        prefs.folderModes[path.String()] = prefs.PaneMode(id);
    }
    SetDisplayMode(LocationMode());
    PublishDisplay(remembered, requestedMode < 0);
    Reload();
    FocusList();
    break;
  }
  case kRememberView:
    if (IsSearchList()) break;
    if (prefs.folderModes.size() >= 4096 &&
        !prefs.folderModes.count(path.String())) {
      SetStatus("Folder view limit reached");
      break;
    }
    prefs.folderModes[path.String()] = mode;
    SetDisplayMode(mode);
    PublishDisplay(true);
    break;
  case kForgetView:
    workingSetView = -1;
    attributeDetails = false;
    prefs.folderModes.erase(path.String());
    SetDisplayMode(LocationMode());
    PublishDisplay(true, true);
    Reload();
    break;
  case kFilterChanged:
    ApplyFilter();
    break;
  case kClearFilter:
    filter->SetText("");
    ApplyFilter();
    FocusList();
    break;
  case kFilterDone:
    FocusList();
    break;
  case kSwitch:
    Window()->PostMessage(kSwitch);
    break;
  case kActivate:
    Activate();
    FocusList();
    break;
  case kRevealSearch: {
    Listi::SearchHit hit{m->GetString("path", ""), m->GetInt64("device", -1),
                         m->GetInt64("node", -1)};
    if (!Listi::SameSearchEntry(hit)) {
      savedSelection.clear();
      reveal = {};
      entries.SetSelection({}, "");
      SendSelection();
      SetStatus(
          "Search result moved, disappeared or was replaced. Search again; nothing selected.");
      break;
    }
    size_t slash = hit.path.find_last_of('/');
    if (slash == std::string::npos)
      break;
    filter->SetText("");
    clearFilter->SetEnabled(false);
    StartNavigation(hit.path.substr(0, slash ? slash : 1));
    // Do not select the path until the scan is complete and identity rechecked.
    savedSelection.clear();
    reveal = hit;
    break;
  }
  case kNavigate:
    if (m->HasInt64("serial") && m->GetInt64("serial", -1) != serial)
      break;
    if (m->GetBool("place", false))
      Activate();
    Navigate(m->GetString("path", edit->Text()));
    if (m->GetBool("tree", false))
      Activate();
    else
      FocusList();
    break;
  case kColumnNavigate: {
    if (mode != MillerMode || m->GetInt64("serial", -1) != serial)
      break;
    if (Listi::InterruptedVolumes().Contains(m->GetString("clickedPath", "")) ||
        Listi::InterruptedVolumes().Contains(m->GetString("path", ""))) {
      SetNavigationError(Listi::kUnavailableVolumeText);
      break;
    }
    if (!ColumnEntryUnchanged(*m)) {
      SetStatus("Column entry changed. Select it again.");
      break;
    }
    const char *destination = m->GetString("path", path.String());
    if (path == destination && (locationValid || scanning) &&
        !m->GetString("selection", "")[0] && !m->GetBool("context", false) &&
        !m->GetBool("invoke", false)) {
      FocusList();
      break;
    }
    auto before = serial;
    Navigate(destination);
    if (serial != before && scanning)
      columnAction = *m;
    FocusList();
    break;
  }
  case kBack:
  case kForward: {
    int target = m->what == kBack ? history.Back() : history.Forward();
    if (target >= 0) {
      std::string destination = history.At(target);
      StartNavigation(destination, target);
      Activate();
      FocusList();
    }
    break;
  }
  case kParent: {
    if (IsSearchList()) {
      Navigate(queries.at(path.String()).request.root.c_str());
      break;
    }
    try {
      const std::string child = path.String();
      auto parent = Listi::ParentLocation(child == "@disks" ? "/" : child);
      Listi::VisitPosition up;
      auto entry = child;
      const auto location = Listi::ParseLocation(child);
      if (location.Archive() && location.path == "/")
        entry = location.archive;
      if (locationValid && (Listi::IsVirtual(entry) || folderIdentity.second >= 0)) {
        up.focusId = folderIdentity;
        up.focus = up.anchor = entry;
        up.anchorId = up.focusId;
        up.selected.emplace(entry, up.focusId);
      }
      Navigate(parent.c_str());
      if (scanning && parent != child)
        pendingVisit = std::move(up);
    } catch (const std::exception &e) {
      SetStatus(e.what());
    }
    break;
  }
  case kRoot:
    Navigate("/");
    break;
  case kHome:
    Navigate("/boot/home");
    break;
  case kDesktop:
    Navigate("/boot/home/Desktop");
    break;
  case kTrashFolder: {
    if (IsSearchList() || path == "@disks" || Listi::IsVirtual(path.String())) {
      SetStatus("Trash requires a local supported volume.");
      break;
    }
    StartNavigation(path.String(), -1, true);
    break;
  }
  case kDisks:
    Navigate("@disks");
    break;
  case kSelection:
    if (!scanning)
      SendSelection();
    break;
  case kInvoke: {
    auto r = entries.Focused();
    if (r && (Listi::InterruptedVolumes().ContainsDevice(r->device) ||
              Listi::InterruptedVolumes().Contains(r->path))) {
      SetNavigationError(Listi::kUnavailableVolumeText);
      break;
    }
    if (m->HasString("column") &&
        (scanning || path != m->GetString("column", "") || !r ||
         r->path != m->GetString("selection", "") || !ColumnEntryUnchanged(*m)))
      break;
    if (r) {
      if (r->parent) {
        BMessage parent(kParent);
        MessageReceived(&parent);
      } else if (r->unmountedDisk)
        SetStatus("This disk is not mounted. Right-click and choose Mount.");
      else if (r->directory)
        Navigate(r->path.c_str());
      else {
        BMessage open(kOpenPaths);
        open.AddString("path", r->path.c_str());
        open.AddMessenger("pane", BMessenger(this));
        open.AddInt64("serial", serial);
        Window()->PostMessage(&open);
      }
    }
    break;
  }
  case kVisibleIconReady: {
    if (m->GetInt64("serial", -1) != serial)
      break;
    auto item = pendingIcons.find(m->GetString("path", ""));
    if (item == pendingIcons.end())
      break;
    const void *bits;
    ssize_t length;
    if (m->FindData("icon", B_RAW_TYPE, &bits, &length) == B_OK)
      item->second->icon = icons.Get(bits, length, m->GetInt32("iconSize", 0));
    pendingIcons.erase(item);
    if (mode == 0)
      list->Redraw();
    else if (mode == MillerMode)
      columns->RedrawRows();
    else
      grid->Invalidate();
    break;
  }
  case kCheckScanDelivery: {
    if (IsSearchList() && m->GetInt64("serial", -1) == serial &&
        queryFailure->load() == serial) {
      queryFailure->store(-1);
      BMessage failure(Listi::kSearchDone);
      failure.AddString("error", "Search update was not delivered. Reload to try again.");
      failure.AddInt64("serial", serial);
      HandleQueryReply(failure);
    } else if (scanning && m->GetInt64("serial", -1) == serial &&
        scanDelivery->failedSerial.load() == serial) {
      BMessage failed(kScanDone);
      failed.AddInt64("serial", serial);
      failed.AddString(
          "error",
          "Directory completion was not delivered. Reload to try again.");
      MessageReceived(&failed);
    }
    break;
  }
  case kRows: {
    if (!scanning || m->GetInt64("serial", -1) != serial)
      break;
    scanDelivery->ReleaseBatch(serial);
    const size_t first = entries.Visible().size();
    receivedRows.Read(*m, "entry", [&](const BMessage &e) {
      auto row = std::make_unique<PaneEntry>();
      row->name = e.GetString("name", "");
      row->path = e.GetString("path", "");
      row->directory = e.GetBool("directory", false);
      row->parent = e.GetBool("parent", false);
      const char *cell;
      for (int32 i = 0; e.FindString("cell", i, &cell) == B_OK; ++i) {
        uint32 type = 0;
        e.FindUInt32("cellType", i, &type);
        row->attributeCells.push_back({cell, type});
      }
      row->kind = e.GetInt32("kind", 0);
      row->device = e.GetInt64("device", -1);
      if (Listi::InterruptedVolumes().ContainsDevice(row->device) ||
          Listi::InterruptedVolumes().Contains(row->path))
        return;
      row->node = e.GetInt64("node", -1);
      if (path == "@disks") {
        row->unmountedDisk = e.GetBool("unmountedDisk", false);
        if (row->unmountedDisk || e.HasInt32("partition")) {
          auto partition = Listi::DiskPartition::ReadFrom(e);
          if (partition.id < 0) {
            receivedRows.failed = true;
            return;
          }
          row->partition = std::make_unique<Listi::DiskPartition>(std::move(partition));
        }
      }
      // Virtual listings have no local node identity; retain their path-based
      // behavior. A replacement local entry must not inherit selection.
      auto saved = savedSelection.find(row->path);
      bool restore = saved != savedSelection.end() &&
                     saved->second == std::make_pair(row->device, row->node);
      row->size = e.GetInt64("size", 0);
      row->sizeKnown = e.GetBool("sizeKnown", true);
      row->modified = e.GetInt64("date", 0);
      row->modifiedKnown = e.GetBool("modifiedKnown", false);
      row->modifiedFraction = e.GetString("modifiedFraction", "");
      row->providerIdentity = e.GetString("providerIdentity", "");
      auto transfer = transfers.find(row->path);
      if (transfer != transfers.end()) {
        row->transferState = transfer->second.state;
        row->fraction = transfer->second.fraction;
      }
      const void *bits;
      ssize_t length;
      int32 pixels = e.GetInt32("iconSize", 16);
      if (pixels >= 16 && pixels <= 128 &&
          e.FindData("icon", B_RAW_TYPE, &bits, &length) == B_OK &&
          length == pixels * pixels * 4) {
        row->icon = icons.Get(bits, length, pixels);
      }
      entries.AddScanned(std::move(row), einn ? "" : filter->Text(), restore);
    });
    if (mode == 0)
      list->Append(first);
    else if (mode == MillerMode)
      columns->Refresh();
    else
      grid->Update();
    break;
  }
  case kScanDone: {
    if (!scanning || m->GetInt64("serial", -1) != serial)
      break;
    scanning = false;
    savedSelection.clear();
    scanTimer.reset();
    path = m->GetString("path", path);
    const char *error = m->GetString("error", nullptr);
    if (!error && !receivedRows.Complete(*m))
      error = "Incomplete directory listing. Reload to try again.";
    locationValid = error == nullptr;
    folderIdentity = {m->GetInt64("folderDevice", -1), m->GetInt64("folderNode", -1)};
    if (!locationValid) pendingVisit.reset();
    if (locationValid && mode != 0)
      entries.Sort(entries.Sorting());
    history.Complete(serial, path.String(), locationValid);
    UpdateNavigation();
    const int32 finalMode = LocationMode();
    // Native details rows were already appended by the scan. Keep the
    // final location, menu and model updates without revisiting every row.
    SetDisplayMode(finalMode, mode == 0 && finalMode == mode && locationValid);
    edit->SetText(LocationLabel().c_str());
    sidebar->UpdateLocation();
    if (error) {
      ClearRows();
      BString s("Cannot open: ");
      s << error;
      SetNavigationError(s);
    } else {
      navigationFailed = false;
      bool stale = false;
      BMessage action(columnAction);
      columnAction.MakeEmpty();
      std::string picked = action.GetString("selection", "");
      if (!picked.empty())
        entries.SetSelection({picked}, picked);
      bool changedColumn = !ColumnEntryUnchanged(action);
      if (!picked.empty() && action.HasString("clickedPath")) {
        const auto *row = entries.OnlySelected();
        changedColumn |= !row || row->device != action.GetInt64("device", -1) ||
                         row->node != action.GetInt64("node", -1);
      }
      if (changedColumn)
        entries.SetSelection({}, "");
      if (!reveal.path.empty()) {
        const auto &visible = entries.Visible();
        const auto row =
            std::find_if(visible.begin(), visible.end(), [&](PaneEntry *entry) {
              return !entry->parent && entry->path == reveal.path &&
                     entry->device == reveal.device && entry->node == reveal.node;
            });
        stale = row == visible.end() || !Listi::SameSearchEntry(reveal);
        if (stale)
          entries.SetSelection({}, "");
        else
          entries.SetSelection({reveal.path}, reveal.path);
        reveal = {};
      }
      if (pendingVisit && !restoring) RestoreVisit();
      else SendSelection();
      if (stale)
        SetStatus(
            "Search result changed during navigation. Search again; nothing selected.");
      if (m->GetBool("watchFailed", false))
        SetStatus("Live updates unavailable. Reload to refresh.");
      if (changedColumn)
        SetStatus("Column entry changed. Select it again.");
      else if (action.GetBool("context", false))
        ContextMenu(picked, action.GetPoint("screen", BPoint()));
      else if (action.GetBool("invoke", false) && !entries.Selected().empty()) {
        BMessage invoke(action);
        invoke.what = kInvoke;
        invoke.AddString("column", path);
        Window()->PostMessage(&invoke, this);
      }
    }
    if (restoring) {
      if (locationValid) {
        std::string focus;
        for (auto *row : entries.Visible())
          if (row->path == restoring->focused &&
              std::make_pair(row->device, row->node) == restoring->focusIdentity) {
            focus = row->path;
            break;
          }
        entries.SetSelection(entries.Selected(), focus);
        Sort(restoring->sorting);
        RestoreScroll(restoring->scroll);
        RestoreVisit();
      }
      restoring.reset();
    }
    if (locationValid && m->HasString("inspectionWarning"))
      SetNavigationError(m->GetString("inspectionWarning", ""));
    if (refreshPending) {
      directoryRefreshTimer.reset();
      directoryRefreshPending = false;
      Reload();
    }
    break;
  }
  case Listi::kDiskDeviceUpdate:
    if (path == "@disks")
      ScheduleDirectoryRefresh();
    break;
  case Listi::kDiskActionDone: {
    const auto result = m->GetInt32("status", B_ERROR);
    const bool mount = m->GetBool("mount", false);
    if (result != B_OK)
      PostMainWindowNotice(Listi::DiskActionError(mount, result).c_str());
    if (path == "@disks")
      ScheduleDirectoryRefresh();
    break;
  }
  case B_NODE_MONITOR: {
    const auto opcode = m->GetInt32("opcode", 0);
    const bool volumeChange = opcode == B_DEVICE_MOUNTED || opcode == B_DEVICE_UNMOUNTED;
    const auto device = m->GetInt32("device", -1);
    if (opcode == B_DEVICE_UNMOUNTED && device >= 0 && path != "@disks" &&
        (folderIdentity.first == device ||
         (directoryWatch && directoryWatch->WatchesDevice(serial, device)))) {
      // Navigate without inspecting any cached entry on the retired mount.
      // This also invalidates pending row batches and clears the selection.
      Navigate("@disks");
    } else if (volumeChange == (path == "@disks"))
      ScheduleDirectoryRefresh();
    break;
  }
  case B_COLORS_UPDATED:
    Apply(prefs);
    SetSource(active);
    break;
  default:
    if (!sidebar->HandleCommand(*m))
      BView::MessageReceived(m);
    break;
  }
}
