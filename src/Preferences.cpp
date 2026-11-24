// SPDX-License-Identifier: MIT
#include "Common.h"
#include "WrappedLabel.h"
#include "WorkspaceEditor.h"
#include <Alert.h>
#include <Button.h>
#include <CardLayout.h>
#include <CardView.h>
#include <CheckBox.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
rgb_color Preferences::Color(const BString &text) const {
  unsigned int r, g, b;
  if (text.Length() == 7 &&
      sscanf(text.String(), "#%2x%2x%2x", &r, &g, &b) == 3)
    return {(uint8)r, (uint8)g, (uint8)b, 255};
  return ui_color(B_PANEL_TEXT_COLOR);
}
void Preferences::Load(const BMessage &m) {
  WorkspaceLayout::Load(m);
  tviskiptButtons = m.GetInt32("tviskiptButtons", -1);
  if (tviskiptButtons < -1 || tviskiptButtons > 1)
    tviskiptButtons = -1;
  smidjaButtons = m.GetInt32("smidjaButtons", -1);
  if (smidjaButtons < -1 || smidjaButtons > 1)
    smidjaButtons = -1;
  profiles.Load(m);
  folderModes.clear();
  BMessage folder;
  for (int32 i = 0; i < 4096 && m.FindMessage("folderView", i, &folder) == B_OK;
       ++i) {
    const char *path = folder.GetString("path", "");
    int32 mode = folder.GetInt32("mode", -1);
    if (*path && mode >= 0 && mode < ViewModeCount)
      folderModes[path] = mode;
  }
  alwaysFolderTabs = m.GetBool("alwaysFolderTabs", false);
  separateWindows = m.GetBool("separateWindows", true);
  icons = m.GetBool("icons", true);
  size = m.GetBool("size", true);
  date = m.GetBool("date", true);
  float fontSize = m.GetFloat("fontSize", be_plain_font->Size());
  font.SetSize(std::isfinite(fontSize) && fontSize >= 8 && fontSize <= 40
                   ? fontSize
                   : 12);
  const char *family = m.GetString("fontFamily", "");
  if (*family)
    font.SetFamilyAndStyle(family, nullptr);
  editor = m.GetString("editor", editor);
  terminal = m.GetString("terminal", terminal);
  folders = m.GetString("folders", "system");
  files = m.GetString("files", "system");
  links = m.GetString("links", "system");
}
void Preferences::ApplyDialog(const BMessage &message) {
  // The dialog edits appearance, launchers and workspace defaults. Keep
  // layout changes made from the main window while the dialog was open.
  Preferences updated = *this;
  updated.Load(message);
  auto layout = CaptureLayout();
  BMessage changes;
  const bool hasChanges = message.FindMessage("layoutChanges", &changes) == B_OK;
  updated.alwaysFolderTabs = alwaysFolderTabs;
  updated.tviskiptButtons = tviskiptButtons;
  updated.smidjaButtons = smidjaButtons;
  for (auto w : {Workspace::tviskipt, Workspace::einn, Workspace::askr,
                 Workspace::smidja}) {
    if (!hasChanges || changes.GetBool(WorkspaceId(w), false))
      layout.SetDefaultMode(w, updated.DefaultMode(w));
  }
  if ((!hasChanges && layout.buttons != updated.buttons) ||
      (hasChanges && changes.GetBool("buttons", false))) {
    layout.buttons = updated.buttons;
    updated.smidjaButtons = updated.tviskiptButtons = -1;
  }
  if (!hasChanges || changes.GetBool("command", false))
    layout.command = updated.command;
  updated.ApplyLayout(layout);
  updated.profiles = profiles;
  updated.folderModes = folderModes;
  BMessage delta;
  if (message.FindMessage("sharedChanges", &delta) == B_OK) {
    updated.ApplyShared(SharedChanges());
    updated.ApplyShared(delta);
  }
  *this = updated;
}
BMessage Preferences::Save() const {
  BMessage m = SharedChanges();
  m.what = 0;
  WorkspaceLayout::Archive(m);
  m.AddInt32("tviskiptButtons", tviskiptButtons);
  m.AddInt32("smidjaButtons", smidjaButtons);
  profiles.Archive(m);
  for (auto &entry : folderModes) {
    BMessage folder;
    folder.AddString("path", entry.first.c_str());
    folder.AddInt32("mode", entry.second);
    m.AddMessage("folderView", &folder);
  }
  return m;
}
BMessage Preferences::SharedChanges(const Preferences *before) const {
  BMessage m(kSharedPreferences);
  if (!before || separateWindows != before->separateWindows)
    m.AddBool("separateWindows", separateWindows);
  if (!before || alwaysFolderTabs != before->alwaysFolderTabs)
    m.AddBool("alwaysFolderTabs", alwaysFolderTabs);
  if (!before || icons != before->icons)
    m.AddBool("icons", icons);
  if (!before || size != before->size)
    m.AddBool("size", size);
  if (!before || date != before->date)
    m.AddBool("date", date);
  if (!before || editor != before->editor)
    m.AddString("editor", editor);
  if (!before || terminal != before->terminal)
    m.AddString("terminal", terminal);
  if (!before || folders != before->folders)
    m.AddString("folders", folders);
  if (!before || files != before->files)
    m.AddString("files", files);
  if (!before || links != before->links)
    m.AddString("links", links);
  font_family family, previousFamily = {};
  font_style style;
  font.GetFamilyAndStyle(&family, &style);
  if (before)
    before->font.GetFamilyAndStyle(&previousFamily, &style);
  if (!before || strcmp(family, previousFamily) != 0)
    m.AddString("fontFamily", family);
  if (!before || font.Size() != before->font.Size())
    m.AddFloat("fontSize", font.Size());
  return m;
}
void Preferences::ApplyShared(const BMessage &m) {
  separateWindows = m.GetBool("separateWindows", separateWindows);
  alwaysFolderTabs = m.GetBool("alwaysFolderTabs", alwaysFolderTabs);
  icons = m.GetBool("icons", icons);
  size = m.GetBool("size", size);
  date = m.GetBool("date", date);
  editor = m.GetString("editor", editor);
  terminal = m.GetString("terminal", terminal);
  folders = m.GetString("folders", folders);
  files = m.GetString("files", files);
  links = m.GetString("links", links);
  const char *family = nullptr;
  if (m.FindString("fontFamily", &family) == B_OK)
    font.SetFamilyAndStyle(family, nullptr);
  float points;
  if (m.FindFloat("fontSize", &points) == B_OK && std::isfinite(points) &&
      points >= 8 && points <= 40)
    font.SetSize(points);
}
namespace {
constexpr Workspace kSettingsWorkspaces[] = {
    Workspace::einn, Workspace::tviskipt, Workspace::askr, Workspace::smidja};
constexpr uint32 kSettingsCategory = 'stct';
constexpr int32 kAppearancePage = 1;

// Clip to a fixed viewport, but give the page enough height for every
// control. The scroll bar moves the viewport rather than shrinking the page.
class SettingsPage : public BScrollView {
  BView *content;

public:
  explicit SettingsPage(BView *page)
      : BScrollView("settings page scroll",
                    new BView("settings viewport", B_SCROLL_VIEW_AWARE),
                    0, false, true, B_NO_BORDER),
        content(page) {
    Target()->AddChild(content);
  }

  BSize MinSize() override {
    const auto bar = ScrollBar(B_VERTICAL);
    return {content->MinSize().width + bar->PreferredSize().width + 1,
            std::max(64.f, bar->MinSize().height)};
  }

  void DoLayout() override {
    BScrollView::DoLayout();
    const BRect bounds = Target()->Bounds();
    if (!bounds.IsValid())
      return;
    float minimum = content->MinSize().height;
    if (content->HasHeightForWidth())
      content->GetHeightForWidth(bounds.Width(), &minimum, nullptr, nullptr);
    const float height = std::max(bounds.Height(), minimum);
    content->ResizeTo(bounds.Width(), height);

    auto bar = ScrollBar(B_VERTICAL);
    bar->SetRange(0, height - bounds.Height());
    bar->SetProportion((bounds.Height() + 1) / (height + 1));
    bar->SetSteps(be_plain_font->Size() * 1.33f, bounds.Height());
  }

  void Reveal(BView *view) {
    BView *ancestor = view;
    while (ancestor && ancestor != content)
      ancestor = ancestor->Parent();
    if (!ancestor)
      return;
    const BRect bounds = Target()->Bounds();
    const BRect field =
        Target()->ConvertFromScreen(view->ConvertToScreen(view->Bounds()));
    const float top = bounds.top;
    if (field.top < top)
      ScrollBar(B_VERTICAL)->SetValue(field.top);
    else if (field.bottom > bounds.bottom)
      ScrollBar(B_VERTICAL)->SetValue(top + field.bottom - bounds.bottom);
  }
};

class PreferencesWindow : public BWindow {
  BMessenger target;
  Preferences prefs;
  BCardView *pages;
  BPopUpMenu *categories;
  BPopUpMenu *defaultModes[sizeof(kSettingsWorkspaces) /
                           sizeof(kSettingsWorkspaces[0])];
  BCheckBox *icons, *size, *date, *buttons, *command, *separateWindows;
  BTextControl *family, *fontSize, *folders, *files, *links, *editor, *terminal;

  void AddPage(BView *page) {
    auto choose = new BMessage(kSettingsCategory);
    choose->AddInt32("page", categories->CountItems());
    categories->AddItem(new BMenuItem(page->Name(), choose));
    pages->AddChild(new SettingsPage(page));
  }

  void SelectPage(int32 index) {
    auto item = categories->ItemAt(index);
    if (!item)
      return;
    item->SetMarked(true);
    pages->CardLayout()->SetVisibleItem(index);
  }

  void Reveal(BView *view) {
    auto item = pages->CardLayout()->VisibleItem();
    if (item)
      static_cast<SettingsPage *>(item->View())->Reveal(view);
  }

  void ShowInputError(BTextControl *control, const char *error) {
    SelectPage(kAppearancePage);
    Layout(false);
    control->MakeFocus();
    Reveal(control);
    (new BAlert("invalid setting", error, "OK"))->Go(nullptr);
  }

public:
  PreferencesWindow(BMessenger t, const Preferences &p)
      : BWindow(BRect(0, 0, 560, 300), "Listi settings", B_TITLED_WINDOW,
                B_AUTO_UPDATE_SIZE_LIMITS | B_NOT_ZOOMABLE),
        target(t), prefs(p) {
    pages = new BCardView("settings pages");
    categories = new BPopUpMenu("Category");
    auto categoryField =
        new BMenuField("settings category", "Category:", categories);

    auto workspaces = new BGroupView("Workspaces", B_VERTICAL);
    auto workspaceLayout = BLayoutBuilder::Group<>(workspaces);
    auto views = workspaceLayout.SetInsets(B_USE_WINDOW_SPACING)
                     .Add(new BStringView("default views",
                                          "Default file view by workspace"))
                     .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
    int32 index = 0;
    for (Workspace workspace : kSettingsWorkspaces) {
      auto menu = new BPopUpMenu("File views");
      for (int32 mode = 0; mode < ViewModeCount; ++mode)
        menu->AddItem(new BMenuItem(ViewModeLabel(mode), nullptr));
      menu->ItemAt(p.DefaultMode(workspace))->SetMarked(true);
      defaultModes[index] = menu;
      auto field = new BMenuField(WorkspaceId(workspace),
                                  WorkspaceMenuLabel(workspace), menu);
      views.Add(field->CreateLabelLayoutItem(), 0, index)
          .Add(field->CreateMenuBarLayoutItem(), 1, index);
      ++index;
    }
    auto openLayout = new BMessage(kOpenWorkspaceEditor);
    openLayout->AddBool("modules", true);
    auto customize = new BButton("workspace layout",
                                 "Customize current workspace layout…", openLayout);
    customize->SetTarget(target);
    separateWindows = new BCheckBox("Open a new window when launching Listi again", nullptr);
    separateWindows->SetValue(p.separateWindows);
    views.End()
        .Add(separateWindows)
        .Add(new WrappedLabel(
            "view precedence",
            "An explicit view saved for a folder takes priority."), 0)
        .Add(customize)
        .AddGlue();
    AddPage(workspaces);

    icons = new BCheckBox("Show file icons", nullptr);
    icons->SetValue(p.icons);
    size = new BCheckBox("Size column", nullptr);
    size->SetValue(p.size);
    date = new BCheckBox("Modified column", nullptr);
    date->SetValue(p.date);
    buttons = new BCheckBox("Command button bank", nullptr);
    buttons->SetValue(p.buttons);
    command = new BCheckBox("Command field (runs in external terminal)", nullptr);
    command->SetValue(p.command);
    font_family f;
    font_style s;
    p.font.GetFamilyAndStyle(&f, &s);
    char number[32];
    snprintf(number, sizeof(number), "%.6g", p.font.Size());
    family = new BTextControl("File list font:", f, nullptr);
    fontSize = new BTextControl("Font size:", number, nullptr);
    folders = new BTextControl("Folder color:", p.folders, nullptr);
    files = new BTextControl("File color:", p.files, nullptr);
    links = new BTextControl("settings link color", "Symbolic link color:",
                             p.links, nullptr);
    editor = new BTextControl("settings editor", "Editor application:",
                              p.editor, nullptr);
    terminal = new BTextControl("settings terminal", "Terminal application:",
                                p.terminal, nullptr);

    auto appearance = new BGroupView("Appearance", B_VERTICAL);
    BLayoutBuilder::Group<>(appearance)
        .SetInsets(B_USE_WINDOW_SPACING)
        .Add(icons)
        .Add(size)
        .Add(date)
        .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
        .Add(family->CreateLabelLayoutItem(), 0, 0)
        .Add(family->CreateTextViewLayoutItem(), 1, 0)
        .Add(fontSize->CreateLabelLayoutItem(), 0, 1)
        .Add(fontSize->CreateTextViewLayoutItem(), 1, 1)
        .Add(folders->CreateLabelLayoutItem(), 0, 2)
        .Add(folders->CreateTextViewLayoutItem(), 1, 2)
        .Add(files->CreateLabelLayoutItem(), 0, 3)
        .Add(files->CreateTextViewLayoutItem(), 1, 3)
        .Add(links->CreateLabelLayoutItem(), 0, 4)
        .Add(links->CreateTextViewLayoutItem(), 1, 4)
        .End()
        .Add(new WrappedLabel("color format",
                              "Use system or a color like #B8C4D0."), 0)
        .AddGlue();
    AddPage(appearance);

    auto workflow = new BGroupView("Workflow", B_VERTICAL);
    BLayoutBuilder::Group<>(workflow)
        .SetInsets(B_USE_WINDOW_SPACING)
        .Add(buttons)
        .Add(command)
        .AddGlue();
    AddPage(workflow);

    auto applications = new BGroupView("Applications", B_VERTICAL);
    BLayoutBuilder::Group<>(applications)
        .SetInsets(B_USE_WINDOW_SPACING)
        .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
        .Add(editor->CreateLabelLayoutItem(), 0, 0)
        .Add(editor->CreateTextViewLayoutItem(), 1, 0)
        .Add(terminal->CreateLabelLayoutItem(), 0, 1)
        .Add(terminal->CreateTextViewLayoutItem(), 1, 1)
        .End()
        .AddGlue();
    AddPage(applications);

    auto save = new BButton("settings save", "Save",
                            new BMessage(kApplyPreferences));
    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_SPACING)
        .Add(categoryField, 0)
        .Add(pages)
        .AddGroup(B_HORIZONTAL)
        .AddGlue()
        .Add(new BButton("settings cancel", "Cancel",
                         new BMessage(B_QUIT_REQUESTED)))
        .Add(save)
        .End();
    categories->SetTargetForItems(this);
    SelectPage(0);
    SetDefaultButton(save);
    AddShortcut(B_ESCAPE, B_NO_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
    AddShortcut('W', B_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
    CenterOnScreen();
  }
  void DispatchMessage(BMessage *message, BHandler *handler) override {
    const bool tab = message && message->what == B_KEY_DOWN &&
                     message->GetString("bytes", "")[0] == B_TAB;
    BView *previous = tab ? CurrentFocus() : nullptr;
    BWindow::DispatchMessage(message, handler);
    if (tab && CurrentFocus() != previous)
      Reveal(CurrentFocus());
  }
  void MessageReceived(BMessage *m) override {
    if (m->what == kSettingsCategory) {
      SelectPage(m->GetInt32("page", -1));
      return;
    }
    if (m->what != kApplyPreferences) {
      BWindow::MessageReceived(m);
      return;
    }
    char *end = nullptr;
    float n = strtof(fontSize->Text(), &end);
    if (*end || !std::isfinite(n) || n < 8 || n > 40) {
      ShowInputError(fontSize, "Font size must be 8 to 40.");
      return;
    }
    for (auto c : {folders, files, links}) {
      BString s = c->Text();
      bool valid = s == "system";
      if (s.Length() == 7 && s.ByteAt(0) == '#') {
        valid = true;
        for (int i = 1; i < 7; i++)
          if (!std::isxdigit(static_cast<unsigned char>(s.ByteAt(i))))
            valid = false;
      }
      if (!valid) {
        ShowInputError(c, "Use system or a six-digit color such as #B8C4D0.");
        return;
      }
    }
    const Preferences before = prefs;
    prefs.separateWindows = separateWindows->Value();
    prefs.font.SetFamilyAndStyle(family->Text(), nullptr);
    prefs.font.SetSize(n);
    prefs.icons = icons->Value();
    prefs.size = size->Value();
    prefs.date = date->Value();
    BMessage changes;
    changes.AddBool("buttons", prefs.buttons != (buttons->Value() != 0));
    changes.AddBool("command", prefs.command != (command->Value() != 0));
    prefs.buttons = buttons->Value();
    prefs.command = command->Value();
    prefs.folders = folders->Text();
    prefs.files = files->Text();
    prefs.links = links->Text();
    prefs.editor = editor->Text();
    prefs.terminal = terminal->Text();
    int32 index = 0;
    for (Workspace workspace : kSettingsWorkspaces) {
      auto menu = defaultModes[index++];
      const int32 mode = menu->IndexOf(menu->FindMarked());
      changes.AddBool(WorkspaceId(workspace),
                      prefs.DefaultMode(workspace) != mode);
      prefs.SetDefaultMode(workspace, mode);
    }
    BMessage result = prefs.Save();
    BMessage delta = prefs.SharedChanges(&before);
    result.AddMessage("sharedChanges", &delta);
    result.AddMessage("layoutChanges", &changes);
    result.what = kApplyPreferences;
    target.SendMessage(&result);
    Quit();
  }
};
std::mutex preferencesLock;
std::vector<std::pair<BMessenger, BMessenger>> preferencesWindows;
} // namespace

bool PreferencesOpenFor(BMessenger owner) {
  std::lock_guard<std::mutex> lock(preferencesLock);
  for (const auto &entry : preferencesWindows)
    if (entry.first == owner && entry.second.IsValid())
      return true;
  return false;
}

void OpenPreferences(BMessenger target, const Preferences &prefs) {
  std::lock_guard<std::mutex> lock(preferencesLock);
  for (auto it = preferencesWindows.begin(); it != preferencesWindows.end();) {
    if (!it->second.IsValid()) {
      it = preferencesWindows.erase(it);
      continue;
    }
    if (it->first == target && it->second.LockTarget()) {
      BLooper *looper = nullptr;
      it->second.Target(&looper);
      static_cast<BWindow *>(looper)->Activate();
      looper->Unlock();
      return;
    }
    ++it;
  }
  auto window = new PreferencesWindow(target, prefs);
  preferencesWindows.emplace_back(target, BMessenger(window));
  window->Show();
}
