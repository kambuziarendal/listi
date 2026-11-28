// SPDX-License-Identifier: MIT
#include "FileListExportWindow.h"
#include "DirectoryFinalization.h"
#include "ExclusiveFileOutput.h"
#include "FileListExport.h"
#include "Pane.h"
#include <Alert.h>
#include <Button.h>
#include <Directory.h>
#include <Entry.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <Path.h>
#include <RadioButton.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextView.h>
#include <Window.h>
#include <memory>
#include <stdexcept>

namespace Listi {
namespace {
class ExportWindow : public BWindow {
  std::vector<ExportRow> rows;
  BRadioButton *selected, *all;
  BStringView *count;
  BTextView *preview;
  BButton *save;
  std::unique_ptr<BFilePanel> panel;
  void Refresh() {
    size_t n = 0;
    std::string sample = kExportHeader;
    for (const auto &row : rows) {
      if (selected->Value() && !row.selected)
        continue;
      if (++n <= 3)
        sample += row.csv;
    }
    count->SetText((std::to_string(n) + (n == 1 ? " row" : " rows")).c_str());
    preview->SetText(sample.c_str());
    save->SetEnabled(n != 0);
  }
public:
  explicit ExportWindow(const Pane &pane)
      : BWindow(BRect(0, 0, 620, 300), "Export file list", B_TITLED_WINDOW,
                B_AUTO_UPDATE_SIZE_LIMITS), rows(FreezeFileList(pane.ExportModel(), pane.DisplayedRows())) {
    selected = new BRadioButton("selected", "Selected items", new BMessage('scop'));
    all = new BRadioButton("all", "All visible items", new BMessage('scop'));
    save = new BButton("save", "Save CSV…", new BMessage('pick'));
    count = new BStringView("count", "");
    preview = new BTextView("preview");
    preview->MakeEditable(false);
    BLayoutBuilder::Group<>(this, B_VERTICAL)
      .SetInsets(B_USE_DEFAULT_SPACING)
      .AddGroup(B_HORIZONTAL).Add(selected).Add(all).AddGlue().End()
      .Add(count).Add(new BScrollView("sample", preview, 0, false, true))
      .AddGroup(B_HORIZONTAL).AddGlue().Add(save).End();
    bool hasSelection = false;
    for (const auto &row : rows)
      hasSelection |= row.selected;
    (hasSelection ? selected : all)->SetValue(B_CONTROL_ON);
    Refresh();
    CenterOnScreen();
  }
  void MessageReceived(BMessage *message) override {
    switch (message->what) {
    case 'scop': Refresh(); return;
    case 'pick': {
      BMessenger target(this);
      panel.reset(new BFilePanel(B_SAVE_PANEL, &target, nullptr, 0, false,
                                 nullptr, nullptr, true));
      panel->SetSaveText("file-list.csv");
      panel->Show();
      return;
    }
    case B_SAVE_REQUESTED: {
      entry_ref ref;
      const char *name;
      if (message->FindRef("directory", &ref) != B_OK ||
          message->FindString("name", &name) != B_OK)
        return;
      try {
        BDirectory dir(&ref);
        BEntry entry;
        BPath path;
        node_ref expected;
        if (dir.GetNodeRef(&expected) != B_OK ||
            dir.GetEntry(&entry) != B_OK || entry.GetPath(&path) != B_OK)
          throw std::runtime_error("The destination folder is no longer available.");
        PinnedDirectory folder(path.Path(), expected.device, expected.node, true);
        SaveExclusiveFile(folder.Descriptor(), name, 0666, [&](int fd) {
          WriteOutputBytes(fd, kExportHeader, sizeof(kExportHeader) - 1);
          for (const auto &row : rows)
            if (!selected->Value() || row.selected)
              WriteOutputBytes(fd, row.csv.data(), row.csv.size());
        }, [&] { folder.Verify(); });
      } catch (const std::exception &error) {
        (new BAlert("Export", error.what(), "OK"))->Go();
        return;
      }
      PostMessage(B_QUIT_REQUESTED);
      return;
    }
    default: BWindow::MessageReceived(message);
    }
  }
};
}
void ShowFileListExport(const Pane &pane) {
  (new ExportWindow(pane))->Show();
}
}
