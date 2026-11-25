// SPDX-License-Identifier: MIT
#include "ReadWorker.h"
#include "PaneNavigationState.h"
#include "SearchWindow.h"
#include "Common.h"
#include "ExplicitSubmit.h"
#include "FileSearch.h"
#include "SearchWorker.h"
#include "SearchResultsView.h"
#include "SavedSearches.h"
#include <GroupView.h>
#include <MessageRunner.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <Looper.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>
#include <memory>
#include <stdexcept>
#include <utility>
namespace {
using namespace Listi;
constexpr uint32 kStart = 'ssrt', kStop = 'sstp', kHit = 'shit',
                 kFinished = 'sfin', kSave = 'sqsv', kForget = 'sqfg',
                 kLoad = 'sqld', kName = 'sqnm', kMode = 'smod',
                 kClearFilters = 'sclr', kDeliveryCheck = 'sdck',
                 kSection = 'ssec', kOrder = 'sord';
class SearchView : public BGroupView {
  bool embedded;
  BPopUpMenu *sectionMenu = nullptr, *orderMenu;
  BGroupView *sections[3]{};
  int32 section = 0;
  bool initialized = false;
  BMessenger owner;
  int32 pane;
  BTextControl *root, *expression, *name;
  BTextControl *extension, *minimum, *maximum, *fromDate, *throughDate;
  BCheckBox *createdDate;
  BButton *clearFilters;
  BPopUpMenu *savedMenu;
  BMenuField *savedField;
  BButton *save, *forget;
  SavedSearches saved;
  std::string chosen;
  bool chosenChanged = false;
  int64 storeRevision = 0, resultRevision = 0;
  bool writable = true, saving = false, initial = true;
  std::vector<SearchHit> pending;
  BCheckBox *predicate, *trash, *live;
  BStringView *status, *selected, *scope, *hint;
  BButton *start, *stop, *show, *openList;
  SearchResultsView *results;
  Listi::ReadWorker *worker = nullptr;
  std::shared_ptr<std::atomic<int64>> generation;
  std::shared_ptr<std::atomic<int64>> failedSerial =
      std::make_shared<std::atomic<int64>>(-1);
  std::unique_ptr<BMessageRunner> deliveryTimer;
  int64 serial = 0;
  bool busy = false;
  static BStringView *Line(const char *name, const char *text) {
    auto view = new BStringView(name, text);
    view->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
    view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    view->SetTruncation(B_TRUNCATE_MIDDLE);
    view->SetToolTip(text);
    return view;
  }
  void Status(const std::string &text) {
    status->SetText(text.c_str());
    status->SetToolTip(text.c_str());
  }
  void Busy(bool value) {
    busy = value;
    if (!busy)
      deliveryTimer.reset();
    start->SetEnabled(!busy);
    stop->SetEnabled(busy);
    root->SetEnabled(!busy);
    expression->SetEnabled(!busy);
    predicate->SetEnabled(!busy);
    trash->SetEnabled(!busy);
    for (auto field : {extension, minimum, maximum, fromDate, throughDate})
      field->SetEnabled(!busy);
    createdDate->SetEnabled(!busy);
    clearFilters->SetEnabled(!busy);
    live->SetEnabled(!busy && predicate->Value() == B_CONTROL_ON);
  }
  SearchFilters Filters() const {
    return {extension->Text(),   minimum->Text(),
            maximum->Text(),     fromDate->Text(),
            throughDate->Text(), createdDate->Value() == B_CONTROL_ON};
  }
  void Filters(const SearchFilters &f) {
    extension->SetText(f.extension.c_str());
    minimum->SetText(f.minimumSize.c_str());
    maximum->SetText(f.maximumSize.c_str());
    fromDate->SetText(f.fromDate.c_str());
    throughDate->SetText(f.throughDate.c_str());
    createdDate->SetValue(f.created);
  }
  bool ValidateFilters(const SearchRequest &request) {
    try {
      CompileSearchFilters(request.filters);
      if (request.expression.empty() &&
          (request.predicate || !request.filters.Active()))
        throw std::runtime_error(
            "Enter a name, at least one file filter, or a BFS formula.");
      return true;
    } catch (const std::exception &e) {
      Status(e.what());
      return false;
    }
  }
  SearchRequest Criteria() const {
    SearchRequest request;
    request.root = root->Text();
    request.expression = expression->Text();
    request.predicate = predicate->Value() == B_CONTROL_ON;
    request.includeTrash = trash->Value() == B_CONTROL_ON;
    request.filters = Filters();
    return request;
  }
  void StoreControls() {
    save->SetEnabled(writable && !saving &&
                     !(chosenChanged && chosen == name->Text()));
    forget->SetEnabled(writable && !saving && !chosenChanged && !chosen.empty() &&
                       chosen == name->Text());
    name->SetEnabled(!saving);
    savedField->SetEnabled(!saving);
  }
  void Store(const BMessage &m) {
    if (m.GetInt64("revision", 0) < storeRevision)
      return;
    auto next = SavedSearches::FromMessage(m);
    if (!m.HasString("chosen") && !chosen.empty() && !chosenChanged) {
      const auto *before = saved.Find(chosen);
      const auto *after = next.Find(chosen);
      if (before && after) {
        BMessage previous, current;
        before->WriteTo(previous);
        after->WriteTo(current);
        chosenChanged = !previous.HasSameData(current, true, true);
      } else
        chosenChanged = !after;
    }
    saved = std::move(next);
    if (m.HasString("chosen")) {
      chosenChanged = false;
      chosen = m.GetString("chosen", "");
      name->SetText(chosen.c_str());
    }
    storeRevision = m.GetInt64("revision", 0);
    writable = m.GetBool("writable", false);
    saving = m.GetBool("saving", false);
    while (auto item = savedMenu->RemoveItem(int32(0)))
      delete item;
    auto fresh = new BMessage(kLoad);
    fresh->AddString("name", "");
    savedMenu->AddItem(new BMenuItem("New search", fresh));
    for (const auto &item : saved.Items()) {
      auto pick = new BMessage(kLoad);
      pick->AddString("name", item.name.c_str());
      auto entry = new BMenuItem(item.name.c_str(), pick);
      savedMenu->AddItem(entry);
      entry->SetMarked(item.name == chosen);
    }
    savedMenu->SetTargetForItems(this);
    if (!saved.Find(chosen))
      savedMenu->ItemAt(0)->SetMarked(true);
    StoreControls();
    if (m.HasString("error"))
      Status(m.GetString("error", "Save failed."));
    else if (chosenChanged)
      Status("Saved search changed elsewhere. Reload it or save under another name.");
    else if (m.GetBool("committed", false))
      Status(chosen.empty() ? "Saved search forgotten." :
                              "Search criteria saved. Results are never stored.");
  }
  void Cancel() {
    if (!busy)
      return;
    BMessage request(kStop);
    request.AddInt64("serial", serial);
    ++serial;
    generation->store(serial);
    if (worker)
      worker->PostMessage(&request);
    pending.clear();
    Busy(false);
    Status("Stopped: " + std::to_string(results->CountRows()) +
           " results. No longer updating.");
  }

public:
  SearchView(BMessenger target, int32 which, const char *path,
             const BMessage &store, bool compact)
      : BGroupView("file search", B_VERTICAL, compact ? 4 : 8),
        embedded(compact), owner(target), pane(which),
        generation(std::make_shared<std::atomic<int64>>(0)) {
    root = new BTextControl("search scope", "Folder (including subfolders):", path, nullptr);
    expression =
        new BTextControl("search expression", "Name contains:", "", nullptr);
    root->TextView()->SetMaxBytes(B_PATH_NAME_LENGTH - 1);
    expression->TextView()->SetMaxBytes(4096);
    predicate =
        new BCheckBox("bfs formula", "BFS formula", new BMessage(kMode));
    live = new BCheckBox("live results", "Live", nullptr);
    trash = new BCheckBox("include trash", "Include Trash", nullptr);
    extension = new BTextControl("file extension", "Extension:", "", nullptr);
    minimum = new BTextControl("minimum size", "Min size:", "", nullptr);
    maximum = new BTextControl("maximum size", "Max size:", "", nullptr);
    fromDate = new BTextControl("from date", "From:", "", nullptr);
    throughDate = new BTextControl("through date", "Through:", "", nullptr);
    createdDate = new BCheckBox("creation date", "Use creation date", nullptr);
    clearFilters = new BButton("clear file filters", "Clear filters",
                               new BMessage(kClearFilters));
    for (auto field : {extension, minimum, maximum, fromDate, throughDate}) {
      field->TextView()->SetMaxBytes(255);
      if (!compact)
        field->SetExplicitMinSize(BSize(180, B_SIZE_UNSET));
    }
    extension->SetToolTip(
        "File extension, case-insensitive, e.g. pdf or tar.gz. Not MIME or wildcards.");
    for (auto field : {minimum, maximum})
      field->SetToolTip(
          "Inclusive size: whole bytes or B, KiB, MiB, GiB. Blank means no bound.");
    for (auto field : {fromDate, throughDate})
      field->SetToolTip(
          "YYYY-MM-DD in local time. Both calendar days included. Blank means no bound.");
    createdDate->SetToolTip(
        "Unchecked: last modification. Checked: native creation time, not metadata-change time.");
    hint = Line(
        "search hint",
        "Names: literal, case-insensitive. Subfolders on this volume; no link traversal.");
    scope = Line("result scope", which == 0
                                     ? "Show original uses the left panel."
                                     : "Show original uses the right panel.");
    status = Line(
        "search status",
        "Choose a folder and search. Results are a snapshot, not a live query.");
    selected = Line("search selected",
                    "Select a result to see its full original path.");
    start = new BButton("start search", "Search", new BMessage(kStart));
    openList = new BButton("open search list", "Open as file list", new BMessage(kOpenSearchList));
    stop = new BButton("stop search", "Stop", new BMessage(kStop));
    show = new BButton("show original",
                       compact ? "Show original" : "Show original in panel",
                       new BMessage(kShowSearchResult));
    results = new SearchResultsView(target, which);
    orderMenu = new BPopUpMenu("result order");
    const char *orders[] = {"As found", "Name ↑", "Name ↓", "Folder ↑", "Folder ↓"};
    for (int32 i = 0; i < 5; ++i) {
      auto message = new BMessage(kOrder);
      message->AddInt32("order", i);
      orderMenu->AddItem(new BMenuItem(orders[i], message));
    }
    orderMenu->ItemAt(0)->SetMarked(true);
    auto orderField = new BMenuField("result order", "Sort:", orderMenu);
    savedMenu = new BPopUpMenu("saved searches");
    savedField = new BMenuField("saved searches", "Saved:", savedMenu);
    savedField->SetExplicitMinSize(BSize(130, B_SIZE_UNSET));
    savedField->SetExplicitMaxSize(BSize(220, B_SIZE_UNSET));
    name = new BTextControl("search name", "Name:", "", nullptr);
    name->TextView()->SetMaxBytes(80);
    name->SetModificationMessage(new BMessage(kName));
    save = new BButton("save search", "Save", new BMessage(kSave));
    forget = new BButton("forget search", "Forget", new BMessage(kForget));
    BuildSections(orderField);
    Store(store);
    Busy(false);
    show->SetEnabled(false);
    results->UpdateColors();
  }

  void BuildSections(BMenuField *orderField) {
    // Keep the everyday name search short. Criteria and saved searches use
    // the same controls and worker as the standalone form, on demand.
    sectionMenu = new BPopUpMenu("search section");
    const char *labels[] = {"Results", "File filters", "Saved searches"};
    for (int32 i = 0; i < 3; ++i) {
      auto pick = new BMessage(kSection);
      pick->AddInt32("section", i);
      sectionMenu->AddItem(new BMenuItem(labels[i], pick));
      sections[i] = new BGroupView(B_VERTICAL, 4);
    }
    sectionMenu->ItemAt(0)->SetMarked(true);
    auto sectionField = new BMenuField("search section", nullptr, sectionMenu);
    sectionField->SetToolTip("Switch between results, file filters and saved searches");
    BLayoutBuilder::Group<>(sections[0], B_VERTICAL, 4)
        .Add(root)
        .Add(expression)
        .AddGroup(B_HORIZONTAL, 4)
        .Add(start)
        .Add(stop)
        .AddGlue()
        .Add(openList)
        .Add(show)
        .End()
        .Add(results)
        .Add(selected);
    BLayoutBuilder::Group<>(sections[1], B_VERTICAL, 3)
        .Add(extension)
        .Add(minimum)
        .Add(maximum)
        .Add(fromDate)
        .Add(throughDate)
        .Add(createdDate)
        .AddGroup(B_HORIZONTAL, 4)
        .Add(predicate)
        .Add(live)
        .Add(trash)
        .End()
        .Add(clearFilters)
        .Add(hint)
        .AddGlue();
    BLayoutBuilder::Group<>(sections[2], B_VERTICAL, 4)
        .Add(savedField)
        .Add(name)
        .AddGroup(B_HORIZONTAL, 4)
        .Add(save)
        .Add(forget)
        .End()
        .AddGlue();
    BLayoutBuilder::Group<>(this, B_VERTICAL, 4)
        .SetInsets(4)
        .AddGroup(B_HORIZONTAL, 4)
        .Add(sectionField)
        .Add(orderField)
        .End()
        .Add(sections[0])
        .Add(sections[1])
        .Add(sections[2])
        .Add(scope)
        .Add(status);
    sections[1]->Hide();
    sections[2]->Hide();
  }

  void SelectSection(int32 next) {
    if (!sectionMenu || next < 0 || next > 2 || next == section)
      return;
    sections[section]->Hide();
    section = next;
    sections[section]->Show();
    sectionMenu->ItemAt(section)->SetMarked(true);
  }

  void AllAttached() override {
    BGroupView::AllAttached();
    // Controls and asynchronous updates belong to this search, not the host
    // window (which may contain a second independent search).
    for (BControl *control : std::initializer_list<BControl *>{
             root, expression, name, predicate, start, stop, show, openList, save, forget,
             clearFilters})
      control->SetTarget(this);
    savedMenu->SetTargetForItems(this);
    if (sectionMenu)
      sectionMenu->SetTargetForItems(this);
    orderMenu->SetTargetForItems(this);
    results->SetReceiver(this);
    if (!initialized) {
      for (auto field : {root, expression, extension, minimum, maximum, fromDate,
                         throughDate})
        field->TextView()->AddFilter(
            new Listi::ExplicitSubmit(BMessenger(this), BMessage(kStart)));
      worker = NewSearchWorker(BMessenger(this), generation, failedSerial);
      Listi::StartReadWorker(worker);
      initialized = true;
    }
  }

  ~SearchView() {
    generation->fetch_add(1);
    Listi::RetireReadWorker(worker);
  }
  void MessageReceived(BMessage *m) override {
    switch (m->what) {
    case kSearchStore:
      Store(*m);
      break;
    case kName:
      StoreControls();
      break;
    case kLoad: {
      if (saving)
        break;
      const std::string requested = m->GetString("name", "");
      if (!requested.empty() && !saved.Find(requested)) {
        Status("That saved search no longer exists.");
        break;
      }
      Cancel();
      chosenChanged = false;
      chosen = requested;
      name->SetText(chosen.c_str());
      if (const auto *item = saved.Find(chosen)) {
        root->SetText(item->request.root.c_str());
        expression->SetText(item->request.expression.c_str());
        predicate->SetValue(item->request.predicate);
        trash->SetValue(item->request.includeTrash);
        live->SetValue(item->live);
        Filters(item->request.filters);
      } else {
        expression->SetText("");
        predicate->SetValue(0);
        trash->SetValue(0);
        live->SetValue(0);
        Filters({});
      }
      results->Clear();
      results->InvalidateLayout();
      show->SetEnabled(false);
      selected->SetText("");
      selected->SetToolTip("");
      scope->SetText(pane == 0 ? "Show original uses the left panel."
                               : "Show original uses the right panel.");
      scope->SetToolTip(scope->Text());
      BMessenger(this).SendMessage(kMode);
      StoreControls();
      SelectSection(0);
      expression->MakeFocus();
      Status("Criteria loaded. Press Search to obtain current results.");
      break;
    }
    case kSave:
    case kForget: {
      if (!writable || saving || (chosenChanged && chosen == name->Text()))
        break;
      if (m->what == kForget && (chosen.empty() || chosen != name->Text()))
        break;
      BMessage edit(kSearchEdit);
      // The owner addresses embedded searches by view, standalone by window.
      edit.AddMessenger("searchEditor", embedded ? BMessenger(this)
                                                  : BMessenger(Window()));
      edit.AddInt64("revision", storeRevision);
      edit.AddBool("remove", m->what == kForget);
      edit.AddBool("replace", !chosen.empty() && chosen == name->Text());
      if (!chosen.empty() && chosen == name->Text()) {
        if (const auto *previous = saved.Find(chosen)) {
          BMessage expected;
          previous->WriteTo(expected);
          edit.AddMessage("expectedSearch", &expected);
        }
      }
      if (m->what == kForget)
        edit.AddString("name", name->Text());
      else {
        SavedSearch item{name->Text(), Criteria(),
                         live->Value() == B_CONTROL_ON};
        if (!ValidateFilters(item.request))
          break;
        item.WriteTo(edit);
      }
      if (owner.SendMessage(&edit) == B_OK) {
        saving = true;
        StoreControls();
      } else
        Status(
            "Could not queue saved search. Previous criteria are unchanged.");
      break;
    }
    case kClearFilters:
      if (!busy) {
        Filters({});
        Status("File filters cleared. Name or BFS formula is unchanged.");
      }
      break;
    case kOrder:
      results->Order(m->GetInt32("order", -1));
      break;
    case kSection:
      SelectSection(m->GetInt32("section", -1));
      break;
    case kSearch:
      SelectSection(0);
      if (busy)
        results->MakeFocus();
      else
        expression->MakeFocus();
      break;
    case kMode: {
      bool formula = predicate->Value() == B_CONTROL_ON;
      if (!formula)
        live->SetValue(0);
      live->SetEnabled(formula && !busy);
      expression->SetLabel(formula ? "BFS formula:" : "Name contains:");
      const char *text =
          formula
              ? "Example: name==\"*.txt\". Native indexed BFS syntax; Live keeps this folder updated."
              : "Names: literal, case-insensitive. Subfolders on this volume; no link traversal.";
      hint->SetText(text);
      hint->SetToolTip(text);
      break;
    }
    case kStart: {
      if (busy)
        break;
      BMessage request(kStart);
      const auto criteria = Criteria();
      if (!ValidateFilters(criteria))
        break;
      criteria.WriteCriteria(request);
      SelectSection(0);
      ++serial;
      generation->store(serial);
      resultRevision = 0;
      pending.clear();
      results->Clear();
      results->InvalidateLayout();
      show->SetEnabled(false);
      selected->SetText("");
      selected->SetToolTip("");
      request.AddInt64("serial", serial);
      request.AddBool("live", live->Value() == B_CONTROL_ON);
      BString description(pane == 0 ? "Left panel | " : "Right panel | ");
      description << criteria.root.c_str() << " | " << criteria.expression.c_str();
      const auto &filters = criteria.filters;
      if (filters.Active()) {
        description << " | Files";
        if (!filters.extension.empty())
          description << " | extension: " << filters.extension.c_str();
        if (!filters.minimumSize.empty())
          description << " | min: " << filters.minimumSize.c_str();
        if (!filters.maximumSize.empty())
          description << " | max: " << filters.maximumSize.c_str();
        if (!filters.fromDate.empty() || !filters.throughDate.empty()) {
          description << (filters.created ? " | created: " : " | modified: ");
          description << filters.fromDate.c_str() << " through "
                      << filters.throughDate.c_str();
        }
      }
      scope->SetText(description);
      scope->SetToolTip(description);
      Busy(true);
      Status("Searching... Stop keeps only the results received so far.");
      BMessage check(kDeliveryCheck);
      deliveryTimer = std::make_unique<BMessageRunner>(BMessenger(this), &check, 250000);
      if (!worker) {
        worker = NewSearchWorker(BMessenger(this), generation, failedSerial);
        Listi::StartReadWorker(worker);
      }
      if (!worker || deliveryTimer->InitCheck() != B_OK ||
          worker->PostMessage(&request) != B_OK) {
        Busy(false);
        Status("Could not start search.");
      }
      break;
    }
    case kDeliveryCheck:
      if (busy && failedSerial->load() == serial) {
        Cancel();
        Status("Incomplete: search results could not be delivered. Start again to retry.");
      }
      break;
    case kStop:
      if (busy) {
        Cancel();
        const int32 count = results->CountRows();
        Status("Stopped: " + std::to_string(count) +
               (count == 1 ? " result kept." : " results kept."));
      }
      break;
    case kSearchBegin:
      if (!busy || m->GetInt64("serial", -1) != serial)
        break;
      if (m->GetInt64("revision", -1) <= resultRevision)
        break;
      resultRevision = m->GetInt64("revision", -1);
      initial = m->GetBool("initial", false);
      pending.clear();
      break;
    case kHit: {
      if (!busy || m->GetInt64("serial", -1) != serial)
        break;
      if (resultRevision == 0 || m->GetInt64("revision", -1) != resultRevision)
        break;
      BMessage item;
      for (int32 i = 0; m->FindMessage("hit", i, &item) == B_OK; ++i) {
        SearchHit hit{item.GetString("path", ""), item.GetInt64("device", -1),
                      item.GetInt64("node", -1)};
        if (hit.path.empty() || hit.path[0] != '/' || hit.device < 0 ||
            hit.node < 0)
          continue;
        if (initial)
          results->Add(hit);
        else
          pending.push_back(hit);
      }
      if (initial)
        Status("Searching: " + std::to_string(results->CountRows()) +
               " results. Snapshot in progress.");
      break;
    }
    case kFinished: {
      if (!busy || m->GetInt64("serial", -1) != serial)
        break;
      if (m->GetInt64("revision", -1) != resultRevision)
        break;
      bool updating = m->GetBool("live", false);
      if (!initial) {
        results->Reconcile(pending);
        pending.clear();
        BMessenger(this).SendMessage(kSearchResultPicked);
      }
      Busy(updating);
      std::string error = m->GetString("error", "");
      std::string state =
          !error.empty()                   ? "Failed / incomplete"
          : m->GetBool("cancelled", false) ? "Interrupted / incomplete"
          : m->GetBool("limited", false)   ? "Limit reached / incomplete"
          : m->GetInt64("skipped", 0)      ? "Finished with skipped areas"
          : updating                       ? "Live"
                                           : "Complete snapshot";
      Status(state + ": " + std::to_string(results->CountRows()) +
             " results; " + std::to_string(m->GetInt64("visited", 0)) +
             " examined; " + std::to_string(m->GetInt64("skipped", 0)) +
             " skipped." + (error.empty() ? "" : " " + error));
      break;
    }
    case kSearchResultPicked: {
      const auto *hit = results->SelectedHit();
      selected->SetText(hit ? hit->path.c_str() : "");
      selected->SetToolTip(hit ? hit->path.c_str() : "");
      show->SetEnabled(hit != nullptr);
      break;
    }
    case kOpenSearchList: {
      auto criteria = Criteria();
      if (!ValidateFilters(criteria))
        break;
      SavedSearch query{name->Text(), criteria,
                        criteria.predicate && live->Value() == B_CONTROL_ON};
      BMessage request(kOpenSearchList);
      query.WriteTo(request);
      request.AddInt32("pane", pane);
      owner.SendMessage(&request);
      break;
    }
    case kShowSearchResult: {
      const auto *hit = results->SelectedHit();
      if (hit)
        results->ShowOriginal(*hit);
      break;
    }
    case B_COLORS_UPDATED:
      BGroupView::MessageReceived(m);
      results->UpdateColors();
      break;
    default:
      BGroupView::MessageReceived(m);
      break;
    }
  }
};

class SearchWindow : public BWindow {
  SearchView *content;
public:
  SearchWindow(BMessenger target, int32 which, const char *path,
               const BMessage &store)
      : BWindow(BRect(90, 90, 1000, 720), "Listi: Find files", B_TITLED_WINDOW,
                B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS) {
    content = new SearchView(target, which, path, store, false);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0).Add(content);
    AddShortcut(B_ESCAPE, B_NO_COMMAND_KEY, new BMessage(kStop));
    AddShortcut('W', B_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
    BMessenger(content).SendMessage(kSearch);
  }

  void MessageReceived(BMessage *message) override {
    // Preserve the window messenger contract for callers outside Smiðja.
    switch (message->what) {
    case kSearch:
      Activate();
      // fall through
    case kSearchStore:
    case kStart:
    case kStop:
      content->MessageReceived(message);
      break;
    default:
      BWindow::MessageReceived(message);
    }
  }
};
} // namespace

BView *CreatePanelSearch(BMessenger owner, int32 pane, const char *root,
                         const BMessage &store) {
  return new SearchView(owner, pane, root, store, true);
}

BMessenger OpenFileSearch(BMessenger owner, int32 pane, const char *root,
                          const BMessage &store) {
  auto window = new SearchWindow(owner, pane, root, store);
  BMessenger result(window);
  window->Show();
  return result;
}
