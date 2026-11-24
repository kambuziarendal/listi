NAME = Listi
TYPE = APP
SRCS = SearchFilters.cpp LocalMoveTree.cpp Sha256.cpp FolderCompare.cpp FolderCompareWindow.cpp ArchiveEditor.cpp ArchiveEditorWindow.cpp ArchiveVolumes.cpp SevenZipBackend.cpp ArchivePasswords.cpp ArchivePasswordWindow.cpp ArchiveSettings.cpp CabWriter.cpp ArchiveJobs.cpp ArchiveWriting.cpp ArchiveActions.cpp DataIsoVerification.cpp ArchiveWindow.cpp JobSnapshot.cpp FileContextMenu.cpp PaneContextMenu.cpp NamedEntry.cpp FileDrop.cpp FileDropFlow.cpp OpenWith.cpp OpenWithWindow.cpp FileProperties.cpp PropertiesWindow.cpp FileClipboard.cpp MessageStore.cpp SavedSearches.cpp SearchWorker.cpp FileSearch.cpp SearchWindow.cpp SavedPlaces.cpp App.cpp ApplicationServices.cpp WindowKeyFilter.cpp Pane.cpp PaneModel.cpp FileDetails.cpp FileGrid.cpp DirectoryScanner.cpp FileOperation.cpp DirectoryFinalization.cpp RemovableCopy.cpp Trash.cpp OperationQueue.cpp Preferences.cpp SelfTest.cpp VirtualFile.cpp ArchiveFile.cpp LibarchiveBackend.cpp NetworkFile.cpp VirtualTest.cpp VirtualOperation.cpp ConnectionView.cpp InlineAction.cpp ImagePreview.cpp FolderTree.cpp
SRCS += RarStore.cpp ArWriter.cpp ArjWriter.cpp LhaWriter.cpp Shelf.cpp ShelfController.cpp ShelfWindow.cpp ShelfCollections.cpp ShelfIdentity.cpp ImageConvert.cpp BulkRename.cpp BulkRenameWindow.cpp ClipboardFlow.cpp ClipboardTextFile.cpp ClipboardImage.cpp
SRCS += WindowOverview.cpp LayoutIntroduction.cpp MainWindow.cpp MainWindowMenu.cpp FileOperationFlow.cpp WindowScripting.cpp FunctionButtonBank.cpp WorkspaceToolbar.cpp SavedNavigationController.cpp PaneSidebar.cpp
SRCS += PaneNavigation.cpp PaneCommands.cpp MillerColumns.cpp TransferCommand.cpp JobResultsView.cpp JobReportStore.cpp
SRCS += ArchiveJobPreparation.cpp ArchiveJobChecks.cpp RecoveryInventory.cpp RecoveryReviewWindow.cpp VirtualLocation.cpp
SRCS += FlatView.cpp WorkspaceLayout.cpp WorkspaceEditor.cpp WorkspaceSplit.cpp PanelTools.cpp FolderTabs.cpp InspectorContent.cpp InspectorDocument.cpp InspectorEditor.cpp InspectorPhoto.cpp
SRCS += InspectorTextView.cpp SearchResultsView.cpp ReadWorker.cpp
SRCS += FileListExportWindow.cpp FileListExport.cpp
SRCS += TerminalDock.cpp TerminalPanel.cpp TerminalSession.cpp $(wildcard vendor/libvterm/src/*.c)
LOCAL_INCLUDE_PATHS += vendor/libvterm/include
LIBS = shared translation textencoding media be columnlistview tracker sqlite3 $(STDCPPLIBS)
RDEFS = Listi.rdef
OPTIMIZE := FULL
WARNINGS = ALL
COMPILER_FLAGS = -std=c++17 -Wno-multichar
SYSTEM_INCLUDE_PATHS = /boot/system/develop/headers/samba-4.0 /boot/system/develop/headers/private/interface /boot/system/develop/headers/private/shared
DEVEL_DIRECTORY := $(shell findpaths -r makefile_engine B_FIND_PATH_DEVELOP_DIRECTORY)
include $(DEVEL_DIRECTORY)/etc/makefile-engine

.PHONY: listi-self-test listi-audit-config
listi-self-test: $(TARGET)
	cd .. && "$(abspath $(TARGET))" --self-test

# Keep configuration in the native build, including the exact (not globbed) object list.
listi-audit-config:
	@printf '%s\n' 'listi:target=$(TARGET)' 'listi:compiler=$(C++)' \
		'listi:objects=$(strip $(OBJS))' 'listi:flags=$(strip $(INCLUDES) $(CXXFLAGS))' \
		'listi:libraries=$(strip $(LINK_PATHS) $(LINK_LIBS) $(LINKER_FLAGS))'

# Check each invocation; unchanged metadata must not recompile the window.
.PHONY: listi-check-identity
../build/ListiBuildIdentity.h: listi-check-identity
	python3 ../ci/build-identity.py $@
$(OBJ_DIR)/MainWindow.o $(OBJ_DIR)/MainWindow.d: ../build/ListiBuildIdentity.h
