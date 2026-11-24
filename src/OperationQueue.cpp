// SPDX-License-Identifier: MIT
#include "OperationQueue.h"
#include "JobResultsView.h"
#include "ArchivePasswords.h"
#include "ArchiveBackend.h"
#include "VirtualFile.h"
#include "ViewVisibility.h"
#include <Button.h>
#include <MessageRunner.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <StatusBar.h>
#include <StringView.h>
#include <Window.h>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <limits>
#include <set>
#include <sys/stat.h>
namespace {
constexpr uint32 kCancelAll = 'jcab', kJobAccepted = 'jacp',
                 kDismissError = 'jder';

uint64_t EstimateBytes(const std::vector<std::string> &sources,
                       const std::function<bool()> &proceed) {
  uint64_t total = 0;
  for (const auto &source : sources) {
    struct stat st;
    if (Listi::IsVirtual(source) || !proceed() || lstat(source.c_str(), &st) ||
        !S_ISREG(st.st_mode) || st.st_size < 0 ||
        uint64_t(st.st_size) > uint64_t(std::numeric_limits<int64_t>::max()) - total)
      return 0;
    total += st.st_size;
  }
  return total;
}
std::string DescribeFile(const std::string &path, int64_t size, time_t modified) {
  char date[64] = "unknown";
  struct tm local;
  if (localtime_r(&modified, &local))
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local);
  return path + "  (" + std::to_string(size) + " bytes, modified " + date + ")";
}

const char *Verb(Listi::Action a) {
  switch (a) {
  case Listi::Action::CreateArchive:
    return "Create archive";
  case Listi::Action::ExtractArchive:
    return "Extract archive";
  case Listi::Action::EditArchive:
    return "Update archive";
  case Listi::Action::MoveArchive:
    return "Move archive entries";
  case Listi::Action::Copy:
    return "Copy";
  case Listi::Action::ConvertImage:
    return "Convert image";
  case Listi::Action::Move:
    return "Move";
  case Listi::Action::Duplicate:
    return "Duplicate";
  case Listi::Action::Link:
    return "Create link";
  case Listi::Action::Trash:
    return "Move to Trash";
  case Listi::Action::Restore:
    return "Restore";
  case Listi::Action::Delete:
    return "Delete";
  case Listi::Action::Rename:
    return "Rename";
  case Listi::Action::BulkRename:
    return "Bulk rename";
  case Listi::Action::UndoBulkRename:
    return "Undo bulk rename";
  default:
    return "Create directory";
  }
}
} // namespace
class ProgressView : public BView {
  OperationQueue *owner;
  BStringView *title, *file, *summary;
  BStatusBar *bar;
  BButton *pause, *cancelButton, *cancelAllButton;
  BGroupView *errorRow;
  BStringView *errorText;
  BButton *dismissError;
  std::deque<std::string> unreadErrors;
  bool hasCompletion = false;

  void ShowNextError() {
    Listi::SetViewVisible(errorRow, !unreadErrors.empty());
    if (unreadErrors.empty())
      return;
    BString label("Error: ");
    label << unreadErrors.front().c_str();
    if (unreadErrors.size() > 1)
      label << " (+" << int64(unreadErrors.size() - 1) << ")";
    errorText->SetText(label);
    errorText->SetToolTip(label);
  }
  void KeepError(const char *error) {
    if (!*error || (!unreadErrors.empty() && unreadErrors.back() == error))
      return;
    // Full job errors remain available in their report.
    if (unreadErrors.size() == 24)
      unreadErrors.pop_back();
    unreadErrors.emplace_back(error);
    ShowNextError();
  }
  BGroupView *choices;
  BStringView *comparison[3];
  BView *details;
  BButton *detailsButton;
  bool expanded = false;
  bool choicesVisible = false;
  bool cancelling = false;
  int64 conflictRequest = 0, activeOperation = 0, lastStarted = 0;
  bool hasByteEstimate = false;
  void ShowChoices(bool show) {
    if (show == choicesVisible)
      return;
    choicesVisible = show;
    if (!show)
      for (auto line : comparison)
        Listi::SetViewVisible(line, false);
    if (show)
      choices->Show();
    else
      choices->Hide();
  }
  JobResultsView *results;

public:
  ProgressView(OperationQueue *o)
      : BView("operations", B_WILL_DRAW | B_SUPPORTS_LAYOUT), owner(o) {
    SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
    title = new BStringView("operation", "No queued operations");
    title->SetFont(be_bold_font);
    file = new BStringView("current-file", "");
    file->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    file->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
    file->SetTruncation(B_TRUNCATE_MIDDLE);
    summary = new BStringView("summary", "Ready");
    summary->SetExplicitMinSize(BSize(180, B_SIZE_UNSET));
    summary->SetTruncation(B_TRUNCATE_END);
    title->SetExplicitMinSize(BSize(140, B_SIZE_UNSET));
    title->SetTruncation(B_TRUNCATE_END);
    bar = new BStatusBar("progress");
    bar->SetBarColor(tint_color(ui_color(B_STATUS_BAR_COLOR), 1.2));
    bar->SetBarHeight(4);
    pause = new BButton("pause", "Pause", new BMessage(kPause));
    cancelButton =
        new BButton("cancel-job", "Cancel job", new BMessage(kAbort));
    cancelAllButton =
        new BButton("cancel-all", "Cancel all", new BMessage(kCancelAll));
    pause->SetEnabled(false);
    cancelButton->SetEnabled(false);
    pause->Hide();
    cancelButton->Hide();
    cancelAllButton->SetEnabled(false);
    file->Hide();
    bar->Hide();
    choices = new BGroupView(B_HORIZONTAL, 4);
    const char *labels[] = {"Replace", "Replace all", "Skip", "Skip all",
                            "Abort", "Keep both"};
    for (int i = 0; i <= int(Listi::Decision::KeepBoth); i++) {
      auto m = new BMessage(kAnswer);
      m->AddInt32("decision", i);
      choices->AddChild(new BButton(labels[i], m));
    }
    choices->Hide();
    cancelButton->SetToolTip("Stops this job. Completed work is kept.");
    cancelAllButton->SetToolTip(
        "Stops the current and waiting jobs. Completed work is kept.");
    results = new JobResultsView(owner);
    details = new BGroupView(B_VERTICAL, 5);
    for (auto &line : comparison) {
      line = new BStringView("conflict-detail", "");
      line->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
      line->SetTruncation(B_TRUNCATE_MIDDLE);
      line->Hide();
    }
    BLayoutBuilder::Group<>(details, B_VERTICAL, 5)
        .Add(comparison[0]).Add(comparison[1]).Add(comparison[2])
        .Add(choices)
        .Add(results)
        .AddGroup(B_HORIZONTAL)
        .AddGlue()
        .Add(cancelAllButton)
        .End();
    details->Hide();
    detailsButton = new BButton("Details", new BMessage(kShowJobs));
    errorText = new BStringView("operation error", "");
    errorText->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
    errorText->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    errorText->SetTruncation(B_TRUNCATE_END);
    dismissError = new BButton("Dismiss error", new BMessage(kDismissError));
    errorRow = new BGroupView(B_HORIZONTAL, 8);
    errorRow->AddChild(errorText);
    errorRow->AddChild(dismissError);
    errorRow->Hide();
    BLayoutBuilder::Group<>(this, B_VERTICAL, 4)
        .SetInsets(8, 4, 8, 6)
        .AddGroup(B_HORIZONTAL, 8)
        .Add(title)
        .AddGlue()
        .Add(summary)
        .Add(pause)
        .Add(cancelButton)
        .Add(detailsButton)
        .End()
        .Add(file)
        .Add(bar)
        .Add(errorRow)
        .Add(details);
    SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
  }
  void AttachedToWindow() override {
    pause->SetTarget(this);
    dismissError->SetTarget(this);
    detailsButton->SetTarget(this);
    cancelButton->SetTarget(this);
    cancelAllButton->SetTarget(this);
    for (int32 i = 0; i < choices->CountChildren(); ++i)
      if (auto button = dynamic_cast<BButton *>(choices->ChildAt(i)))
        button->SetTarget(this);
  }
  bool compact = false, visible = true;
  void UpdateVisibility() {
    const bool show = !compact || expanded || choicesVisible || activeOperation ||
                      owner->Busy() || hasCompletion || !unreadErrors.empty();
    if (visible == show)
      return;
    visible = show;
    if (show)
      BView::Show();
    else
      BView::Hide();
  }
  void SetCompact(bool value) {
    compact = value;
    UpdateVisibility();
  }
  void Expand(bool show) {
    if (expanded == show) {
      UpdateVisibility();
      return;
    }
    expanded = show;
    if (show)
      details->Show();
    else
      details->Hide();
    detailsButton->SetLabel(show ? "Less" : "Details");
    UpdateVisibility();
  }
  void Toggle() { Expand(!expanded); }
  void Dismiss() {
    if (choicesVisible) {
      owner->Answer(Listi::Decision::Abort, conflictRequest);
      ShowChoices(false);
    }
    Expand(false);
  }
  void MessageReceived(BMessage *m) override {
    switch (m->what) {
    case kShowJobs:
      Toggle();
      break;
    case kFileErrorSelected:
      if (const char *error = m->GetString("error", nullptr)) {
        if (unreadErrors.empty() || unreadErrors.back() != error)
          results->ShowFileError(error);
        KeepError(error);
      }
      break;
    case kDismissError:
      if (!unreadErrors.empty())
        unreadErrors.pop_front();
      ShowNextError();
      break;
    case kPause: {
      const auto operation = m->GetInt64("operation", 0);
      if (operation == activeOperation && owner->Pause(operation))
        pause->SetLabel(!strcmp(pause->Label(), "Pause") ? "Resume" : "Pause");
      break;
    }
    case kAbort:
    case kCancelAll: {
      const auto operation = m->GetInt64("operation", 0);
      if (!operation || operation != activeOperation ||
          !(m->what == kAbort ? owner->CancelOperation(operation)
                              : owner->Cancel(operation)))
        break;
      cancelling = true;
      pause->SetLabel("Pause");
      pause->SetEnabled(false);
      cancelButton->SetEnabled(false);
      if (m->what == kCancelAll)
        cancelAllButton->SetEnabled(false);
      ShowChoices(false);
      summary->SetText("Cancelling. Finishing the current safe step...");
      break;
    }
    case kAnswer:
      if (owner->Answer(
              static_cast<Listi::Decision>(m->GetInt32("decision", 4)),
              m->GetInt64("request", -1)))
        ShowChoices(false);
      break;
    case kConflict:
      if (!activeOperation || cancelling ||
          m->GetInt64("operation", 0) != activeOperation)
        break;
      conflictRequest = m->GetInt64("request", -1);
      for (int i = 0; i < 3; ++i) {
        const char *line = nullptr;
        bool present = m->FindString("detail", i, &line) == B_OK;
        if (present) {
          comparison[i]->SetText(line);
          comparison[i]->SetToolTip(line);
        }
        Listi::SetViewVisible(comparison[i], present);
      }
      for (int32 i = 0; i < choices->CountChildren(); ++i) {
        if (auto button = dynamic_cast<BButton *>(choices->ChildAt(i))) {
          if (i == int(Listi::Decision::KeepBoth))
            Listi::SetViewVisible(button, m->HasString("detail"));
          auto answer = new BMessage(kAnswer);
          answer->AddInt32("decision", i);
          answer->AddInt64("request", conflictRequest);
          button->SetMessage(answer);
        }
      }
      Expand(true);
      summary->SetText("Destination exists. Choose how to continue:");
      file->SetText(m->GetString("path", ""));
      ShowChoices(true);
      break;
    case kJobAccepted:
      results->Accepted(m->GetInt64("operation", 0), m->GetString("title", ""));
      break;
    case kJobUpdate: {
      const auto operation = m->GetInt64("operation", 0);
      if (m->GetBool("start", false)) {
        if (operation <= lastStarted)
          break;
        lastStarted = activeOperation = operation;
        results->Started(operation);
        hasCompletion = false;
        cancelling = false;
        ShowChoices(false);
        for (auto *button : {pause, cancelButton, cancelAllButton}) {
          auto message = new BMessage(button->Message()->what);
          message->AddInt64("operation", operation);
          button->SetMessage(message);
          button->SetEnabled(true);
        }
        Listi::SetViewVisible(pause, true);
        Listi::SetViewVisible(cancelButton, true);
        Listi::SetViewVisible(file, true);
        Listi::SetViewVisible(bar, false);
        hasByteEstimate = false;
        bar->Reset();
        bar->SetMaxValue(1);
        pause->SetLabel("Pause");
        summary->SetText("Working. You can keep browsing.");
      }
      if (!activeOperation || operation != activeOperation)
        break;
      title->SetText(m->GetString("title", title->Text()));
      if (const char *path = m->GetString("path", nullptr)) {
        file->SetText(path);
        file->SetToolTip(path);
      }
      int64 total, bytes;
      if (m->FindInt64("total", &total) == B_OK) {
        hasByteEstimate = total > 0;
        Listi::SetViewVisible(bar, hasByteEstimate);
        bar->SetMaxValue(float(std::max<int64>(1, total)));
      }
      if (m->FindInt64("bytes", &bytes) == B_OK) {
        // Bytes can reach the estimate before publication or source cleanup.
        const float progress = std::min(float(bytes), 0.99f * bar->MaxValue());
        if (hasByteEstimate)
          bar->Update(progress - bar->CurrentValue());
        BString s;
        s << bytes << " bytes processed";
        if (hasByteEstimate)
          bar->SetToolTip(s);
        else
          file->SetToolTip(BString(file->Text()) << "\n" << s);
      }
      break;
    }
    case kJobDone: {
      BString s = m->GetString("result", "");
      const bool activeDone =
          activeOperation && m->GetInt64("operation", 0) == activeOperation;
      if (activeDone) {
        if (m->GetBool("complete", false))
          bar->Update(bar->MaxValue() - bar->CurrentValue());
        summary->SetText(s);
      }
      results->Finished(*m);
      if (activeDone) {
        ShowChoices(false);
        activeOperation = 0;
        pause->SetEnabled(false);
        cancelButton->SetEnabled(false);
        cancelAllButton->SetEnabled(false);
        Listi::SetViewVisible(pause, false);
        Listi::SetViewVisible(cancelButton, false);
        const char *transfer = m->GetString("transferResult", "");
        hasCompletion = *transfer != '\0';
        if (hasCompletion) {
          file->SetText(transfer);
          file->SetToolTip(transfer);
        }
        Listi::SetViewVisible(file, hasCompletion);
        Listi::SetViewVisible(bar, false);
      }
      if (!activeOperation && !owner->Busy())
        title->SetText("Queue finished");
      const char *error;
      for (int32 i = 0; m->FindString("error", i, &error) == B_OK; ++i)
        KeepError(error);
      if (m->HasString("retainedSource")) {
        BString retained("Complete copy retained at: ");
        retained << m->GetString("retainedDestination", "");
        KeepError(retained);
      }
      break;
    }
    case B_COLORS_UPDATED:
      bar->SetBarColor(ui_color(B_STATUS_BAR_COLOR));
      BView::MessageReceived(m);
      break;
    default:
      BView::MessageReceived(m);
      break;
    }
    UpdateVisibility();
  }
};
OperationQueue::OperationQueue(BMessenger m) : main(m) {
  view = new ProgressView(this);
}
BView *OperationQueue::View() { return view; }
void OperationQueue::Start() {
  display = BMessenger(view);
  BMessage poll(kQueueWake);
  eventTimer.reset(new BMessageRunner(main, &poll, 250000));
  if (eventTimer->InitCheck() != B_OK)
    throw std::runtime_error("Cannot start operation result delivery.");
  thread = std::thread([this] { Loop(); });
}
OperationQueue::~OperationQueue() {
  eventTimer.reset();
  std::deque<QueuedJob> discarded;
  {
    std::lock_guard<std::mutex> l(mutex);
    stopping = true;
    cancel = true;
    paused = false;
    discarded.swap(jobs);
    count -= discarded.size();
  }
  changed.notify_all();
  for (const auto &queued : discarded)
    if (queued.job.archivePasswordToken)
      Listi::DiscardArchiveCreationPassword(queued.job.archivePasswordToken);
  if (thread.joinable())
    thread.join();
}
void OperationQueue::SetCompact(bool value) { view->SetCompact(value); }
void OperationQueue::Show() { view->Expand(true); }
void OperationQueue::Toggle() { view->Toggle(); }
void OperationQueue::Collapse() { view->Dismiss(); }
void OperationQueue::ErrorInfo(const char *text) {
  BMessage error(kFileErrorSelected);
  error.AddString("error", text);
  display.SendMessage(&error);
}
int64 OperationQueue::Enqueue(const Listi::Job &j) {
  try {
    for (const auto &source : j.sources)
      if (Listi::IsVirtual(source))
        Listi::RequireConnection(source);
    if (Listi::IsVirtual(j.destination))
      Listi::RequireConnection(j.destination);
    if (Listi::IsVirtual(j.destinationFolder))
      Listi::RequireConnection(j.destinationFolder);
  } catch (const std::exception &error) {
    if (j.archivePasswordToken)
      Listi::DiscardArchiveCreationPassword(j.archivePasswordToken);
    ErrorInfo(error.what());
    return 0;
  }
  std::lock_guard<std::mutex> lock(mutex);
  return EnqueueLocked(j);
}
bool OperationQueue::CanUndoBulkRename() {
  std::lock_guard<std::mutex> lock(mutex);
  return !count && !stopping && lastBulkRename.has_value();
}
int64 OperationQueue::EnqueueBulkRenameUndo() {
  std::lock_guard<std::mutex> lock(mutex);
  if (count || stopping || !lastBulkRename)
    return 0;
  return EnqueueLocked(*lastBulkRename);
}
int64 OperationQueue::EnqueueLocked(const Listi::Job &j) {
  const int64 operation = ++operationSerial;
  // Publish acceptance under the same lock as insertion so a fast worker or
  // a cancellation can never present the terminal event before the job exists.
  BMessage accepted(kJobAccepted);
  accepted.AddInt64("operation", operation);
  std::string title = Verb(j.action);
  const auto leaf =
      Listi::Leaf(j.sources.empty() ? j.destination : j.sources.front());
  if (!leaf.empty()) {
    title += ": " + leaf;
    if (j.sources.size() > 1)
      title += " (+" + std::to_string(j.sources.size() - 1) + ")";
  }
  accepted.AddString("title", title.c_str());
  events.push_back(accepted);
  jobs.push_back({operation, j});
  count++;
  changed.notify_all();
  return operation;
}
bool OperationQueue::Pause(int64 operation) {
  {
    std::lock_guard<std::mutex> l(mutex);
    if (!operation || operation != runningOperation || cancel || stopping)
      return false;
    paused = !paused;
  }
  changed.notify_all();
  return true;
}
bool OperationQueue::CancelOperation(int64 operation) {
  return CancelOne(operation, false);
}
bool OperationQueue::CancelWaiting(int64 operation) {
  return CancelOne(operation, true);
}
bool OperationQueue::CancelOne(int64 operation, bool waitingOnly) {
  QueuedJob discarded;
  {
    std::lock_guard<std::mutex> l(mutex);
    if (!operation || stopping)
      return false;
    if (operation == runningOperation) {
      if (cancel || waitingOnly)
        return false;
      cancel = true;
      paused = false;
    } else {
      const auto next =
          std::find_if(jobs.begin(), jobs.end(), [&](const QueuedJob &queued) {
            return queued.operation == operation;
          });
      if (next == jobs.end())
        return false;
      discarded = std::move(*next);
      jobs.erase(next);
      --count;
    }
  }
  if (discarded.operation)
    PublishCancelled(discarded);
  else
    changed.notify_all();
  return true;
}
bool OperationQueue::Cancel(int64 expectedOperation) {
  std::deque<QueuedJob> discarded;
  {
    std::lock_guard<std::mutex> l(mutex);
    if (stopping ||
        (expectedOperation && expectedOperation != runningOperation) ||
        ((!runningOperation || cancel) && jobs.empty()))
      return false;
    cancel = true;
    paused = false;
    discarded.swap(jobs);
    count -= discarded.size();
  }
  changed.notify_all();
  for (const auto &queued : discarded)
    PublishCancelled(queued);
  return true;
}
void OperationQueue::PublishCancelled(const QueuedJob &queued) {
  if (queued.job.archivePasswordToken)
    Listi::DiscardArchiveCreationPassword(queued.job.archivePasswordToken);
  BMessage done(kJobDone);
  done.AddInt64("operation", queued.operation);
  done.AddInt64("tag", queued.job.tag);
  done.AddBool("complete", false);
  done.AddBool("cancelled", true);
  done.AddBool("beforeStart", true);
  done.AddBool("idle", count.load() == 0);
  BString summary;
  summary << Verb(queued.job.action) << ": Cancelled before starting.";
  done.AddString("result", summary);
  auto details = std::make_shared<Listi::JobReportStore>();
  for (const auto &path : queued.job.sources)
    details->WriteOutcome({path, "", "unprocessed", ""}, false);
  if (queued.job.sources.empty() && !queued.job.destination.empty())
    details->WriteOutcome({"", queued.job.destination, "unprocessed", ""}, false);
  Finish(done, std::move(details));
}
void OperationQueue::Finish(
    BMessage &done, std::shared_ptr<Listi::JobReportStore> details) {
  details->Seal();
  std::string reportSummary = done.GetString("result", "");
  const char *source, *destination;
  if (done.FindString("retainedSource", &source) == B_OK &&
      done.FindString("retainedDestination", &destination) == B_OK)
    reportSummary += std::string("\nComplete copy retained at: ") + destination +
                     "\nSource left intact: " + source;
  auto report = std::make_shared<JobReport>(
      JobReport{std::move(reportSummary), std::move(details)});
  {
    std::lock_guard<std::mutex> lock(mutex);
    reports.emplace(done.GetInt64("operation", 0), std::move(report));
    while (reports.size() > kFinishedReportLimit)
      reports.erase(reports.begin());
  }
  Publish(done);
}
std::shared_ptr<const OperationQueue::JobReport>
OperationQueue::Report(int64 id) {
  std::lock_guard<std::mutex> lock(mutex);
  auto found = reports.find(id);
  return found == reports.end() ? nullptr : found->second;
}
void OperationQueue::ForgetReports(const std::vector<int64> &presented) {
  std::lock_guard<std::mutex> lock(mutex);
  for (auto id : presented)
    reports.erase(id);
}
void OperationQueue::Publish(const BMessage &done) {
  {
    std::lock_guard<std::mutex> lock(mutex);
    events.push_back(done);
  }
  BMessage wake(kQueueWake);
  main.SendMessage(&wake, (BHandler *)nullptr, 100000);
}
std::vector<BMessage> OperationQueue::TakeEvents() {
  std::lock_guard<std::mutex> lock(mutex);
  std::vector<BMessage> out;
  out.swap(events);
  return out;
}
void OperationQueue::PresentEvent(BMessage &done) {
  // Called on the view's owning window thread, never by the worker.
  view->MessageReceived(&done);
}
bool OperationQueue::Answer(Listi::Decision d, int64 request) {
  {
    std::lock_guard<std::mutex> l(mutex);
    if (cancel || stopping || !awaitingAnswer || request != conflictSerial ||
        answer >= 0 || int(d) < int(Listi::Decision::Replace) ||
        int(d) > int(allowKeepBoth ? Listi::Decision::KeepBoth : Listi::Decision::Abort))
      return false;
    answer = int(d);
  }
  changed.notify_all();
  return true;
}
bool OperationQueue::Proceed() {
  std::unique_lock<std::mutex> l(mutex);
  changed.wait(l, [this] { return !paused || cancel || stopping; });
  return !cancel && !stopping;
}
Listi::Decision OperationQueue::Ask(const std::string &path, const Listi::CopyConflict *copy) {
  int64 request, operation;
  {
    std::lock_guard<std::mutex> l(mutex);
    if (cancel || stopping)
      return Listi::Decision::Abort;
    answer = -1;
    request = ++conflictSerial;
    operation = runningOperation;
    allowKeepBoth = copy != nullptr;
    awaitingAnswer = true;
  }
  BMessage m(kConflict);
  m.AddInt64("operation", operation);
  m.AddInt64("request", request);
  m.AddString("path", path.c_str());
  if (copy) {
    m.AddString("detail", ("Incoming: " + DescribeFile(copy->source, copy->sourceSize,
                                                       copy->sourceModified)).c_str());
    m.AddString("detail", ("Existing: " + DescribeFile(path, copy->destinationSize,
                                                       copy->destinationModified)).c_str());
    m.AddString("detail", ("Keep both as: " + copy->proposed).c_str());
  }
  Publish(m);
  std::unique_lock<std::mutex> l(mutex);
  changed.wait(l, [this] { return answer >= 0 || cancel || stopping; });
  awaitingAnswer = false;
  return cancel || stopping ? Listi::Decision::Abort
                            : static_cast<Listi::Decision>(answer);
}
void OperationQueue::Loop() {
  for (;;) {
    QueuedJob queued;
    {
      std::unique_lock<std::mutex> l(mutex);
      changed.wait(l, [this] { return stopping || !jobs.empty(); });
      if (stopping)
        return;
      queued = std::move(jobs.front());
      jobs.pop_front();
      runningOperation = queued.operation;
      cancel = false;
      paused = false;
    }
    Listi::ArchiveListingScope listingScope;
    const auto &job = queued.job;
    const int64 operation = queued.operation;
    BString title;
    title << Verb(job.action) << ": " << int32(job.sources.size())
          << " selected entries";
    BMessage start(kJobUpdate);
    start.AddInt64("operation", operation);
    start.AddString("title", title);
    start.AddString("path", job.sources.empty() ? job.destination.c_str()
                                                : job.sources[0].c_str());
    start.AddBool("start", true);
    Publish(start);
    uint64_t total = 0;
    if (job.action == Listi::Action::Copy ||
        job.action == Listi::Action::Move ||
        job.action == Listi::Action::Duplicate ||
        job.action == Listi::Action::CreateArchive ||
        job.action == Listi::Action::EditArchive ||
        job.action == Listi::Action::MoveArchive)
      total = EstimateBytes(job.sources, [this] { return Proceed(); });
    BMessage size(kJobUpdate);
    size.AddInt64("operation", operation);
    size.AddInt64("total", total);
    Publish(size);
    std::string activePath;
    uint64_t fileBytes = 0, fileSize = 0;
    uint64_t bytes = 0;
    bigtime_t last = 0;
    Listi::Hooks hooks;
    hooks.report = std::make_shared<Listi::JobReportStore>();
    hooks.proceed = [this] { return Proceed(); };
    if (job.action == Listi::Action::Copy && !Listi::IsVirtual(job.destination) &&
        std::none_of(job.sources.begin(), job.sources.end(),
                     [](const auto &path) { return Listi::IsVirtual(path); }))
      hooks.copyConflict = [this](const auto &c) { return Ask(c.destination, &c); };
    hooks.conflict = [this](auto &p) { return Ask(p); };
    hooks.fileSize = [&](const std::string &path, uint64_t size) {
      activePath = path;
      fileSize = size;
      fileBytes = 0;
    };
    hooks.progress = [&](auto &p, uint64_t n) {
      bytes += n;
      if (p != activePath) {
        activePath = p;
        fileBytes = 0;
        fileSize = 0;
      }
      fileBytes += n;
      if (system_time() - last < 80000)
        return;
      last = system_time();
      BMessage m(kJobUpdate);
      m.AddInt64("operation", operation);
      m.AddString("path", p.c_str());
      m.AddInt64("bytes", bytes);
      display.SendMessage(&m, (BHandler *)nullptr, 500000);
      BMessage file(kFileProgress);
      file.AddString("path", p.c_str());
      file.AddInt32("state", 1);
      file.AddFloat("fraction",
                    fileSize
                        ? std::min(0.99f, float(fileBytes) / fileSize)
                        : 0.f);
      main.SendMessage(&file, (BHandler *)nullptr, 500000);
    };
    const bool transfer = job.action == Listi::Action::Copy ||
                          job.action == Listi::Action::Move ||
                          job.action == Listi::Action::MoveArchive ||
                          job.action == Listi::Action::EditArchive;
    std::set<std::string> transferSources;
    if (transfer)
      transferSources.insert(job.sources.begin(), job.sources.end());
    std::pair<std::string, std::string> firstTransfer;
    hooks.fileDone = [&](const std::string &path,
                         const std::string &destination) {
      if (transfer && firstTransfer.first.empty() &&
          transferSources.count(path))
        firstTransfer = {path, destination};
      BMessage file(kFileProgress);
      file.AddString("path", path.c_str());
      file.AddString("destination", destination.c_str());
      file.AddInt32("state", 2);
      file.AddFloat("fraction", 1.f);
      main.SendMessage(&file, (BHandler *)nullptr, 500000);
    };
    hooks.fileFailed = [&](const std::string &path, const std::string &error) {
      BMessage file(kFileProgress);
      file.AddString("path", path.c_str());
      file.AddString("error", error.c_str());
      file.AddInt32("state", 3);
      main.SendMessage(&file, (BHandler *)nullptr, 500000);
    };
    auto result = Listi::Run(job, hooks);
    auto &details = *hooks.report;
    for (const auto &error : result.errors)
      details.AddError(error);
    for (const auto &row : result.outcomes)
      details.WriteOutcome(row, false);
    result.errors.clear();
    result.outcomes.clear();
    const bool renameBatch = job.action == Listi::Action::BulkRename;
    const bool undoBatch = job.action == Listi::Action::UndoBulkRename;
    const bool undoAvailable = result.bulkRenameUndo.has_value();
    {
      std::lock_guard<std::mutex> l(mutex);
      if (renameBatch && result.completed)
        lastBulkRename = std::move(result.bulkRenameUndo);
      if ((undoBatch && result.completed) || result.bulkRenameRecoveryNeeded)
        lastBulkRename.reset();
      result.bulkRenameUndo.reset();
      runningOperation = 0;
      --count;
    }
    if (job.archivePasswordToken)
      Listi::DiscardArchiveCreationPassword(job.archivePasswordToken);
    BMessage done(kJobDone);
    done.AddInt64("operation", operation);
    done.AddInt64("tag", job.tag);
    if (job.action == Listi::Action::Move ||
        job.action == Listi::Action::Duplicate ||
        job.action == Listi::Action::CreateArchive ||
        job.action == Listi::Action::EditArchive ||
        job.action == Listi::Action::MoveArchive)
      for (const auto &root : result.completedRoots)
        done.AddString("moved", root.first.c_str());
    BString summary;
    summary << Verb(job.action) << ": " << result.completed << " completed, "
            << result.skipped << " skipped, " << int32(details.ErrorCount())
            << " errors";
    if (result.bulkRenameRecoveryNeeded)
      summary << ". Check entry locations in the operation report.";
    else if (result.cancelled)
      summary << (renameBatch || undoBatch ? ". Cancelled; originals retained."
                                           : ". Cancelled; completed entries kept.");
    else if (renameBatch && result.completed && !undoAvailable)
      summary << ". Undo unavailable: contents could not be verified.";
    if (result.recoverableMoves)
      summary << ". Cross-volume originals kept in source Trash.";
    if (details.Incomplete())
      summary << ". Job report storage unavailable; some details may be missing.";
    done.AddString("result", summary);
    if (!result.interruptedCopyFolder.empty())
      done.AddString("interruptedCopyFolder", result.interruptedCopyFolder.c_str());
    if (transfer) {
      const auto &entry = result.completedRoots.empty()
                              ? firstTransfer : result.completedRoots.front();
      if (!entry.first.empty() && !entry.second.empty()) {
        BString transferResult;
        transferResult << Listi::LocationLeaf(entry.first).c_str();
        if (result.completed > 1)
          transferResult << " (+" << result.completed - 1 << ")";
        transferResult << " → " << entry.second.c_str();
        done.AddString("transferResult", transferResult);
      }
    }
    done.AddBool("complete", !result.cancelled && !details.ErrorCount() &&
                                 !details.Incomplete() &&
                                 result.skipped == 0);
    done.AddBool("cancelled", result.cancelled);
    if (result.retainedMoveCopy) {
      const auto &[source, destination] = *result.retainedMoveCopy;
      done.AddString("retainedSource", source.c_str());
      done.AddString("retainedDestination", destination.c_str());
    }
    const auto sample = details.Page(0, 4);
    for (const auto &error : sample.errors)
      done.AddString("error", error.c_str());
    if (details.ErrorCount() > sample.errors.size() || details.Incomplete())
      done.AddString("error", "See the job report for available details.");
    done.AddBool("idle", count.load() == 0);
    Finish(done, std::move(hooks.report));
  }
}
