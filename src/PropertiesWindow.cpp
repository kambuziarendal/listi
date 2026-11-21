// SPDX-License-Identifier: MIT
#include "AdaptiveScrollView.h"
#include "ColumnRows.h"
#include "PropertiesWindow.h"
#include "FileProperties.h"
#include "ThemeTextView.h"
#include <Button.h>
#include <Alert.h>
#include <Volume.h>
#include <Application.h>
#include <algorithm>
#include "AdaptiveColumnList.h"
#include <ColumnTypes.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>
#include <thread>
#include <mutex>
#include <functional>
#include <sstream>
#include <iomanip>
namespace {
using namespace Listi;
constexpr uint32 kRefresh = 'prrf', kEdit = 'pred', kNew = 'prnw',
                 kSave = 'prsv', kRemove = 'prrm', kDiscard = 'prdc',
                 kPermissions = 'prpm', kFinished = 'prdn', kCanQuit = 'prcq';
std::mutex windowsMutex;
std::vector<BMessenger> propertyWindows;
struct AttributeRow : BRow {
  FileAttribute value;
  explicit AttributeRow(const FileAttribute &a) : BRow(23), value(a) {
    SetField(new BStringField(a.name.c_str()), 0);
    SetField(new BStringField(AttributeTypeName(a.type).c_str()), 1);
    std::string preview = a.text;
    for (char &c : preview)
      if (c == '\n' || c == '\r' || c == '\t')
        c = ' ';
    if (preview.size() > 200)
      preview = preview.substr(0, 200) + "...";
    SetField(new BStringField(preview.c_str()), 2);
  }
};
constexpr uint32 kMakeSearchable = 'mkix';
struct Delivery {
  std::string completion;
  std::mutex mutex;
  std::unique_ptr<PropertySnapshot> snapshot;
  std::shared_ptr<PropertySession> session;
  std::string error;
};
class PropertiesWindow : public BWindow {
  std::shared_ptr<PropertySession> session;
  std::shared_ptr<Delivery> delivery = std::make_shared<Delivery>();
  BMessenger pane;
  std::string path;
  PropertySnapshot snapshot;
  std::unique_ptr<FileAttribute> editing;
  BColumnListView *attributes;
  ThemeTextView *details, *value;
  BStringView *location, *status, *heading;
  BTextControl *name, *permissions;
  BPopUpMenu *types;
  BMenuField *typeField;
  BButton *save, *remove, *discard, *edit, *create, *refresh, *apply;
  BButton *showColumn, *index;
  bool busy = false, creating = false, confirmRemove = false;
  std::string originalName, originalValue;
  type_code originalType = B_STRING_TYPE;
  mode_t permissionBase = 0;
  type_code Type() const {
    if (editing)
      return editing->type;
    auto item = types->FindMarked();
    return item ? item->Message()->GetInt32("type", B_STRING_TYPE)
                : B_STRING_TYPE;
  }
  bool Dirty() const {
    return (editing || creating) &&
           (originalName != name->Text() || originalValue != value->Text() ||
            originalType != Type());
  }
  std::string PermissionText() const {
    std::ostringstream current;
    current << std::oct << std::setfill('0') << std::setw(3) << permissionBase;
    return current.str();
  }
  bool PermissionDirty() const {
    return session && PermissionText() != permissions->Text();
  }
  void Status(const std::string &s) {
    status->SetText(s.c_str());
    status->SetToolTip(s.c_str());
  }
  static BStringView *Line(const char *name, const char *text) {
    auto line = new BStringView(name, text);
    line->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
    line->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    line->SetTruncation(B_TRUNCATE_MIDDLE);
    return line;
  }
  void Controls() {
    bool writable = (editing && editing->editable) || creating;
    name->SetEnabled(!busy && creating);
    value->MakeEditable(!busy && writable);
    typeField->SetEnabled(!busy && creating);
    save->SetEnabled(!busy && writable);
    remove->SetEnabled(!busy && editing && editing->editable);
    discard->SetEnabled(!busy);
    edit->SetEnabled(!busy);
    showColumn->SetEnabled(!busy && pane.IsValid());
    index->SetEnabled(!busy && session);
    create->SetEnabled(!busy && snapshot.writableKind);
    refresh->SetEnabled(!busy);
    permissions->SetEnabled(!busy && snapshot.writableKind &&
                            !(snapshot.stat.st_mode & 07000));
    apply->SetEnabled(permissions->IsEnabled());
    remove->SetLabel(confirmRemove ? "Confirm remove" : "Remove attribute");
  }
  void Reset(bool keepPermissionDraft = false) {
    editing.reset();
    creating = false;
    confirmRemove = false;
    originalName = "";
    originalValue = "";
    originalType = B_STRING_TYPE;
    name->SetText("");
    value->SetText("");
    types->ItemAt(0)->SetMarked(true);
    heading->SetText("Choose an attribute and Edit, or create a new one.");
    if (!keepPermissionDraft) {
      permissionBase = snapshot.stat.st_mode & 07777;
      permissions->SetText(PermissionText().c_str());
    }
    Controls();
  }
  bool MayReplaceDraft() {
    if (Dirty() || PermissionDirty()) {
      Status(
          "Save or Discard your draft before loading another value or refreshing.");
      return false;
    }
    return true;
  }
  void Work(std::function<void(PropertySession &)> change = {},
            bool keepPermissionDraft = false) {
    if (busy)
      return;
    delivery->completion.clear();
    busy = true;
    Controls();
    Status(change ? "Saving metadata..." : "Reading properties...");
    auto holder = delivery;
    auto current = session;
    auto target = BMessenger(this);
    auto originalPath = path;
    std::thread([holder, current, target, originalPath, change, keepPermissionDraft]() mutable {
      BMessage done(kFinished);
      done.AddBool("keepPermissionDraft", keepPermissionDraft);
      bool changed = false;
      try {
        if (!current)
          current = std::make_shared<PropertySession>(originalPath);
        if (change) {
          change(*current);
          changed = true;
        }
        auto result = std::make_unique<PropertySnapshot>(current->Read());
        std::lock_guard<std::mutex> lock(holder->mutex);
        holder->snapshot = std::move(result);
        holder->session = current;
        holder->error.clear();
      } catch (const std::exception &e) {
        std::lock_guard<std::mutex> lock(holder->mutex);
        holder->snapshot.reset();
        holder->error =
            (changed ? std::string("Metadata saved, but refresh failed: ")
                     : std::string()) +
            e.what();
      }
      target.SendMessage(&done);
    }).detach();
  }
  void Colors() {
    attributes->SetColor(B_COLOR_BACKGROUND,
                         ui_color(B_DOCUMENT_BACKGROUND_COLOR));
    attributes->SetColor(B_COLOR_TEXT, ui_color(B_DOCUMENT_TEXT_COLOR));
    attributes->SetColor(B_COLOR_SELECTION,
                         ui_color(B_CONTROL_HIGHLIGHT_COLOR));
    attributes->SetColor(B_COLOR_SELECTION_TEXT,
                         ui_color(B_CONTROL_TEXT_COLOR));
    attributes->SetColor(B_COLOR_NON_FOCUS_SELECTION,
                         tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), .8f));
    attributes->Invalidate();
  }

public:
  PropertiesWindow(const std::string &p, BMessenger owner)
      : BWindow(BRect(100, 90, 850, 710), "Listi: Properties", B_TITLED_WINDOW,
                B_AUTO_UPDATE_SIZE_LIMITS),
        pane(owner), path(p) {
    location = Line("property path", p.c_str());
    location->SetToolTip(p.c_str());
    details = new ThemeTextView("property details", B_PANEL_BACKGROUND_COLOR,
                                B_PANEL_TEXT_COLOR);
    details->MakeEditable(false);
    details->MakeSelectable(true);
    details->SetExplicitMinSize(BSize(0, 62));
    details->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 72));
    permissions = new BTextControl("property permissions",
                                   "Permissions (octal):", "", nullptr);
    apply = new BButton("Apply permissions", new BMessage(kPermissions));
    refresh = new BButton("Refresh", new BMessage(kRefresh));
    attributes = new Listi::AdaptiveColumnList("property attributes",
                                     B_WILL_DRAW | B_NAVIGABLE, B_FANCY_BORDER);
    attributes->AddColumn(
        new BStringColumn("Attribute", 215, 80, 800, B_TRUNCATE_MIDDLE), 0);
    attributes->AddColumn(
        new BStringColumn("Type", 85, 60, 300, B_TRUNCATE_END), 1);
    attributes->AddColumn(
        new BStringColumn("Value / preview", 380, 80, 1500, B_TRUNCATE_END), 2);
    attributes->SetSelectionMode(B_SINGLE_SELECTION_LIST);
    attributes->SetInvocationMessage(new BMessage(kEdit));
    attributes->SetTarget(this);
    attributes->SetExplicitMinSize(BSize(300, 100));
    edit = new BButton("Edit selected", new BMessage(kEdit));
    showColumn =
        new BButton("Show in Details", new BMessage(kShowAttributeColumn));
    index = new BButton("Make searchable…", new BMessage(kMakeSearchable));
    create = new BButton("New attribute", new BMessage(kNew));
    heading = Line("property editing",
                   "Choose an attribute and Edit, or create a new one.");
    name = new BTextControl("attribute name", "Name:", "", nullptr);
    name->TextView()->SetMaxBytes(254);
    types = new BPopUpMenu("attribute types");
    types->SetRadioMode(true);
    for (type_code type :
         {B_STRING_TYPE, B_BOOL_TYPE, B_INT32_TYPE, B_UINT32_TYPE, B_INT64_TYPE,
          B_UINT64_TYPE, B_FLOAT_TYPE, B_DOUBLE_TYPE}) {
      auto message = new BMessage('prty');
      message->AddInt32("type", type);
      types->AddItem(new BMenuItem(AttributeTypeName(type).c_str(), message));
    }
    types->ItemAt(0)->SetMarked(true);
    typeField = new BMenuField("Type:", types);
    value = new ThemeTextView("attribute value", B_DOCUMENT_BACKGROUND_COLOR,
                              B_DOCUMENT_TEXT_COLOR);
    value->SetMaxBytes(4096);
    value->SetWordWrap(true);
    auto scroll = new Listi::AdaptiveScrollView("attribute editor", value, false);
    scroll->SetExplicitMinSize(BSize(0, 70));
    scroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 100));
    save = new BButton("Save attribute", new BMessage(kSave));
    remove = new BButton("Remove attribute", new BMessage(kRemove));
    discard = new BButton("Discard draft", new BMessage(kDiscard));
    status = Line("property status", "Reading properties...");
    auto close = new BButton("Close", new BMessage(B_QUIT_REQUESTED));
    BLayoutBuilder::Group<>(this, B_VERTICAL, 6)
        .SetInsets(10)
        .Add(location)
        .Add(details)
        .AddGroup(B_HORIZONTAL, 8)
        .Add(permissions)
        .Add(apply)
        .Add(refresh)
        .End()
        .Add(attributes)
        .AddGroup(B_HORIZONTAL, 8)
        .Add(edit)
        .Add(create)
        .Add(showColumn)
        .Add(index)
        .AddGlue()
        .End()
        .Add(heading)
        .AddGroup(B_HORIZONTAL, 8)
        .Add(name)
        .Add(typeField)
        .End()
        .Add(scroll)
        .AddGroup(B_HORIZONTAL, 8)
        .Add(save)
        .Add(remove)
        .Add(discard)
        .AddGlue()
        .Add(close)
        .End()
        .Add(status);
    AddShortcut('W', B_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
    AddShortcut(B_ESCAPE, B_NO_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
    {
      std::lock_guard<std::mutex> lock(windowsMutex);
      propertyWindows.emplace_back(this);
    }
    Reset();
    Colors();
    Show();
    PostMessage(kRefresh);
  }
  ~PropertiesWindow() override {
    std::lock_guard<std::mutex> lock(windowsMutex);
    propertyWindows.erase(std::remove(propertyWindows.begin(),
                                      propertyWindows.end(), BMessenger(this)),
                          propertyWindows.end());
  }
  bool QuitRequested() override {
    if (busy) {
      Status("Metadata work is still running. Please wait before closing.");
      return false;
    }
    if (Dirty() || PermissionDirty()) {
      Status("Unsaved changes: Save or Discard before closing.");
      return false;
    }
    return true;
  }
  void MessageReceived(BMessage *message) override {
    switch (message->what) {
    case kCanQuit: {
      bool allowed = QuitRequested();
      if (!allowed)
        Activate();
      BMessage reply(B_REPLY);
      reply.AddBool("allow", allowed);
      message->SendReply(&reply);
      break;
    }
    case kFinished: {
      busy = false;
      std::lock_guard<std::mutex> lock(delivery->mutex);
      if (!delivery->error.empty()) {
        Status(delivery->error);
        Controls();
        break;
      }
      snapshot = std::move(*delivery->snapshot);
      delivery->snapshot.reset();
      session = delivery->session;
      details->SetText(snapshot.summary.c_str());
      auto *selection = static_cast<AttributeRow *>(attributes->CurrentSelection());
      const std::string selectedName = selection ? selection->value.name : "";
      Listi::ClearColumnRows(*attributes);
      for (const auto &a : snapshot.attributes) {
        auto *row = new AttributeRow(a);
        attributes->AddRow(row);
        if (a.name == selectedName)
          attributes->SetFocusRow(row, true);
      }
      const bool keepPermissions = message->GetBool("keepPermissionDraft", false);
      Reset(keepPermissions);
      Status(
          keepPermissions ? "Attribute saved. Apply or Discard the permission draft."
          : !delivery->completion.empty() ? delivery->completion
          : snapshot.limited
              ? "Attribute limit reached. Some entries are not shown."
              : "Properties loaded. Changes require explicit Save or Apply.");
      break;
    }
    case kRefresh:
      if (!busy && MayReplaceDraft())
        Work();
      break;
    case kDiscard:
      if (!busy) {
        Reset();
        Status("Draft discarded. Stored metadata is unchanged.");
      }
      break;
    case kNew:
      if (!busy && MayReplaceDraft()) {
        Reset();
        creating = true;
        heading->SetText(
            "Creating a new attribute. Existing names will not be replaced.");
        Controls();
        name->MakeFocus();
      }
      break;
    case kShowAttributeColumn:
    case kMakeSearchable: {
      if (busy || (message->what == kMakeSearchable && !MayReplaceDraft()))
        break;
      auto row = static_cast<AttributeRow *>(attributes->CurrentSelection());
      if (!row) {
        Status("Select an attribute first.");
        break;
      }
      auto attr = row->value;
      if (message->what == kShowAttributeColumn) {
        BMessage add(kShowAttributeColumn), reply;
        add.AddString("attribute", attr.name.c_str());
        if (pane.SendMessage(&add, &reply, 500000, 1000000) != B_OK)
          Status("The original pane is unavailable.");
        else
          Status(reply.GetString("status", "Column added to Details."));
        break;
      }
      if (!IndexableAttribute(attr.type)) {
        Status("This attribute type cannot be indexed by BFS.");
        break;
      }
      char volume[B_FILE_NAME_LENGTH] = {};
      if (BVolume(snapshot.stat.st_dev).GetName(volume) != B_OK) {
        Status("Cannot identify the affected volume.");
        break;
      }
      std::string warning = "Create or check the " +
                            AttributeTypeName(attr.type) + " index for “" +
                            attr.name + "” on the entire volume “" + volume +
                            "”? Existing files will not be rewritten.";
      auto alert = new BAlert("Volume index", warning.c_str(), "Cancel",
                              "Make searchable");
      alert->SetDefaultButton(alert->ButtonAt(0));
      alert->SetShortcut(0, B_ESCAPE);
      if (alert->Go() != 1)
        break;
      auto holder = delivery;
      Work([holder, attr](PropertySession &s) {
        auto result = s.MakeSearchable(attr);
        std::lock_guard<std::mutex> lock(holder->mutex);
        holder->completion = result;
      });
      break;
    }
    case kEdit: {
      if (busy || !MayReplaceDraft())
        break;
      auto row = static_cast<AttributeRow *>(attributes->CurrentSelection());
      if (!row) {
        Status("Select an attribute first.");
        break;
      }
      editing = std::make_unique<FileAttribute>(row->value);
      creating = false;
      confirmRemove = false;
      originalName = editing->name;
      originalValue = editing->text;
      originalType = editing->type;
      name->SetText(originalName.c_str());
      value->SetText(originalValue.c_str());
      for (int32 i = 0; i < types->CountItems(); ++i)
        types->ItemAt(i)->SetMarked(
            uint32(types->ItemAt(i)->Message()->GetInt32("type", 0)) ==
            originalType);
      if (types->Superitem())
        types->Superitem()->SetLabel(AttributeTypeName(originalType).c_str());
      heading->SetText(
          (std::string(editing->editable ? "Editing: " : "Read-only: ") +
           editing->name)
              .c_str());
      heading->SetToolTip(heading->Text());
      Controls();
      Status(
          editing->editable
              ? "Save updates this attribute only. Switching file selection does not retarget it."
              : "System, binary, oversized or malformed values are inspection-only.");
      break;
    }
    case kSave: {
      if (busy || (!creating && (!editing || !editing->editable)))
        break;
      auto expected =
          editing ? std::make_shared<FileAttribute>(*editing) : nullptr;
      std::string key = name->Text(), text = value->Text();
      type_code type = Type();
      Work([expected, key, text, type](auto &s) {
        s.Save(expected.get(), key, type, text);
      }, PermissionDirty());
      break;
    }
    case kRemove: {
      if (busy || !editing || !editing->editable)
        break;
      if (Dirty() || PermissionDirty()) {
        Status("Save or Discard the draft before removing an attribute.");
        break;
      }
      if (!confirmRemove) {
        confirmRemove = true;
        Controls();
        Status(
            "Remove this attribute, not the file? Click Confirm remove, or Discard to cancel.");
        break;
      }
      auto expected = *editing;
      Work([expected](auto &s) { s.Remove(expected); });
      break;
    }
    case kPermissions: {
      if (busy || !session)
        break;
      if (Dirty()) {
        Status("Save or Discard the attribute draft first.");
        break;
      }
      mode_t expected = permissionBase;
      std::string text = permissions->Text();
      Work([expected, text](auto &s) { s.Permissions(expected, text); });
      break;
    }
    case B_COLORS_UPDATED:
      BWindow::MessageReceived(message);
      Colors();
      break;
    default:
      BWindow::MessageReceived(message);
    }
  }
};
}
void OpenFileProperties(const std::string &path, BMessenger pane) {
  new PropertiesWindow(path, pane);
}

bool FilePropertiesAllowQuit() {
  std::vector<BMessenger> windows;
  {
    std::lock_guard<std::mutex> lock(windowsMutex);
    windows = propertyWindows;
  }
  for (auto &window : windows) {
    BMessage question(kCanQuit), reply;
    if (window.SendMessage(&question, &reply, 500000, 500000) != B_OK) {
      if (window.IsValid())
        return false;
    } else if (!reply.GetBool("allow", false))
      return false;
  }
  return true;
}
