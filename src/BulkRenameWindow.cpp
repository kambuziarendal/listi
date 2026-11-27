// SPDX-License-Identifier: MIT
#include "ColumnRows.h"
#include "BulkRenameWindow.h"
#include "BulkRename.h"
#include "Common.h"
#include "JobSnapshot.h"
#include <Button.h>
#include <CheckBox.h>
#include "AdaptiveColumnList.h"
#include <ColumnTypes.h>
#include <LayoutBuilder.h>
#include <String.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>
#include <charconv>
#include <cstring>
#include <stdexcept>

namespace {
constexpr uint32 kOptionsChanged = 'broc', kPreview = 'brpv', kApply = 'brap',
                 kSelectRow = 'brsr', kSetName = 'brsn', kResetName = 'brrn',
                 kRowNameChanged = 'brne';

unsigned Number(const BTextControl *field, const char *error) {
  unsigned value;
  const char *begin = field->Text(), *end = begin + strlen(begin);
  const auto parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc() || parsed.ptr != end)
    throw std::runtime_error(error);
  return value;
}

class BulkRenameWindow : public BWindow {
  const Listi::Job frozen;
  const BMessenger target;
  Listi::BulkRenameOptions previewOptions;
  Listi::BulkRenamePreview preview;
  std::map<std::string, std::string> overrides;
  BTextControl *rowName;
  BButton *setName, *resetName;
  BTextControl *search, *replacement, *prefix, *suffix, *start, *digits;
  BCheckBox *number;
  BColumnListView *rows;
  BStringView *status;
  BButton *apply;
  int32 editedRow = -1;
  bool queued = false, previewInvalidated = false;

  BTextControl *Field(const char *label, const char *initial = "") {
    auto field = new BTextControl(label, label, initial, nullptr);
    field->SetModificationMessage(new BMessage(kOptionsChanged));
    return field;
  }

  Listi::BulkRenameOptions Options() const {
    Listi::BulkRenameOptions options;
    options.search = search->Text();
    options.replacement = replacement->Text();
    options.prefix = prefix->Text();
    options.suffix = suffix->Text();
    options.number = number->Value() == B_CONTROL_ON;
    options.overrides = overrides;
    if (options.number) {
      options.start = Number(start, "Enter a nonnegative starting number.");
      options.digits = Number(digits, "Enter a number width from 1 to 9.");
    }
    return options;
  }

  bool HasCurrentPreview() const {
    if (previewInvalidated)
      return false;
    try {
      const auto current = Options();
      return current.prefix == previewOptions.prefix &&
             current.suffix == previewOptions.suffix &&
             current.search == previewOptions.search &&
             current.replacement == previewOptions.replacement &&
             current.number == previewOptions.number &&
             current.start == previewOptions.start &&
             current.digits == previewOptions.digits &&
             current.overrides == previewOptions.overrides;
    } catch (const std::runtime_error &) {
      return false;
    }
  }

  int32 SelectedRow() const {
    auto *selected = rows->CurrentSelection();
    return selected ? rows->IndexOf(selected) : -1;
  }

  void UpdateRowEditor() {
    editedRow = SelectedRow();
    const bool selected = editedRow >= 0 && size_t(editedRow) < preview.rows.size();
    rowName->SetText(selected ? preview.rows[editedRow].after.c_str() : "");
    rowName->SetEnabled(selected);
    setName->SetEnabled(selected);
    resetName->SetEnabled(selected &&
                          overrides.count(frozen.sources[editedRow]));
  }

  bool HasPendingName() const {
    return editedRow >= 0 && size_t(editedRow) < preview.rows.size() &&
           preview.rows[editedRow].after != rowName->Text();
  }

  void UpdateApplyState() {
    const bool current = HasCurrentPreview();
    const bool pending = HasPendingName();
    apply->SetEnabled(!queued && current && !pending && preview.CanApply());
    if (!preview.error.empty()) {
      status->SetText(preview.error.c_str());
    } else if (!current) {
      status->SetText("Update the preview to check the new names.");
    } else if (pending) {
      status->SetText("Click Set name to include the edit in the preview.");
    } else {
      BString summary;
      summary << int32(preview.changed) << " name changes";
      if (preview.blocked)
        summary << ", " << int32(preview.blocked) << " blocked rows";
      status->SetText(summary.String());
    }
  }

  void Render(int32 selected = -1) {
    Listi::ClearColumnRows(*rows);
    preview = {};
    previewInvalidated = false;
    try {
      previewOptions = Options();
      preview = Listi::PreviewBulkRename(frozen, previewOptions);
    } catch (const std::runtime_error &error) {
      preview.error = error.what();
    }
    for (const auto &entry : preview.rows) {
      auto row = new BRow();
      row->SetField(new BStringField(entry.before.c_str()), 0);
      row->SetField(new BStringField(entry.after.c_str()), 1);
      row->SetField(new BStringField(
                        !entry.error.empty() ? entry.error.c_str()
                        : entry.before == entry.after ? "Unchanged"
                                                     : "Ready"),
                    2);
      rows->AddRow(row);
    }
    if (selected >= 0 && size_t(selected) < preview.rows.size())
      rows->SetFocusRow(rows->RowAt(selected), true);
    UpdateRowEditor();
    UpdateApplyState();
  }

public:
  BulkRenameWindow(Listi::Job job, const BMessenger &receiver)
      : BWindow(BRect(130, 120, 930, 650), "Listi: Bulk rename",
                B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS),
        frozen(std::move(job)), target(receiver) {
    search = Field("Find:");
    replacement = Field("Replace with:");
    prefix = Field("Prefix:");
    suffix = Field("Suffix:");
    start = Field("Start at:", "1");
    digits = Field("Minimum digits:", "1");
    start->SetEnabled(false);
    digits->SetEnabled(false);
    number = new BCheckBox("Add number", new BMessage(kOptionsChanged));
    rows = new Listi::AdaptiveColumnList("rename preview", B_WILL_DRAW | B_NAVIGABLE,
                               B_FANCY_BORDER);
    rows->AddColumn(
        new BStringColumn("Current name", 215, 100, 1000, B_TRUNCATE_MIDDLE), 0);
    rows->AddColumn(
        new BStringColumn("New name", 215, 100, 1000, B_TRUNCATE_MIDDLE), 1);
    rows->AddColumn(
        new BStringColumn("Status", 325, 100, 1200, B_TRUNCATE_END), 2);
    rows->SetSortingEnabled(false);
    rows->SetSelectionMode(B_SINGLE_SELECTION_LIST);
    rows->SetSelectionMessage(new BMessage(kSelectRow));
    rows->SetTarget(this);
    rows->SetExplicitMinSize(BSize(630, 200));
    rowName = new BTextControl("row-name", "Selected new name:", "", nullptr);
    rowName->SetModificationMessage(new BMessage(kRowNameChanged));
    setName = new BButton("set-row-name", "Set name", new BMessage(kSetName));
    resetName = new BButton("reset-row-name", "Use rules",
                            new BMessage(kResetName));
    status = new BStringView("rename status", "");
    apply = new BButton("apply-rename", "Apply rename", new BMessage(kApply));
    BLayoutBuilder::Group<>(this, B_VERTICAL, 8)
        .SetInsets(10)
        .Add(new BStringView("rename folder", frozen.destinationFolder.c_str()))
        .AddGroup(B_HORIZONTAL, 10)
            .Add(search)
            .Add(replacement)
        .End()
        .AddGroup(B_HORIZONTAL, 10)
            .Add(prefix)
            .Add(suffix)
        .End()
        .AddGroup(B_HORIZONTAL, 10)
            .Add(number)
            .Add(start)
            .Add(digits)
        .End()
        .Add(new BStringView(
            "rename hint",
            "Replacements are case-sensitive. File extensions are kept."))
        .Add(rows, 1)
        .AddGroup(B_HORIZONTAL, 8)
            .Add(rowName)
            .Add(setName)
            .Add(resetName)
        .End()
        .Add(status)
        .AddGroup(B_HORIZONTAL, 8)
            .Add(new BButton("update-preview", "Update preview",
                             new BMessage(kPreview)))
            .AddGlue()
            .Add(new BButton("Cancel", new BMessage(B_QUIT_REQUESTED)))
            .Add(apply)
        .End();
    for (auto field : {search, replacement, prefix, suffix, start, digits, rowName})
      field->SetTarget(this);
    Render();
    search->MakeFocus();
  }

  void MessageReceived(BMessage *message) override {
    switch (message->what) {
    case kBulkRename:
      Activate();
      break;
    case kOptionsChanged:
      start->SetEnabled(number->Value() == B_CONTROL_ON);
      digits->SetEnabled(number->Value() == B_CONTROL_ON);
      if (!HasCurrentPreview()) {
        previewInvalidated = true;
        preview.error.clear();
        Listi::ClearColumnRows(*rows);
        UpdateRowEditor();
      }
      UpdateApplyState();
      break;
    case kPreview: {
      // A refresh checks the filesystem again; it does not submit or discard
      // the selected row's text. Only Set name commits that draft.
      const int32 selected = SelectedRow();
      const bool keepDraft = selected == editedRow && HasPendingName();
      const std::string draft = keepDraft ? rowName->Text() : "";
      Render(selected);
      if (keepDraft && editedRow == selected) {
        rowName->SetText(draft.c_str());
        UpdateApplyState();
      }
      break;
    }
    case kSelectRow:
      if (editedRow != SelectedRow())
        UpdateRowEditor();
      UpdateApplyState();
      break;
    case kRowNameChanged:
      UpdateApplyState();
      break;
    case kSetName:
    case kResetName: {
      const auto index = SelectedRow();
      if (HasCurrentPreview() && index == editedRow && index >= 0 &&
          size_t(index) < preview.rows.size()) {
        const auto &source = frozen.sources[index];
        if (message->what == kSetName)
          overrides[source] = rowName->Text();
        else
          overrides.erase(source);
        Render(index);
      }
      break;
    }
    case kApply:
      if (HasPendingName()) {
        UpdateApplyState();
        break;
      }
      if (!queued && preview.CanApply() && HasCurrentPreview()) {
        try {
          auto accepted = Listi::EncodeJob(preview.job, kBulkRenameAccepted);
          if (target.SendMessage(&accepted) == B_OK) {
            queued = true;
            apply->SetEnabled(false);
            PostMessage(B_QUIT_REQUESTED);
          } else {
            status->SetText("Could not queue the rename. No changes were made.");
          }
        } catch (const std::exception &error) {
          status->SetText(error.what());
        }
      }
      break;
    default:
      BWindow::MessageReceived(message);
      break;
    }
  }
};
} // namespace

namespace Listi {
BMessenger OpenBulkRenameWindow(Job job, const BMessenger &target) {
  auto window = new BulkRenameWindow(std::move(job), target);
  BMessenger messenger(window);
  window->Show();
  return messenger;
}
}
