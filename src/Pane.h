// SPDX-License-Identifier: MIT
#pragma once
#include "Common.h"
#include "IconCache.h"
#include "PaneModel.h"
#include "PaneNavigationState.h"
#include "SavedPlaces.h"
#include "FileSearch.h"
#include <Button.h>
#include <Looper.h>
#include <StringView.h>
#include <TextControl.h>
#include <View.h>
#include <functional>
#include <optional>
class FileDetails;
class FileGrid;
class ImagePreview;
class InlineAction;
class OperationQueue;
class PaneSidebar;
class MillerColumns;
class WrappedLabel;
class BCardView;
class BGroupView;
class BSplitView;
class BMenuField;
namespace Listi {
class ReadWorker;
class Shelf;
inline constexpr uint32 kApplyPattern = 'spap';
}
class BMessageRunner;
class Pane : public BView {
public:
  Pane(int32 id, const Preferences &, std::shared_ptr<Listi::Shelf> = {});
  ~Pane();
  void AttachedToWindow() override;
  void Draw(BRect) override;
  void MessageReceived(BMessage *) override;
  void Navigate(const char *path);
  void NavigateFromColumn(const std::string &folder,
                          const std::string &selection = {},
                          bool invoke = false, bool context = false,
                          BPoint at = {}, const PaneEntry *clicked = nullptr);
  Listi::PaneNavigationState CaptureNavigation();
  void RestoreNavigation(const Listi::PaneNavigationState &);
  bool IsSearchList() const { return queries.count(path.String()) != 0; }
  std::string LocationLabel() const;
  const char *SearchRoot() const {
    auto query = queries.find(path.String());
    return query == queries.end() ? path.String() : query->second.request.root.c_str();
  }
  void Reload();
  void SetPlaces(const Listi::SavedPlaces &, bool writable);
  void SetSource(bool);
  bool IsSource() const { return active; }
  void Apply(const Preferences &);
  void SetWorkspace(const Preferences &);
  void Select(int mode);
  bool HandleSelectionCommand(const BMessage &, InlineAction &,
                              OperationQueue &);
  bool HandleOpenCommand(uint32, const BString &editor,
                         const BMessenger &service,
                         const std::function<ImagePreview *()> &openPreview);
  int64 SelectionRevision() const {
    return locationValid && !scanning ? selectionRevision : -1;
  }
  size_t MatchPattern(const char *pattern, bool apply);
  std::string SelectedFileType() const { return entries.FocusedFileType(); }
  std::vector<std::string> Selected() const;
  std::map<std::string, Listi::NodeIdentity> SelectedLocalIdentities(size_t limit = SIZE_MAX) const {
    return entries.SelectedLocalIdentities(limit);
  }
  std::string SelectedFolder() const {
    const auto *entry = entries.OnlySelected();
    return entry && entry->directory ? entry->path : "";
  }
  std::string SelectedPath() const {
    const auto *entry = entries.OnlySelected();
    return entry ? entry->path : "";
  }
  const char *Path() const { return path.String(); }
  const PaneModel &ExportModel() const { return entries; }
  bool CancelSelectionGesture();
  void FocusList();
  void ToggleTreeFocus();
  void FocusFilter();
  void Activate();
  bool IsEinn() const { return einn; }
  bool IsScanning() const { return scanning; }
  void SendSelection(bool gesture = false);
  void ContextMenu(const std::string &entry, BPoint screen);
  void SetHistoryHint(bool visible, bool enabled);
  bool HasHistoryHint() const;
  void ShowStatus(const char *text) { SetStatus(text); }
  void RequestVisibleIcon(PaneEntry &);
  PaneSort Sorting();
  void Sort(PaneSort);
  int32 CurrentView() const { return mode; }
  int32 PaneOverride() const {
    return prefs.HasPaneMode(id) ? prefs.PaneMode(id) : -1;
  }
  bool CanToggleTree() const;
  bool SidebarCanBeRestored() const;
  Listi::VisitPosition::Identity FolderIdentity() const { return folderIdentity; }
  bool LocationValid() const { return locationValid; }
  void RefreshShelf();
  SidebarMode Sidebar() const { return prefs.Sidebar(id); }
  void FolderModes(const std::map<std::string, int32> &modes) {
    prefs.folderModes = modes;
  }

private:
  int32 id;
  Preferences prefs;
  BString path = "/boot/home";
  PaneModel entries;
  int64 selectionRevision = 0;
  void SelectByPattern(const BMessage &, InlineAction &, OperationQueue &);
  std::vector<std::string> attributeColumns;
  bool attributeDetails = false;
  IconCache icons;
  Listi::ReadWorker *iconReader = nullptr;
  std::map<std::string, PaneEntry *> pendingIcons;
  FileDetails *list;
  FileGrid *grid;
  MillerColumns *columns;
  PaneSidebar *sidebar;
  bool einn = false;
  BCardView *cards;
  BGroupView *header, *filterRow;
  BSplitView *content;
  bool chromeVisible = true;
  bool treeOnRight = false, filterAtBottom = false;
  float contentSplitterSize = 0;
  BMenuField *display;
  int32 mode = 0;
  void SetDisplayMode(int32, bool appendedDetails = false);
  int32 LocationMode() const;
  void PublishDisplay(bool folder = false, bool forget = false);
  bool locationValid = false;
  BTextControl *edit;
  BButton *back, *forward, *historyButton;
  bool historyEnabled = false;
  Listi::NavigationHistory history;
  std::map<std::string, Listi::SavedSearch> queries;
  std::optional<Listi::PaneNavigationState> restoring;
  int workingSetView = -1;
  int restoredView = -1;
  Listi::ReadWorker *queryWorker = nullptr;
  std::shared_ptr<std::atomic<int64>> queryFailure = std::make_shared<std::atomic<int64>>(-1);
  std::vector<std::unique_ptr<PaneEntry>> queryRows;
  void OpenSearchList(const Listi::SavedSearch &);
  bool HandleQueryReply(BMessage &);
  BPoint ScrollPosition();
  PaneEntry *ScrollRow(const std::string *, float &);
  Listi::VisitPosition CaptureVisit();
  void RestoreVisit();
  std::optional<Listi::VisitPosition> pendingVisit;
  Listi::VisitPosition::Identity folderIdentity{-1, -1};
  void RestoreScroll(BPoint);

  void StartNavigation(const std::string &, int target = -1,
                       bool trash = false, bool restore = false);
  void UpdateNavigation();
  BTextControl *filter;
  BButton *clearFilter, *clearEmptyFilter;
  WrappedLabel *status;
  bool navigationFailed = false;
  void SetFilterEmptyState(bool noMatches);
  void SetStatus(const char *);
  void SetNavigationError(const char *);
  void UpdateActivityAppearance();
  void InvalidateActivityBorder();
  std::unique_ptr<BMessageRunner> activityTimer;
  int64 activitySerial = 0;
  bigtime_t activityStarted = 0;
  int activitySteps = 0;
  float activityLevel = 0, activityFrom = 0;
  bool active = false;
  struct Transfer {
    float fraction = 0;
    int32 state = 0;
    std::string error;
    int64 expires = 0;
  };
  std::unique_ptr<BMessageRunner> progressTimer;
  std::map<std::string, Transfer> transfers;
  Listi::ReadWorker *scanner = nullptr;
  void StartDirectoryReader();
  void AfterInterruptedCopy(const char *folder);
  bool mediaReadSuspended = false;
  std::shared_ptr<class DirectoryWatch> directoryWatch;
  std::shared_ptr<struct DirectoryScanDelivery> scanDelivery;
  std::unique_ptr<BMessageRunner> scanTimer;
  std::unique_ptr<BMessageRunner> directoryRefreshTimer;
  bool directoryRefreshPending = false;
  void ScheduleDirectoryRefresh();
  std::shared_ptr<std::atomic<int64>> generation;
  int64 serial = 0;
  bool scanning = false, refreshPending = false;
  std::map<std::string, std::pair<int64_t, int64_t>> savedSelection;
  BMessage columnAction;
  Listi::SearchHit reveal;
  void ApplyFilter();
  void ClearRows();
};
