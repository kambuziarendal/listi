// SPDX-License-Identifier: MIT
#include "ReadWorker.h"
#include "FolderTree.h"
#include "Common.h"
#include "VirtualFile.h"
#include <LayoutBuilder.h>
#include <Looper.h>
#include <OutlineListView.h>
#include "AdaptiveScrollView.h"
#include <StringItem.h>
#include <Window.h>
#include <algorithm>
namespace {
constexpr uint32 kTreeRead = 'trrd', kTreeLoaded = 'trld';
class TreeReader : public Listi::ReadWorker {
  BMessenger target;
  std::shared_ptr<TerminalReplies> terminal;
  std::shared_ptr<std::atomic<int64>> generation;

public:
  TreeReader(BMessenger t, std::shared_ptr<std::atomic<int64>> g, std::shared_ptr<TerminalReplies> queue)
      : ReadWorker("folder tree reader"), target(t), terminal(queue), generation(g) {}
  void MessageReceived(BMessage *m) override {
    if (m->what != kTreeRead) {
      BLooper::MessageReceived(m);
      return;
    }
    auto serial = m->GetInt64("serial", -1);
    Listi::TransferControl c;
    c.proceed = [&] { return generation->load() == serial; };
    BMessage result(kTreeLoaded);
    result.AddInt64("serial", serial);
    result.AddString("path", m->GetString("path", ""));
    try {
      auto children = Listi::ListFiles(m->GetString("path", ""), c);
      std::sort(children.begin(), children.end(),
                [](const auto &a, const auto &b) { return a.name < b.name; });
      size_t count = 0;
      for (auto &entry : children)
        if (entry.directory && !entry.link) {
          if (++count > 10000)
            throw std::runtime_error("Too many folders in this branch");
          result.AddString("child", entry.path.c_str());
        }
    } catch (const Listi::Cancelled &) {
      return;
    } catch (const std::exception &e) {
      result.AddString("error", e.what());
    }
    terminal->Deliver(result, *generation, [&] {
      return target.SendMessage(&result, (BHandler *)nullptr, 50000);
    });
  }
};
} // namespace
class TreeItem : public BStringItem {
public:
  std::string path;
  bool loaded = false, pending = false;
  TreeItem(const std::string &p, const char *label, uint32 level = 0)
      : BStringItem(label, level, false), path(p) {}
};
class TreeList : public BOutlineListView {
  FolderTree *owner;

public:
  explicit TreeList(FolderTree *o)
      : BOutlineListView("folder tree"), owner(o) {}
  void ExpandOrCollapse(BListItem *item, bool expand) override {
    BOutlineListView::ExpandOrCollapse(item, expand);
    Parent()->InvalidateLayout();
    auto tree = dynamic_cast<TreeItem *>(item);
    if (expand && tree)
      owner->ExpandPath(tree->path);
  }
  void MouseDown(BPoint point) override {
    auto before = CurrentSelection();
    BOutlineListView::MouseDown(point);
    auto after = CurrentSelection();
    if (after >= 0 && after != before) {
      auto item = dynamic_cast<TreeItem *>(ItemAt(after));
      if (item && item->IsEnabled())
        owner->NavigateSelection(item->path);
    }
  }
  void KeyDown(const char *bytes, int32 length) override {
    if (length == 1 && bytes[0] == B_TAB) {
      Window()->PostMessage(kSwitch);
      return;
    }
    auto before = CurrentSelection();
    BOutlineListView::KeyDown(bytes, length);
    auto after = CurrentSelection();
    if (after >= 0 &&
        (after != before || (length == 1 && bytes[0] == B_ENTER))) {
      auto item = dynamic_cast<TreeItem *>(ItemAt(after));
      if (item && item->IsEnabled())
        owner->NavigateSelection(item->path);
    }
  }
};
FolderTree::FolderTree(BHandler *target)
    : BView("folder tree panel", B_PULSE_NEEDED | B_WILL_DRAW | B_SUPPORTS_LAYOUT),
      navigationTarget(target),
      generation(std::make_shared<std::atomic<int64>>(0)) {
  list = new TreeList(this);
  auto scroll = new Listi::AdaptiveScrollView("folder tree scroll", list, true);
  BLayoutBuilder::Group<>(this, B_VERTICAL, 0).Add(scroll);
  SetExplicitMinSize(BSize(110, 96));
  SetExplicitPreferredSize(BSize(170, 300));
}
FolderTree::~FolderTree() {
  generation->store(-1);
  Listi::RetireReadWorker(reader);
  Clear();
}
void FolderTree::Clear() {
  std::vector<BListItem *> all;
  for (int32 i = 0; i < list->FullListCountItems(); ++i)
    all.push_back(list->FullListItemAt(i));
  list->MakeEmpty();
  list->Parent()->InvalidateLayout();
  for (auto item : all)
    delete item;
  items.clear();
}
void FolderTree::AttachedToWindow() {
  if (reader)
    return;
  reader = new TreeReader(BMessenger(this), generation, terminal);
  Listi::StartReadWorker(reader);
}
std::string FolderTree::PathAt(BPoint screen) const {
  const BPoint point = list->ConvertFromScreen(screen);
  if (!list->Bounds().Contains(point))
    return {};
  const auto item = dynamic_cast<TreeItem *>(list->ItemAt(list->IndexOf(point)));
  return item && item->IsEnabled() ? item->path : std::string();
}
void FolderTree::NavigateSelection(const std::string &path) {
  BMessage m(kNavigate);
  m.AddString("path", path.c_str());
  m.AddBool("tree", true);
  BMessenger(navigationTarget).SendMessage(&m);
}
void FolderTree::ShowPath(const std::string &path) {
  if (!reader || IsHidden())
    return;
  try {
    const bool overview = path == "@disks";
    auto location = Listi::ParseLocation(overview ? "/" : path);
    location.path = "/";
    std::string newRoot = location.URL();
    desired = overview ? "" : path;
    // A directory created since this tree was loaded may be absent even when
    // its ancestor is cached. Rebuild that route instead of keeping an unrelated
    // selection; clicks on already loaded nodes keep the expanded tree intact.
    if (root != newRoot || (!overview && !items.count(path))) {
      ++*generation;
      Clear();
      root = newRoot;
      auto item =
          new TreeItem(root, location.Local()     ? "Filesystem"
                             : location.Archive() ? "Archive"
                                                  : location.host.c_str());
      items[root] = item;
      list->AddItem(item);
      auto placeholder = new BStringItem("Loading...");
      placeholder->SetEnabled(false);
      list->AddUnder(placeholder, item);
      list->Expand(item);
      ExpandPath(root);
    }
    Follow();
  } catch (const std::exception &) {
  }
}
void FolderTree::Reset() {
  ++*generation;
  Clear();
  root.clear();
  desired.clear();
}
void FolderTree::ExpandPath(const std::string &path) {
  auto found = items.find(path);
  if (found == items.end() || found->second->loaded || found->second->pending ||
      !reader)
    return;
  found->second->pending = true;
  BMessage m(kTreeRead);
  m.AddString("path", path.c_str());
  m.AddInt64("serial", generation->load());
  if (reader->PostMessage(&m) != B_OK) {
    m.what = kTreeLoaded;
    m.AddString("error", "Folder reader busy. Expand again to retry.");
    MessageReceived(&m);
  }
}
void FolderTree::Follow() {
  // Disks is not a directory. Late branch reads must not reselect the last
  // folder and consume a click that is meant to return there.
  if (desired.empty()) {
    list->DeselectAll();
    return;
  }
  for (auto &pair : items) {
    auto p = pair.first;
    if (p == desired) {
      list->Select(list->IndexOf(pair.second));
      list->ScrollToSelection();
      return;
    }
    if (p == root || desired.compare(0, p.size() + (p.back() == '/' ? 0 : 1),
                                     p + (p.back() == '/' ? "" : "/")) == 0) {
      list->Expand(pair.second);
      ExpandPath(p);
    }
  }
}
void FolderTree::Pulse() {
  for (auto &reply : terminal->Take())
    MessageReceived(&reply);
}
void FolderTree::MessageReceived(BMessage *m) {
  if (m->what != kTreeLoaded) {
    BView::MessageReceived(m);
    return;
  }
  if (m->GetInt64("serial", -1) != generation->load())
    return;
  auto found = items.find(m->GetString("path", ""));
  if (found == items.end())
    return;
  list->Parent()->InvalidateLayout();
  auto parent = found->second;
  parent->pending = false;
  while (auto child = list->ItemUnderAt(parent, true, 0)) {
    list->RemoveItem(child);
    delete child;
  }
  if (m->HasString("error")) {
    auto error = new BStringItem(m->GetString("error", "Cannot read folder"));
    error->SetEnabled(false);
    list->AddUnder(error, parent);
    return;
  }
  int32 count = 0;
  type_code type;
  m->GetInfo("child", &type, &count);
  if (items.size() + count > 20000) {
    auto error =
        new BStringItem("Tree limit reached. Reopen the tree to reset.");
    error->SetEnabled(false);
    list->AddUnder(error, parent);
    return;
  }
  parent->loaded = true;
  const char *path;
  // AddUnder inserts immediately after the parent, so insert in reverse to
  // preserve the reader's ascending name order.
  for (int32 i = count - 1; i >= 0; --i) {
    if (m->FindString("child", i, &path) != B_OK)
      continue;
    auto item = new TreeItem(path, Listi::LocationLeaf(path).c_str());
    items[path] = item;
    list->AddUnder(item, parent);
    auto dummy = new BStringItem("Loading...");
    dummy->SetEnabled(false);
    list->AddUnder(dummy, item);
  }
  Follow();
}
