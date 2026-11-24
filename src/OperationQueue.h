// SPDX-License-Identifier: MIT
#pragma once
#include "Common.h"
#include "FileOperation.h"
#include "JobReportStore.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
class BView;
class BMessageRunner;
class OperationQueue {
public:
  explicit OperationQueue(BMessenger main);
  ~OperationQueue();
  int64 Enqueue(const Listi::Job &);
  bool CanUndoBulkRename();
  int64 EnqueueBulkRenameUndo();
  void SetCompact(bool);
  void Show();
  void Toggle();
  void Collapse();
  void ErrorInfo(const char *);
  BView *View();
  void Start();
  // Critical state is retained until the owning UI consumes it. Notifications are
  // only wakeups; a lost message must not strand a clipboard lease.
  std::vector<BMessage> TakeEvents();
  void PresentEvent(BMessage &);
  bool Busy() const { return count.load() > 0; }
  bool Pause(int64 operation);
  bool CancelOperation(int64 operation);
  bool CancelWaiting(int64 operation);
  struct JobReport {
    std::string summary;
    std::shared_ptr<const Listi::JobReportStore> details;
  };
  static constexpr size_t kFinishedReportLimit = 24;
  // The newest finished reports by job ID, until cleared or the window closes.
  std::shared_ptr<const JobReport> Report(int64 operation);
  void ForgetReports(const std::vector<int64> &presented);
  // With an ID, cancel all only if that job is still running.
  bool Cancel(int64 expectedOperation = 0);
  bool Answer(Listi::Decision, int64 request);

private:
  struct QueuedJob {
    int64 operation = 0;
    Listi::Job job;
  };
  int64 EnqueueLocked(const Listi::Job &);
  void Loop();
  void Publish(const BMessage &);
  void Finish(BMessage &, std::shared_ptr<Listi::JobReportStore>);
  bool CancelOne(int64 operation, bool waitingOnly);
  void PublishCancelled(const QueuedJob &);
  bool Proceed();
  Listi::Decision Ask(const std::string &, const Listi::CopyConflict * = nullptr);
  class ProgressView *view;
  BMessenger main, display;
  std::thread thread;
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<QueuedJob> jobs;
  std::optional<Listi::Job> lastBulkRename;
  std::vector<BMessage> events;
  std::map<int64, std::shared_ptr<const JobReport>> reports;
  std::unique_ptr<BMessageRunner> eventTimer;
  std::atomic<int> count{0};
  bool paused = false, cancel = false, stopping = false;
  int answer = -1;
  int64 conflictSerial = 0, operationSerial = 0;
  int64 runningOperation = 0;
  bool allowKeepBoth = false;
  bool awaitingAnswer = false;
};
