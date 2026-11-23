// SPDX-License-Identifier: MIT
#include "PanelTools.h"
#include "FlatView.h"
#include "ViewVisibility.h"

#include "ImagePreview.h"
#include "Pane.h"
#include "SearchWindow.h"
#include <Button.h>
#include <LayoutBuilder.h>
#include <Window.h>

namespace Listi {
PanelTools::PanelTools(int32 index, Pane &list)
    : BGroupView(B_VERTICAL, 0), pane(index), files(list) {
  SetName(pane == 0 ? "first panel tools" : "second panel tools");
  pages[0] = &files;
  tabs = new BGroupView(B_HORIZONTAL, 2);
  tabs->SetName("panel tabs");
  const char *labels[] = {"Files", "Search", "Preview"};
  for (int32 i = 0; i < int32(buttons.size()); ++i) {
    auto *choose = new BMessage(kPanelPage);
    choose->AddInt32("pane", pane);
    choose->AddInt32("page", i);
    buttons[i] = new BButton(labels[i], choose);
    buttons[i]->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    tabs->AddChild(buttons[i]);
  }
  auto *close = new BMessage(kClosePanelPage);
  close->AddInt32("pane", pane);
  closeButton = new BButton("×", close);
  closeButton->SetToolTip("Close this tool and return to files");
  BLayoutBuilder::Group<>(tabs).AddGlue().Add(closeButton);
  GroupLayout()->AddView(tabs, 0.0f);
  AddChild(&files);
  tabs->Hide();
  UpdateTabs();
}

bool PanelTools::Select(PanelPage page) {
  if (!Page(page) || (current != page && !CanLeaveCurrent())) {
    // A toggle button can change itself before its message reaches us.
    UpdateTabs();
    return false;
  }
  ShowPage(page);
  return true;
}

bool PanelTools::CloseCurrent() {
  const auto page = current;
  if (page == PanelPage::Files)
    return true;
  if (!Select(PanelPage::Files))
    return false;
  // A flat snapshot is discarded whenever we leave it. Search and preview
  // pages normally survive tab changes, but not an explicit close.
  if (Page(page)) {
    RemovePage(page);
    UpdateTabs();
  }
  return true;
}

bool PanelTools::CanLeaveCurrent() {
  return current != PanelPage::Preview || CloseEditor();
}

bool PanelTools::CloseEditor() {
  auto *preview = static_cast<ImagePreview *>(Page(PanelPage::Preview));
  return !preview || preview->CloseEditor();
}

void PanelTools::EnableTools(bool enabled) {
  toolsEnabled = enabled;
  if (!enabled && current != PanelPage::Flat)
    ShowPage(PanelPage::Files);
  else
    UpdateTabs();
}

void PanelTools::FocusCurrent() {
  switch (current) {
  case PanelPage::Files:
    files.FocusList();
    break;
  case PanelPage::Search:
  case PanelPage::Flat: {
    BMessage focus(current == PanelPage::Flat ? kFlatView : kSearch);
    Page(current)->MessageReceived(&focus);
    break;
  }
  case PanelPage::Preview:
    Page(current)->MakeFocus();
    break;
  }
}

void PanelTools::OpenFlat(const std::shared_ptr<Shelf> &shelf) {
  if (current == PanelPage::Files)
    Open(PanelPage::Flat,
         CreateFlatView(BMessenger(Window()), shelf, files.Path(), pane));
}

bool PanelTools::OpenSearch(const BMessage &store) {
  if (Page(PanelPage::Search))
    return Select(PanelPage::Search);
  if (!CanLeaveCurrent())
    return false;
  Open(PanelPage::Search,
       CreatePanelSearch(BMessenger(Window()), pane, files.SearchRoot(), store));
  return true;
}

BMessenger PanelTools::SearchMessenger() const {
  auto *search = Page(PanelPage::Search);
  return search ? BMessenger(search) : BMessenger();
}

ImagePreview &PanelTools::OpenPreview() {
  if (auto *preview = static_cast<ImagePreview *>(Page(PanelPage::Preview))) {
    ShowPage(PanelPage::Preview);
    return *preview;
  }
  auto *preview = new ImagePreview();
  Open(PanelPage::Preview, preview);
  return *preview;
}

void PanelTools::UpdatePreview(const PanelTools &opposite) {
  if (auto *preview = static_cast<ImagePreview *>(Page(PanelPage::Preview)))
    preview->ShowFile(current == PanelPage::Preview ? opposite.SelectedFile()
                                                    : "");
}

std::string PanelTools::SelectedFile() const {
  if (current == PanelPage::Flat)
    return FlatSelectedFile(*Page(current));
  return current == PanelPage::Files ? files.SelectedPath() : "";
}

void PanelTools::Open(PanelPage page, BView *view) {
  const auto index = size_t(page);
  pages[index] = view;
  AddChild(view);
  view->Hide();
  ShowPage(page);
}

void PanelTools::RemovePage(PanelPage page) {
  auto *view = Page(page);
  GroupLayout()->RemoveView(view);
  delete view;
  pages[size_t(page)] = nullptr;
}

void PanelTools::ShowPage(PanelPage page) {
  if (current != page) {
    if (current == PanelPage::Flat)
      RemovePage(current);
    else
      Page(current)->Hide();
    Page(page)->Show();
    current = page;
  }
  UpdateTabs();
}

void PanelTools::UpdateTabs() {
  const bool otherTools = Page(PanelPage::Search) || Page(PanelPage::Preview);
  SetViewVisible(tabs, toolsEnabled && otherTools);
  for (size_t i = 0; i < buttons.size(); ++i) {
    const auto page = PanelPage(i);
    SetViewVisible(buttons[i], pages[i] != nullptr);
    const auto selectedTab = current == PanelPage::Flat ? PanelPage::Files : current;
    buttons[i]->SetValue(page == selectedTab ? B_CONTROL_ON : B_CONTROL_OFF);
  }
  closeButton->SetEnabled(current != PanelPage::Files);
}
} // namespace Listi
