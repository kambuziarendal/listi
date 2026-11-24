// SPDX-License-Identifier: MIT
#include "FileListExportWindow.h"
#include "FileListExport.h"
#include <Alert.h>
#include <Button.h>
#include <Directory.h>
#include <File.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <RadioButton.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextView.h>
#include <Window.h>
#include <memory>

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
  explicit ExportWindow(const PaneModel &model)
      : BWindow(BRect(0, 0, 620, 300), "Export file list", B_TITLED_WINDOW,
                B_AUTO_UPDATE_SIZE_LIMITS), rows(FreezeFileList(model)) {
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
      BDirectory dir(&ref);
      BFile output(&dir, name, B_WRITE_ONLY | B_CREATE_FILE | B_FAIL_IF_EXISTS);
      if (output.InitCheck() != B_OK) {
        (new BAlert("Export", "Could not create the CSV. Choose a new filename in a writable folder.", "OK"))->Go();
        return;
      }
      auto write = [&](const std::string &s) {
        return output.Write(s.data(), s.size()) == static_cast<ssize_t>(s.size());
      };
      bool ok = write(kExportHeader);
      for (const auto &row : rows)
        if (ok && (!selected->Value() || row.selected))
          ok = write(row.csv);
      if (!ok || output.Sync() != B_OK) {
        (new BAlert("Export", "Writing failed. The CSV may be incomplete.", "OK"))->Go();
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
void ShowFileListExport(const PaneModel &model) {
  (new ExportWindow(model))->Show();
}
}
