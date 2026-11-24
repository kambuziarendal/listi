// SPDX-License-Identifier: MIT
#pragma once
#include "WorkspaceLayout.h"
#include <Font.h>
#include <InterfaceDefs.h>
#include <Message.h>
#include <Messenger.h>
#include <String.h>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>
// Stable installed-app identity; not a display name. Retained for upgrades.
constexpr const char *kSignature = "application/x-vnd.Kambuzi-Lister";
enum : uint32 {
  kGridMode = 'gmod',
  kRememberView = 'grem',
  kForgetView = 'gfor',
  kSort = 'sort',
  kWorkspace = 'wksp',
  kContextMenu = 'ctxm',
  kActivate = 'actv',
  kNavigate = 'navi',
  kBack = 'back',
  kForward = 'frwd',
  kPlaceAction = 'plac',
  kAddShortcut = 'padd',
  kRestoreSidebar = 'sbrs',
  kPlaceAccepted = 'pacc',
  kSavePlaces = 'psav',
  kPlacesSaved = 'psvd',
  kScan = 'scan',
  kRows = 'rows',
  kScanDone = 'done',
  kInvoke = 'invo',
  kSelection = 'sels',
  kPreviewSelection = 'pvsl',
  kPreviewToggle = 'pvtg',
  kPreviewOtherPanel = 'pvop',
  kCloseQuickView = 'qvcl',
  kPaneDisplay = 'pnds',
  kParent = 'prnt',
  kRoot = 'root',
  kHome = 'home',
  kDesktop = 'desk',
  kDisks = 'disk',
  kProperties = 'prop',
  kFileTypeIcon = 'ftic',
  kFilePanelFavorite = 'fpfa',
  kFilePanelFavoriteNamed = 'fpfn',
  kOpenWith = 'opwi',
  kClipboardCopy = 'ccpy',
  kExportFileList = 'exfl',
  kCopyPaths = 'cpat',
  kAboutListi = 'abli',
  kCopyNames = 'cpnm',
  kCopyRelative = 'cpre',
  kClipboardCut = 'ccut',
  kClipboardPaste = 'cpst',
  kClipboardSaveText = 'ctsf',
  kClipboardSaveImage = 'cpsg',
  kDuplicate = 'dupl',
  kCreateLink = 'link',
  kCreateArchive = 'arcr',
  kExtractArchive = 'arex',
  kAddArchive = 'arad',
  kRemoveArchive = 'arrm',
  kCopyToWindow = 'cpwi',
  kCopy = 'copy',
  kMove = 'move',
  kDelete = 'dele',
  kPermanentDelete = 'pdel',
  kRestore = 'rest',
  kTrashFolder = 'trsh',
  kMkdir = 'mkdr',
  kNewFile = 'nefl',
  kRename = 'rnam',
  kBulkRename = 'brnm',
  kBulkRenameAccepted = 'brac',
  kUndoBulkRename = 'ubrn',
  kView = 'view',
  kEdit = 'edit',
  kAll = 'sall',
  kSelectType = 'styp',
  kSelectPattern = 'spat',
  kCompareFolders = 'fcmp',
  kNone = 'snon',
  kInvert = 'sinv',
  kSwap = 'swap',
  kSame = 'same',
  kRefresh = 'rfsh',
  kSearch = 'srch',
  kRevealSearch = 'srev',
  kFilter = 'fltr',
  kFilterChanged = 'flch',
  kClearFilter = 'flcl',
  kInlineCancel = 'ican',
  kFilterDone = 'fldn',
  kTerminal = 'term',
  kPreferences = 'pref',
  kSharedPreferences = 'shpf',
  kApplyPreferences = 'aprf',
  kCommand = 'cmdl',
  kDrop = 'drop',
  kSwitch = 'swch',
  kJobUpdate = 'jupd',
  kFileProgress = 'fprg',
  kFileErrorSelected = 'ferr',
  kJobDone = 'jdon',
  kQueueWake = 'qwke',
  kPause = 'paus',
  kAbort = 'abor',
  kConflict = 'conf',
  kAnswer = 'answ',
  kShowJobs = 'jobs',
  kReviewRecovery = 'rvrc',
  kNameAccepted = 'name',
  kOpenPaths = 'opth'
};
struct Preferences : WorkspaceLayout {
  BFont font = *be_plain_font;
  enum FolderOpening : int32 { NewWindow, NewTab, Ask };
  inline static constexpr const char *FolderOpeningLabels[] = {
      "Independent window", "Folder tab", "Ask each time"};
  FolderOpening folderOpening = NewWindow;
  bool separateWindows = true;
  bool icons = true, size = true, date = true, alwaysFolderTabs = false;
  // Quick overrides are independent of saved layout profiles.
  int32 tviskiptButtons = -1, smidjaButtons = -1;
  bool ActionButtonsVisible() const {
    int32 choice = IsSmidja() ? smidjaButtons : tviskiptButtons;
    return SinglePane() || choice < 0 ? buttons : choice == 1;
  }
  WorkspaceProfiles profiles;
  WorkspaceLayout CaptureLayout() const { return *this; }
  void ApplyLayout(const WorkspaceLayout &layout) {
    static_cast<WorkspaceLayout &>(*this) = layout;
  }
  void ResetLayout() { ApplyLayout(WorkspaceLayout{}); }
  std::map<std::string, int32> folderModes;
  BString editor = "/boot/system/apps/StyledEdit",
          terminal = "/boot/system/apps/Terminal";
  BString folders = "system", files = "system", links = "system";
  rgb_color Color(const BString &value) const;
  void Load(const BMessage &);
  BMessage SharedChanges(const Preferences *before = nullptr) const;
  void ApplyShared(const BMessage &);
  void ApplyDialog(const BMessage &);
  BMessage Save() const;
};
bool PreferencesOpenFor(BMessenger owner);
void OpenPreferences(BMessenger target, const Preferences &);
