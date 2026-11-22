// SPDX-License-Identifier: MIT
#include "AdaptiveScrollView.h"
#include "JobResultsView.h"
#include "OperationQueue.h"
#include "ThemeTextView.h"
#include <Button.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <algorithm>

namespace {
constexpr uint32 kSelectReport = 'jslr', kPreviousPage = 'jprp',
                 kNextPage = 'jnxp', kCancelWaiting = 'jcnw',
                 kClearFinished = 'jclr';
constexpr size_t kPageSize = 50;
constexpr size_t kActivityBytes = 65536;

std::string Label(const std::string &status) {
  if (status == "source-retained")
    return "Copied; source retained";
  if (status == "partial")
    return "Partially completed";
  if (status == "unprocessed")
    return "Not processed";
  if (status.empty())
    return "Unknown";
  std::string label(status);
  if (label.front() >= 'a' && label.front() <= 'z')
    label.front() -= 'a' - 'A';
  return label;
}
}

JobResultsView::JobResultsView(OperationQueue *owner)
    : BGroupView(B_VERTICAL, 3), queue(owner) {
  jobs = new BPopUpMenu("Recent activity");
  auto recent = new BMenuItem("Recent activity", new BMessage(kSelectReport));
  jobs->AddItem(recent);
  recent->SetMarked(true);
  auto field = new BMenuField("job-picker", "Job:", jobs);
  field->SetExplicitMinSize(BSize(140, B_SIZE_UNSET));
  field->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
  previous =
      new BButton("previous-results", "Previous", new BMessage(kPreviousPage));
  next = new BButton("next-results", "Next", new BMessage(kNextPage));
  cancelWaiting = new BButton("cancel-waiting", "Cancel selected",
                              new BMessage(kCancelWaiting));
  cancelWaiting->SetToolTip("Remove the selected waiting job from the queue.");
  clear = new BButton("clear-finished", "Clear finished",
                      new BMessage(kClearFinished));
  clear->SetToolTip(
      "Forget finished job reports in this window. Files are not changed.");
  clear->SetEnabled(false);
  text = new ThemeTextView("history", B_DOCUMENT_BACKGROUND_COLOR,
                           B_DOCUMENT_TEXT_COLOR);
  text->MakeEditable(false);
  text->MakeSelectable(true);
  text->SetWordWrap(true);
  auto scroll = new Listi::AdaptiveScrollView("history-scroll", text, false);
  scroll->SetExplicitMinSize(BSize(260, 85));
  scroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 135));
  BLayoutBuilder::Group<>(this, B_VERTICAL, 3)
      .AddGroup(B_HORIZONTAL, 4)
      .Add(field)
      .Add(previous)
      .Add(next)
      .Add(cancelWaiting)
      .Add(clear)
      .End()
      .Add(scroll);
  Display();
}
void JobResultsView::AttachedToWindow() {
  BGroupView::AttachedToWindow();
  jobs->SetTargetForItems(this);
  for (auto button : {previous, next, cancelWaiting, clear})
    button->SetTarget(this);
}
void JobResultsView::Accepted(int64 id, const char *title) {
  if (rows.count(id))
    return;
  auto msg = new BMessage(kSelectReport);
  msg->AddInt64("operation", id);
  auto item = new BMenuItem("", msg);
  jobs->AddItem(item, 1);
  item->SetTarget(this);
  rows.emplace(id,
               Row{item, "#" + std::to_string(id) + " " + title, true, false});
  SetState(id, "Waiting", true);
}
void JobResultsView::SetState(int64 id, const char *state, bool waiting) {
  auto found = rows.find(id);
  if (found == rows.end())
    return;
  auto &row = found->second;
  row.item->SetLabel((row.title + " (" + state + ")").c_str());
  row.waiting = waiting;
  if (selected == id) {
    row.item->SetMarked(true);
    Display();
  }
}
void JobResultsView::Started(int64 id) { SetState(id, "Running", false); }

void JobResultsView::Finished(const BMessage &event) {
  const int64 id = event.GetInt64("operation", 0);
  auto found = rows.find(id);
  if (found != rows.end()) {
    found->second.finished = true;
    clear->SetEnabled(true);
  }
  SetState(id,
           event.GetBool("cancelled", false)  ? "Cancelled"
           : event.GetBool("complete", false) ? "Done"
                                              : "Review",
           false);
  std::string log = id ? "#" + std::to_string(id) + " " : "";
  log += event.GetString("result", "");
  if (const char *transfer = event.GetString("transferResult", nullptr))
    log += std::string("\n") + transfer;
  const char *source, *destination;
  if (event.FindString("retainedSource", &source) == B_OK &&
      event.FindString("retainedDestination", &destination) == B_OK)
    log += std::string("\nComplete copy kept at: ") + destination +
           "\nListi did not remove anything from the source: " + source;
  const char *error;
  for (int i = 0; event.FindString("error", i, &error) == B_OK; ++i)
    log += std::string("\n") + error;
  AppendActivity(log);
}
void JobResultsView::ShowFileError(const std::string &line) {
  selected = 0;
  first = 0;
  jobs->ItemAt(0)->SetMarked(true);
  AppendActivity(line);
}
void JobResultsView::AppendActivity(const std::string &line) {
  activity += line + "\n";
  if (activity.size() > kActivityBytes) {
    size_t start = activity.size() - kActivityBytes;
    while (start < activity.size() && (uint8(activity[start]) & 0xc0) == 0x80)
      ++start;
    activity.erase(0, start);
  }
  if (!selected)
    Display();
}
void JobResultsView::Display() {
  previous->SetEnabled(false);
  next->SetEnabled(false);
  cancelWaiting->SetEnabled(false);
  if (!selected) {
    text->SetText(activity.c_str());
    text->Select(text->TextLength(), text->TextLength());
    text->ScrollToSelection();
    return;
  }
  const auto &row = rows.at(selected);
  std::string page = row.item->Label();
  if (auto report = queue->Report(selected)) {
    const auto &r = report->result;
    page += "\n" + report->summary + "\n";
    const size_t entries = r.outcomes.size(), lines = entries + r.errors.size();
    const size_t end = std::min(lines, first + kPageSize);
    if (lines > kPageSize)
      page += "Showing " + std::to_string(first + 1) + " to " +
              std::to_string(end) + " of " + std::to_string(lines) + "\n";
    for (size_t i = first; i < end; ++i) {
      if (i >= entries) {
        page += "\nError: " + r.errors[i - entries] + "\n";
        continue;
      }
      const auto &e = r.outcomes[i];
      page += "\n" + Label(e.status) + ": " + e.source;
      if (!e.destination.empty())
        page += "\n  Destination: " + e.destination;
      if (!e.detail.empty())
        page += "\n  " + e.detail;
      page += "\n";
    }
    previous->SetEnabled(first > 0);
    next->SetEnabled(end < lines);
  } else if (row.waiting) {
    auto cancel = new BMessage(kCancelWaiting);
    cancel->AddInt64("operation", selected);
    cancelWaiting->SetMessage(cancel);
    cancelWaiting->SetEnabled(true);
  }
  text->SetText(page.c_str());
  text->Select(0, 0);
  text->ScrollToSelection();
}
void JobResultsView::ClearFinished() {
  std::vector<int64> finished;
  for (auto it = rows.begin(); it != rows.end();) {
    if (!it->second.finished) {
      ++it;
      continue;
    }
    if (it->first == selected) {
      selected = 0;
      first = 0;
      jobs->ItemAt(0)->SetMarked(true);
    }
    finished.push_back(it->first);
    jobs->RemoveItem(it->second.item);
    delete it->second.item;
    it = rows.erase(it);
  }
  queue->ForgetReports(finished);
  activity.clear();
  clear->SetEnabled(false);
  Display();
}
void JobResultsView::MessageReceived(BMessage *m) {
  switch (m->what) {
  case kSelectReport: {
    const int64 id = m->GetInt64("operation", 0);
    if (id && !rows.count(id))
      return;
    selected = id;
    first = 0;
    Display();
    break;
  }
  case kClearFinished:
    ClearFinished();
    break;
  case kPreviousPage:
    if (previous->IsEnabled()) {
      first -= kPageSize;
      Display();
    }
    break;
  case kNextPage:
    if (next->IsEnabled()) {
      first += kPageSize;
      Display();
    }
    break;
  case kCancelWaiting:
    if (cancelWaiting->IsEnabled() && m->GetInt64("operation", 0) == selected &&
        queue->CancelWaiting(selected))
      cancelWaiting->SetEnabled(false);
    break;
  default:
    BGroupView::MessageReceived(m);
  }
}
