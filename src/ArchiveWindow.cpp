// SPDX-License-Identifier: MIT
#include "ArchiveWindow.h"
#include "LeafName.h"
#include "ArchiveBackend.h"
#include "PaneModel.h"
#include "ArchivePasswords.h"
#include "JobSnapshot.h"
#include "ArchiveSettings.h"
#include "VirtualFile.h"
#include <Button.h>
#include <CheckBox.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <strings.h>
#include <thread>
namespace {
constexpr uint32 kReady = 'arld', kGo = 'argo', kBrowse = 'arbr',
                 kFolder = 'arfo', kFormat = 'arfm', kOptions = 'arop',
                 kPreferences = 'arpf', kShow = 'arsh', kFavorite = 'arfv', kProtect = 'aren';
struct Loaded {
  std::mutex mutex;
  std::vector<Listi::ArchiveFormat> formats;
  BMessage settings;
  std::string error;
};
std::string SettingsPath() {
  BPath p;
  find_directory(B_USER_SETTINGS_DIRECTORY, &p);
  p.Append("Lister_archive_settings");
  return p.Path();
}
bool HasSuffix(const std::string &name, const std::string &suffix) {
  return !suffix.empty() && name.size() >= suffix.size() &&
         strcasecmp(name.c_str() + name.size() - suffix.size(),
                    suffix.c_str()) == 0;
}

class ArchiveWindow : public BWindow {
  Listi::Job job;
  BMessenger target;
  ArchiveWindowOptions launchOptions;
  BTextControl *folder, *name, *password = nullptr, *confirm = nullptr;
  BStringView *status, *formatSummary;
  BMenuField *format, *level;
  BCheckBox *ask, *favorite, *useDefault, *protect = nullptr,
                                          *encryptNames = nullptr;
  BButton *start, *change;
  BGroupView *options, *preferences;
  std::unique_ptr<BFilePanel> picker;
  std::shared_ptr<Loaded> loaded = std::make_shared<Loaded>();
  std::vector<Listi::ArchiveFormat> formats;
  BMessage settings;
  std::string formatID = "zip", extension;
  std::string suggestedStem, suggestedName;
  bool expanded = true, ready = false;
  bool Extract() const { return job.action == Listi::Action::ExtractArchive; }
  const Listi::ArchiveFormat *SelectedFormat() const {
    auto found = std::find_if(formats.begin(), formats.end(),
                              [&](const auto &f) { return f.id == formatID; });
    return found == formats.end() ? nullptr : &*found;
  }
  void Status(const std::string &s) {
    status->SetText(s.c_str());
    status->SetToolTip(s.c_str());
  }
  bool Favorite(const std::string &id) const {
    return settings.GetBool(("favorite:" + id).c_str(),
                            id == "zip" || id == "tgz");
  }
  void BuildFormats() {
    auto menu = format->Menu();
    while (auto item = menu->RemoveItem(int32(0)))
      delete item;
    auto more = new BMenu("More formats");
    for (const auto &f : formats) {
      auto message = new BMessage(kFormat);
      message->AddString("format", f.id.c_str());
      auto item = new BMenuItem(f.label.c_str(), message);
      item->SetTarget(this);
      item->SetMarked(f.id == formatID);
      (Favorite(f.id) ? menu : more)->AddItem(item);
    }
    if (more->CountItems())
      menu->AddItem(more);
    else
      delete more;
    for (const auto &f : formats)
      if (f.id == formatID && menu->Superitem())
        menu->Superitem()->SetLabel(f.label.c_str());
  }
  void SelectFormat(const std::string &id) {
    const auto found = std::find_if(formats.begin(), formats.end(),
                                    [&](const auto &f) { return f.id == id; });
    if (found == formats.end())
      return;
    const bool enableEncryptedNames = protect &&
                                      protect->Value() == B_CONTROL_ON &&
                                      id == "7z" && formatID != "7z";
    formatID = id;
    std::string text = name->Text();
    if (text == suggestedName) {
      suggestedName = Listi::SuffixedLeafName(suggestedStem, found->extension);
      text = suggestedName;
    } else {
      if (HasSuffix(text, extension))
        text.resize(text.size() - extension.size());
      if (!HasSuffix(text, found->extension))
        text += found->extension;
    }
    extension = found->extension;
    name->SetText(text.c_str());
    favorite->SetValue(Favorite(id));
    const int saved =
        std::max(0, std::min(9, settings.GetInt32(("level:" + id).c_str(), 6)));
    for (int i = 0; i < level->Menu()->CountItems(); ++i)
      level->Menu()->ItemAt(i)->SetMarked(i == saved);
    level->SetEnabled(found->compression);
    BuildFormats();
    UpdateSecurity();
    if (enableEncryptedNames && encryptNames->IsEnabled())
      encryptNames->SetValue(B_CONTROL_ON);
  }
  void UpdateSecurity() {
    if (Extract() || !protect)
      return;
    const auto selected = SelectedFormat();
    const bool supported = selected && selected->encryption;
    protect->SetEnabled(supported);
    if (!supported)
      protect->SetValue(B_CONTROL_OFF);
    const bool enabled = supported && protect->Value() == B_CONTROL_ON;
    if (selected)
      formatSummary->SetText(("Format: " + selected->label +
                              (enabled ? " · encrypted" : "")).c_str());
    password->SetEnabled(enabled);
    confirm->SetEnabled(enabled);
    const bool names = enabled && selected->encryptedNames;
    encryptNames->SetEnabled(names);
    if (!names)
      encryptNames->SetValue(B_CONTROL_OFF);
  }
  void ClearSecretInputs() {
    if (password)
      password->SetText("");
    if (confirm)
      confirm->SetText("");
  }
  void ShowOptions(bool show) {
    if (expanded != show) {
      expanded = show;
      if (show)
        options->Show();
      else
        options->Hide();
    }
    change->SetLabel(show ? "Hide format options" : "Change format...");
  }

public:
  ArchiveWindow(const Listi::Job &frozen, const std::string &initial,
                BMessenger owner, ArchiveWindowOptions launch)
      : BWindow(
            BRect(160, 140, 790,
                  frozen.action == Listi::Action::ExtractArchive ? 410 : 600),
            frozen.action == Listi::Action::ExtractArchive
                ? "Listi: Extract archive"
                : "Listi: Create archive",
            B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS),
        job(frozen), target(owner), launchOptions(std::move(launch)) {
    const auto first = Listi::LocationLeaf(job.sources[0]);
    folder = new BTextControl("archive folder",
                              "Destination folder:", initial.c_str(), nullptr);
    auto proposed = launchOptions.suggestedName.empty()
                        ? (job.sources.size() == 1 ? first : "Archive")
                        : launchOptions.suggestedName;
    if (Extract()) {
      auto loc = Listi::ParseLocation(job.sources[0]);
      proposed = Listi::Leaf(loc.Archive() ? loc.archive : job.sources[0]);
      if (Listi::ArchiveExtension(proposed))
        proposed.resize(proposed.size() - FileTypeKey(proposed).size());
      proposed = Listi::SuffixedLeafName(proposed, " extracted");
    } else {
      suggestedStem = suggestedName = proposed;
    }
    name = new BTextControl("archive name",
                            Extract() ? "New folder name:" : "Archive name:",
                            proposed.c_str(), nullptr);
    status = new BStringView(
        "archive status",
        Extract() ? "Ready." : "Checking available archive formats...");
    status->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
    status->SetTruncation(B_TRUNCATE_END);
    auto selected = new BStringView(
        "archive sources",
        (std::to_string(job.sources.size()) + " selected: " + first).c_str());
    selected->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
    selected->SetTruncation(B_TRUNCATE_MIDDLE);
    start = new BButton(Extract() ? "Extract" : "Create archive",
                        new BMessage(kGo));
    start->SetEnabled(Extract());
    auto root = new BGroupView(B_VERTICAL, 10);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0).Add(root);
    auto layout = BLayoutBuilder::Group<>(root);
    layout.SetInsets(12)
        .Add(selected)
        .AddGroup(B_HORIZONTAL, 8)
        .Add(folder)
        .Add(new BButton("Choose...", new BMessage(kBrowse)))
        .End()
        .Add(name);
    if (!Extract()) {
      format = new BMenuField(
          "archive format", "Format:", new BPopUpMenu("Formats", false, false));
      auto levels = new BPopUpMenu("Compression");
      for (int i = 0; i <= 9; ++i) {
        auto msg = new BMessage(kOptions);
        auto item = new BMenuItem((std::to_string(i) + (i == 0   ? " (fastest)"
                                                        : i == 6 ? " (normal)"
                                                        : i == 9 ? " (smallest)"
                                                                 : ""))
                                      .c_str(),
                                  msg);
        levels->AddItem(item);
      }
      level = new BMenuField("archive compression", "Compression:", levels);
      favorite = new BCheckBox("Favorite format", new BMessage(kFavorite));
      useDefault = new BCheckBox("Use this format by default", nullptr);
      ask = new BCheckBox("Ask for a format every time", nullptr);
      protect =
          new BCheckBox("Password protect (AES-256)", new BMessage(kProtect));
      encryptNames = new BCheckBox("Encrypt file names (7z)", nullptr);
      password =
          new BTextControl("archive create password", "Password:", "", nullptr);
      confirm = new BTextControl("archive confirm password",
                                 "Confirm password:", "", nullptr);
      password->TextView()->HideTyping(true);
      confirm->TextView()->HideTyping(true);
      password->TextView()->SetMaxBytes(1024);
      confirm->TextView()->SetMaxBytes(1024);
      password->SetEnabled(false);
      confirm->SetEnabled(false);
      encryptNames->SetEnabled(false);
      formatSummary =
          new BStringView("archive chosen format", "Format: checking...");
      change = new BButton("Hide format options", new BMessage(kShow));
      change->SetEnabled(false);
      options = new BGroupView(B_VERTICAL, 8);
      preferences = new BGroupView(B_VERTICAL, 4);
      BLayoutBuilder::Group<>(preferences).Add(favorite).Add(useDefault).Add(ask);
      preferences->Hide();
      BLayoutBuilder::Group<>(options)
          .Add(format)
          .Add(level)
          .Add(protect)
          .Add(encryptNames)
          .Add(password)
          .Add(confirm)
          .Add(new BStringView(
              "archive password lifetime",
              "Passwords are kept only in memory and are cleared after this window."))
          .Add(new BButton("Format preferences…", new BMessage(kPreferences)))
          .Add(preferences);
      layout.AddGroup(B_HORIZONTAL, 8)
          .Add(formatSummary)
          .AddGlue()
          .Add(change)
          .End()
          .Add(options);
    }
    layout
        .Add(new BStringView(
            "archive safety",
            Extract() ? "Use a new folder name that no other job is creating."
                      : "Use a new name. Existing files are not replaced."))
        .Add(status)
        .AddGlue()
        .AddGroup(B_HORIZONTAL, 8)
        .AddGlue()
        .Add(new BButton("Cancel", new BMessage(B_QUIT_REQUESTED)))
        .Add(start)
        .End();
    AddShortcut(B_ESCAPE, B_NO_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
    GetLayout()->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    name->MakeFocus(true);
    Show();
    if (!Extract()) {
      auto data = loaded;
      BMessenger reply(this);
      std::thread([data, reply] {
        {
          std::lock_guard<std::mutex> lock(data->mutex);
          try {
            data->settings = Listi::LoadArchiveSettings(SettingsPath());
            data->formats = Listi::WritableArchiveFormats();
          } catch (const std::exception &e) {
            data->error = e.what();
          }
        }
        reply.SendMessage(kReady);
      }).detach();
    } else
      ready = true;
  }
  ~ArchiveWindow() {
    ClearSecretInputs();
    if (job.archivePasswordToken)
      Listi::DiscardArchiveCreationPassword(job.archivePasswordToken);
  }
  void MessageReceived(BMessage *m) override {
    try {
      switch (m->what) {
      case 'arav':
        Activate(true);
        break;
      case kReady: {
        std::lock_guard<std::mutex> lock(loaded->mutex);
        if (!loaded->error.empty()) {
          Status(loaded->error);
          break;
        }
        settings = loaded->settings;
        formats = loaded->formats;
        if (formats.empty()) {
          Status(
              "No writable formats. Install libarchive from HaikuDepot, then reopen this window.");
          break;
        }
        std::string def = settings.GetString("default", "zip");
        if (std::none_of(formats.begin(), formats.end(),
                         [&](auto &f) { return f.id == def; }))
          def = formats.front().id;
        ask->SetValue(settings.GetBool("ask", false));
        SelectFormat(def);
        change->SetEnabled(true);
        ShowOptions(ask->Value());
        ready = true;
        start->SetEnabled(true);
        Status("Ready.");
        break;
      }
      case kFormat: {
        auto marked = level->Menu()->FindMarked();
        if (marked)
          settings.SetInt32(("level:" + formatID).c_str(),
                            level->Menu()->IndexOf(marked));
        SelectFormat(m->GetString("format", ""));
        break;
      }
      case kPreferences:
        if (preferences->IsHidden(preferences))
          preferences->Show();
        else
          preferences->Hide();
        break;
      case kOptions:
        break;
      case kFavorite:
        settings.SetBool(("favorite:" + formatID).c_str(), favorite->Value());
        BuildFormats();
        break;
      case kProtect: {
        const bool turnOn = protect->Value() == B_CONTROL_ON;
        UpdateSecurity();
        if (turnOn && formatID == "7z" && encryptNames->IsEnabled())
          encryptNames->SetValue(B_CONTROL_ON);
        if (turnOn)
          password->MakeFocus(true);
        else
          ClearSecretInputs();
        break;
      }
      case kShow:
        ShowOptions(!expanded);
        break;
      case kBrowse: {
        if (!picker) {
          BMessenger here(this);
          picker.reset(new BFilePanel(B_OPEN_PANEL, &here, nullptr,
                                      B_DIRECTORY_NODE, false,
                                      new BMessage(kFolder)));
        }
        picker->SetPanelDirectory(folder->Text());
        picker->Show();
        break;
      }
      case kFolder: {
        entry_ref ref;
        BPath path;
        if (m->FindRef("refs", &ref) == B_OK && path.SetTo(&ref) == B_OK)
          folder->SetText(path.Path());
        break;
      }
      case kGo: {
        if (!ready)
          break;
        if (launchOptions.validate)
          launchOptions.validate();
        Listi::ArchiveSecretPtr secret;
        if (!Extract() && protect->Value() == B_CONTROL_ON) {
          if (strcmp(password->Text(), confirm->Text()))
            throw std::runtime_error("Passwords do not match.");
          secret = std::make_shared<Listi::ArchiveSecret>(password->Text());
        }
        std::string outputName = name->Text();
        if (!Extract() && launchOptions.appendExtension &&
            !outputName.empty() && !HasSuffix(outputName, extension))
          outputName += extension;
        Listi::SetArchiveDestination(job, folder->Text(), outputName);
        if (!Extract()) {
          job.archiveFormat = formatID;
          auto marked = level->Menu()->FindMarked();
          job.archiveLevel = marked ? level->Menu()->IndexOf(marked) : 6;
          job.archiveEncryptNames =
              secret && encryptNames->Value() == B_CONTROL_ON;
          settings.SetInt32(("level:" + formatID).c_str(), job.archiveLevel);
          settings.SetBool("ask", ask->Value());
          if (useDefault->Value())
            settings.SetString("default", formatID.c_str());
          Listi::SaveArchiveSettings(SettingsPath(), settings);
          job.archivePasswordToken = Listi::HoldArchiveCreationPassword(secret);
          ClearSecretInputs();
        }
        auto result = Listi::EncodeJob(job, kArchiveAccepted);
        if (target.SendMessage(&result) != B_OK) {
          Listi::DiscardArchiveCreationPassword(job.archivePasswordToken);
          job.archivePasswordToken = 0;
          Status(
              "The main window is no longer available. Nothing was queued. Re-enter the password to try again.");
          break;
        }
        job.archivePasswordToken = 0;
        ready = false;
        start->SetEnabled(false);
        PostMessage(B_QUIT_REQUESTED);
        break;
      }
      default:
        BWindow::MessageReceived(m);
      }
    } catch (const std::exception &e) {
      Status(e.what());
    }
  }
};
}
BMessenger OpenArchiveWindow(const Listi::Job &job, const std::string &folder,
                             BMessenger target, ArchiveWindowOptions options) {
  return BMessenger(new ArchiveWindow(job, folder, target, std::move(options)));
}
