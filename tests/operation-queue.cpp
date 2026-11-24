// SPDX-License-Identifier: MIT
#include "OperationQueue.h"
#include "ArchiveJobs.h"
#include "ArchivePasswords.h"
#include "BulkRename.h"
#include "JobSnapshot.h"
#include <Application.h>
#include <Button.h>
#include <CheckBox.h>
#include <Window.h>
#include <StringView.h>
#include <StatusBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Menu.h>
#include <TextView.h>
#include <LayoutBuilder.h>
#include <Directory.h>
#include <Entry.h>
#include <Path.h>
#include <atomic>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
using namespace Listi;
std::atomic<int> failedWakes{0};
std::atomic<int> rootDirectoryOpens{0};
std::string measuredFolder;
extern "C" DIR *__real_opendir(const char *);
extern "C" DIR *__wrap_opendir(const char *path) {
  if (measuredFolder == path)
    ++rootDirectoryOpens;
  return __real_opendir(path);
}
extern "C" status_t __real__ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl(const BMessenger*,BMessage*,BHandler*,bigtime_t);
extern "C" status_t __wrap__ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl(const BMessenger* self,BMessage* m,BHandler* reply,bigtime_t timeout){
 if(m->what==kQueueWake&&failedWakes.load()>0){--failedWakes;return B_TIMED_OUT;}
 return __real__ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl(self,m,reply,timeout);
}
class QueueWindow:public BWindow {
public:
 OperationQueue *queue=nullptr;bool hold=true;std::vector<BMessage> completed;
 std::vector<BMessage> conflicts;
 QueueWindow():BWindow(BRect(10,10,910,300),"Queue regression",B_TITLED_WINDOW,0){
  queue=new OperationQueue(BMessenger(this));BLayoutBuilder::Group<>(this,B_VERTICAL).Add(queue->View());queue->Start();
 }
 ~QueueWindow(){delete queue;}
 void MessageReceived(BMessage*m) override {
  if(m->what==kQueueWake){
   if(hold)return;
   for(auto &event:queue->TakeEvents()){
    queue->PresentEvent(event);
    if(event.what==kJobDone)completed.push_back(event);
    if(event.what==kConflict)conflicts.push_back(event);
   }
  }else BWindow::MessageReceived(m);
 }
};
void clean(const std::string&p){BDirectory d(p.c_str());BEntry e;while(d.GetNextEntry(&e,false)==B_OK){struct stat s;e.GetStat(&s);if(S_ISDIR(s.st_mode)){BPath path;e.GetPath(&path);clean(path.Path());}e.Remove();}rmdir(p.c_str());}
std::string bytes(const std::string&p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
int main(){
 BApplication app("application/x-vnd.Listi-QueueRegression");char temp[]="/tmp/Listi-queue-XXXXXX";if(!mkdtemp(temp))return 2;std::string root=temp;
 mkdir((root+"/in").c_str(),0700);mkdir((root+"/out").c_str(),0700);
 int checks=0,failures=0;auto check=[&](bool ok,const char*label){++checks;failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<label<<std::endl;};
 auto window=new QueueWindow;window->Show();
 auto wait=[&](const std::function<bool()> &f){for(int i=0;i<400;++i){window->Lock();bool ok=f();window->Unlock();if(ok)return true;snooze(25000);}return false;};
 auto job=[&](int n,int64 tag){auto p=root+"/in/item"+std::to_string(n);Job j{Action::Copy,{p},root+"/out"};FreezeLocalJob(j,j.destination);j.tag=tag;return j;};
 auto archiveJob = [&](const char *leaf, int64 tag) {
   auto entry = PrepareArchiveJob(false, {root + "/in/item0"});
   SetArchiveDestination(entry, root + "/out", leaf);
   entry.archivePasswordToken =
       HoldArchiveCreationPassword(std::make_shared<ArchiveSecret>("fixture"));
   entry.tag = tag;
   return entry;
 };
 auto passwordDiscarded = [](uint64_t token) {
   try {
     TakeArchiveCreationPassword(token);
   } catch (const Cancelled &) {
     return true;
   }
   return false;
 };
 // Called with the window locked, as are all reads from its event vectors.
 auto finished = [&](int64 operation, int64 tag, bool complete, bool cancelled,
                     bool beforeStart) {
   int matches = 0;
   for (const auto &event : window->completed) {
     if (event.GetInt64("operation", 0) != operation)
       continue;
     if (event.GetInt64("tag", -1) != tag ||
         event.GetBool("complete", !complete) != complete ||
         event.GetBool("cancelled", !cancelled) != cancelled ||
         event.GetBool("beforeStart", false) != beforeStart)
       return false;
     ++matches;
   }
   return matches == 1;
 };
 failedWakes=100;
 std::vector<int64> ids;
 for (int i = 0; i < 20; ++i) {
   std::ofstream(root + "/in/item" + std::to_string(i))
       << std::string(32768, char('A' + i));
   ids.push_back(window->queue->Enqueue(job(i, 1000 + i)));
 }
 check(wait([&]{return !window->queue->Busy();}),"20 jobs finish despite failed wakeups and held UI consumption");
 window->Lock();check(window->completed.empty(),"held UI has not consumed terminal state");window->hold=false;window->Unlock();
 check(wait([&]{return window->completed.size()==20;}),"periodic delivery recovers all retained completions");
 window->Lock();
 bool successful = true;
 for (size_t i = 0; i < ids.size(); ++i)
   successful &= ids[i] > 0 && finished(ids[i], 1000 + i, true, false, false);
 check(successful && std::set<int64>(ids.begin(), ids.end()).size() == ids.size(),
       "enqueue IDs remain unique and independent of tags at completion");
 auto *compactTitle = dynamic_cast<BStringView *>(window->FindView("operation"));
 auto *compactPath = dynamic_cast<BStringView *>(window->FindView("current-file"));
 auto *compactSummary = dynamic_cast<BStringView *>(window->FindView("summary"));
 check(compactTitle && std::string(compactTitle->Text()).find("Copied item19 to ") == 0 &&
           compactPath && compactPath->IsHidden() && compactSummary && compactSummary->IsHidden(),
       "ordinary success is one concise line with the destination");
 window->Unlock();
 bool intact=true;for(int i=0;i<20;++i)intact&=bytes(root+"/out/item"+std::to_string(i))==std::string(32768,char('A'+i));check(intact,"all 20 destinations contain exact payloads");

 measuredFolder = root + "/in/tree";
 mkdir(measuredFolder.c_str(), 0700);
 std::ofstream(measuredFolder + "/nested") << "entry";
 Job folderJob{Action::Copy, {measuredFolder}, root + "/out"};
 FreezeLocalJob(folderJob, folderJob.destination);
 const auto started = system_time();
 window->queue->Enqueue(folderJob);
 const bool folderFinished = wait([&]{return !window->queue->Busy();});
 std::cout << "Folder copy: " << rootDirectoryOpens.load() << " root opens in "
           << system_time() - started << " us including test polling\n";
 check(folderFinished && rootDirectoryOpens == 1 &&
           bytes(root + "/out/tree/nested") == "entry",
       "folder copy opens its tree once, without a progress pre-scan");
 std::ofstream(root + "/in/item0") << "must not replace the destination";
 std::ofstream(root + "/in/item20") << "following job";
 const auto active = window->queue->Enqueue(job(0, 101));
 if (!wait([&] { return window->conflicts.size() == 1; }))
   return 2;
 window->Lock();
 auto pause = dynamic_cast<BButton *>(window->FindView("pause"));
 auto cancelJob = dynamic_cast<BButton *>(window->FindView("cancel-job"));
 auto cancelAll = dynamic_cast<BButton *>(window->FindView("cancel-all"));
 auto summary = dynamic_cast<BStringView *>(window->FindView("summary"));
 if (!pause || !cancelJob || !cancelAll || !summary) {
   window->Quit();
   clean(root);
   return 2;
 }
 auto *repeat = dynamic_cast<BCheckBox *>(window->FindView("Repeat Replace or Skip for remaining conflicts"));
 check(repeat && repeat->Value() == B_CONTROL_OFF &&
           window->FindView("Replace") && window->FindView("Skip") && window->FindView("Keep both") &&
           !window->FindView("Replace all") && !window->FindView("Skip all"),
       "single-file conflict choices do not implicitly apply to a batch");
 BMessage oldConflict = window->conflicts.back();
 const char *incoming = nullptr, *existing = nullptr, *proposed = nullptr;
 check(oldConflict.GetInt64("request", 0) > 0 &&
           oldConflict.FindString("detail", 0, &incoming) == B_OK &&
           oldConflict.FindString("detail", 1, &existing) == B_OK &&
           oldConflict.FindString("detail", 2, &proposed) == B_OK &&
           std::string(incoming).find("/in/item0") != std::string::npos &&
           std::string(existing).find("32768 bytes, modified") != std::string::npos &&
           std::string(proposed).find("/out/item0 (2)") != std::string::npos,
       "local file conflict delivers worker metadata and proposed name with request ID");
 BMessage oldPause(*pause->Message()), oldCancel(*cancelJob->Message()),
     oldCancelAll(*cancelAll->Message());
 const std::string conflictText = summary->Text();
 check(oldConflict.GetInt64("operation", 0) == active &&
           oldPause.GetInt64("operation", 0) == active &&
           oldCancel.GetInt64("operation", 0) == active &&
           oldCancelAll.GetInt64("operation", 0) == active &&
           conflictText.find("Destination exists") != std::string::npos,
       "active conflict and native controls carry the enqueue ID");
 window->Unlock();

 auto removed = archiveJob("targeted.zip", 102);
 for (int i = 1; i <= 50; ++i)
   removed.sources.push_back(root + "/in/pending" + std::to_string(i));
 const auto pending = window->queue->Enqueue(removed);
 const auto following = window->queue->Enqueue(job(20, 103));
 window->Lock();
 auto picker = dynamic_cast<BMenuField *>(window->FindView("job-picker"));
 auto historyView = dynamic_cast<BTextView *>(window->FindView("history"));
 auto cancelWaiting = dynamic_cast<BButton *>(window->FindView("cancel-waiting"));
 auto nextPage = dynamic_cast<BButton *>(window->FindView("next-results"));
 auto clearReports = dynamic_cast<BButton *>(window->FindView("clear-finished"));
 window->Unlock();
 // Called under the window lock. Use the same messages as keyboard or mouse input.
 auto select = [&](int64 id) {
   if (!picker)
     return false;
   for (int32 i = 0; i < picker->Menu()->CountItems(); ++i) {
     auto item = picker->Menu()->ItemAt(i);
     if (item->Message() && item->Message()->GetInt64("operation", 0) == id)
       return item->Messenger().SendMessage(item->Message()) == B_OK;
   }
   return false;
 };
 auto reportText = [&] { return std::string(historyView ? historyView->Text() : ""); };
 check(wait([&] { return select(pending); }) &&
           wait([&] { return cancelWaiting && cancelWaiting->IsEnabled() &&
                              cancelWaiting->Message()->GetInt64("operation", 0) == pending; }),
       "a waiting job can be selected by its accepted ID in the job picker");
 window->Lock();
 if (!cancelWaiting || !clearReports || !nextPage || !historyView)
   return 2;
 cancelWaiting->Invoke();
 window->Unlock();
 check(wait([&] { return finished(pending, 102, false, true, true); }) &&
           !window->queue->CancelOperation(pending) &&
           passwordDiscarded(removed.archivePasswordToken),
       "Cancel selected retains the waiting job's identity and releases its password");
 auto pendingReport = window->queue->Report(pending);
 const auto finalPage = pendingReport ? pendingReport->details->Page(50, 50)
                                      : Listi::ReportPage{};
 check(finalPage.total == 51 && finalPage.outcomes.size() == 1 &&
           finalPage.outcomes.front().source == removed.sources[50] &&
           finalPage.outcomes.front().status == "unprocessed",
       "a pending cancellation retains all untouched entries independently of the UI");
 window->Lock();
 check(nextPage->IsEnabled() && reportText().find("Showing 1 to 50 of 51") != std::string::npos &&
           reportText().find("Not processed: " + removed.sources[0]) != std::string::npos,
       "selected job displays unprocessed paths and pages large results");
 nextPage->Invoke();
 window->Unlock();
 check(wait([&] { return reportText().find("Not processed: " + removed.sources[50]) != std::string::npos; }),
       "last page exposes the final entry rather than truncating history");
 window->Lock();
 check(pause->IsEnabled() && cancelJob->IsEnabled() &&
           conflictText == summary->Text(),
       "pending cancellation leaves the displayed active conflict alone");
 window->queue->View()->MessageReceived(&oldPause);
 check(std::string(pause->Label()) == "Resume", "active job can be paused");
 window->queue->View()->MessageReceived(&oldCancel);
 check(!window->queue->CancelOperation(active) &&
           !window->queue->Answer(Decision::Replace,
                              oldConflict.GetInt64("request", 0)),
       "Cancel job unblocks a paused conflict without accepting an answer");
 window->Unlock();
 check(wait([&] {
         return finished(active, 101, false, true, false) &&
                finished(following, 103, true, false, false);
       }) && bytes(root + "/out/item20") == "following job" &&
           bytes(root + "/out/item0") == std::string(32768, 'A'),
       "Cancel job keeps the following job and does not replace the conflict");

 const auto batchActive = window->queue->Enqueue(job(0, 201));
 if (!wait([&] { return window->conflicts.size() == 2; }))
   return 2;
 const auto tagged = window->queue->Enqueue(job(1, 202));
 auto untaggedJob = archiveJob("batch.zip", 0);
 const auto untagged = window->queue->Enqueue(untaggedJob);
 window->Lock();
 BMessage stale(kJobUpdate);
 stale.AddInt64("operation", active);
 stale.AddBool("start", true);
 stale.AddString("title", "STALE JOB");
 window->queue->PresentEvent(stale);
 window->queue->PresentEvent(oldConflict);
 window->queue->View()->MessageReceived(&oldPause);
 window->queue->View()->MessageReceived(&oldCancel);
 window->queue->View()->MessageReceived(&oldCancelAll);
 check(cancelJob->Message()->GetInt64("operation", 0) == batchActive &&
           pause->IsEnabled() && std::string(pause->Label()) == "Pause" &&
           conflictText == summary->Text() &&
           !window->queue->CancelOperation(active) &&
           !window->queue->Cancel(active) &&
           !window->queue->Pause(active) &&
           !window->queue->Answer(Decision::Abort,
                                  oldConflict.GetInt64("request", 0)),
       "delayed controls, progress and choices cannot affect the newer job");
 auto progress = dynamic_cast<BStatusBar *>(window->FindView("progress"));
 BMessage estimate(kJobUpdate);
 estimate.AddInt64("operation", batchActive);
 estimate.AddInt64("total", 100);
 window->queue->PresentEvent(estimate);
 BMessage transferred(kJobUpdate);
 transferred.AddInt64("operation", batchActive);
 transferred.AddInt64("bytes", 110);
 window->queue->PresentEvent(transferred);
 check(progress && progress->CurrentValue() > 0 &&
           progress->CurrentValue() < progress->MaxValue(),
       "transferring all estimated bytes does not imply the job committed");
 BMessage pauseBatch(*pause->Message());
 BMessage cancelBatch(*cancelAll->Message());
 window->queue->View()->MessageReceived(&pauseBatch);
 window->queue->View()->MessageReceived(&cancelBatch);
 check(!window->queue->Cancel(batchActive), "Cancel all rejects a repeat");
 window->Unlock();
 check(wait([&] {
         return !window->queue->Busy() &&
                finished(batchActive, 201, false, true, false) &&
                finished(tagged, 202, false, true, true) &&
                finished(untagged, 0, false, true, true);
       }) && passwordDiscarded(untaggedJob.archivePasswordToken),
       "Cancel all releases a paused conflict and tagged and untagged jobs");

 const auto fresh = window->queue->Enqueue(job(0, 301));
 if (!wait([&] { return window->conflicts.size() == 3; }))
   return 2;
 window->Lock();
 const auto request = window->conflicts.back().GetInt64("request", 0);
 window->Unlock();
 check(window->queue->Answer(Decision::Skip, request) &&
           !window->queue->Answer(Decision::Replace, request),
       "new conflict accepts exactly one answer after cancellation");
 check(wait([&] { return finished(fresh, 301, false, false, false); }),
       "fresh skipped job reaches terminal state");

 const auto source = root + "/in/item0";
 const auto destination = root + "/out/item0";
 BMessage retained(kJobDone);
 retained.AddString("result", "Move: 0 completed, 0 skipped, 0 errors. Cancelled;");
 retained.AddString("retainedSource", source.c_str());
 retained.AddString("retainedDestination", destination.c_str());
 window->Lock();
 check(reportText().find("Not processed: " + removed.sources[50]) != std::string::npos,
       "later completions do not replace the selected older report");
 select(0);
 window->Unlock();
 check(wait([&] { return reportText().find("Copy:") != std::string::npos; }),
       "recent activity can be reopened");
 window->Lock();
 auto *notice = dynamic_cast<BStringView *>(window->FindView("operation error"));
 auto *dismiss = notice ? dynamic_cast<BButton *>(notice->NextSibling()) : nullptr;
 BMessage error(kFileErrorSelected);
 error.AddString("error", "Unread transfer error");
 window->queue->View()->MessageReceived(&error);
 window->queue->PresentEvent(retained);
 check(notice && !notice->Parent()->IsHidden() &&
           std::string(notice->Text()) == "Error: Unread transfer error (+1)" && dismiss,
       "later completion keeps unread transfer error visible");
 if (dismiss) {
   window->queue->View()->MessageReceived(dismiss->Message());
   check(std::string(notice->Text()).find(destination) != std::string::npos,
         "next unread notice identifies the retained copy");
   window->queue->View()->MessageReceived(dismiss->Message());
   check(notice && notice->Parent()->IsHidden(), "acknowledgment hides read error");
 }
 const std::string history = historyView ? historyView->Text() : "";
 check(history.find("0 errors. Cancelled") != std::string::npos &&
           history.find("Complete copy kept at: " + destination) !=
               std::string::npos &&
           history.find("Listi did not remove anything from the source: " +
                        source) != std::string::npos,
       "cancelled move history reports both retained paths without an error");
 window->Unlock();

 const auto renameFolder = root + "/in";
 const auto one = renameFolder + "/item2", two = renameFolder + "/item3";
 auto swap = FreezeBulkRename(renameFolder, {one, two});
 swap.bulkNames = {{one, Leaf(two)}, {two, Leaf(one)}};
 swap.tag = 350;
 check(!window->queue->CanUndoBulkRename(),
       "undo is unavailable before any completed batch");
 const auto swapped = window->queue->Enqueue(swap);
 check(wait([&] { return finished(swapped, 350, true, false, false); }) &&
           window->queue->CanUndoBulkRename() &&
           bytes(one) == std::string(32768, 'D'),
       "the actual worker retains guarded undo after a successful swap");
 const auto busy = window->queue->Enqueue(job(0, 351));
 if (!wait([&] { return window->conflicts.size() == 4; }))
   return 2;
 check(!window->queue->CanUndoBulkRename() &&
           !window->queue->EnqueueBulkRenameUndo() &&
           window->queue->CancelOperation(busy) &&
           wait([&] { return !window->queue->Busy(); }),
       "undo cannot interleave with earlier active operations");
 const auto restored = window->queue->EnqueueBulkRenameUndo();
 check(restored && !window->queue->EnqueueBulkRenameUndo() &&
           wait([&] { return finished(restored, 0, true, false, false); }) &&
           !window->queue->CanUndoBulkRename() &&
           bytes(one) == std::string(32768, 'C') &&
           bytes(two) == std::string(32768, 'D'),
       "queued undo consumes the last batch exactly once and restores both names");

 // Failures and successful rows must both remain accessible past page one.
 const auto reportFolder = root + "/in/report-tree";
 mkdir(reportFolder.c_str(), 0700);
 for (int i = 0; i < 105; ++i)
   std::ofstream(reportFolder + "/leaf" + std::to_string(i)) << i;
 Job paged{Action::Copy, {reportFolder}, root + "/out"};
 for (int i = 0; i < 55; ++i)
   paged.sources.push_back(root + "/in/missing" + std::to_string(i));
 const auto pagedId = window->queue->Enqueue(paged);
 check(wait([&] { return finished(pagedId, 0, false, false, false); }),
       "file tree and missing selections finish in one actual queued job");
 if (const auto report = window->queue->Report(pagedId)) {
   std::set<std::string> files, failuresSeen;
   size_t errorsSeen = 0;
   bool completePages = true;
   for (size_t first = 0; first < 216; first += 50) {
     const auto page = report->details->Page(first, 50);
     completePages &= !page.incomplete && page.total == 216;
     errorsSeen += page.errors.size();
     for (const auto &row : page.outcomes) {
       if (row.status == "copied" && row.source != reportFolder &&
           bytes(row.destination) == Leaf(row.source).substr(4))
         files.insert(row.source);
       if (row.status == "failed" && !row.detail.empty())
         failuresSeen.insert(row.source);
     }
   }
   check(completePages && errorsSeen == 55 && files.size() == 105 &&
             failuresSeen.size() == 55,
         "disk pages preserve every copied result and missing-file error without duplicates");
 } else {
   check(false, "mixed job report is available");
 }

 Listi::JobReportStore receipts;
 EntryOutcome upload{"source", "destination", "outcome unknown", "Transfer interrupted"};
 upload.upload.emplace();
 upload.upload->publication = Publication::Unknown;
 upload.upload->recoveryPaths = {"staged path", "backup path"};
 receipts.WriteOutcome(upload, false);
 receipts.Seal();
 const auto receiptPage = receipts.Page(0, 50);
 check(receiptPage.outcomes.size() == 1 &&
           receiptPage.outcomes[0].status == "outcome unknown" &&
           receiptPage.outcomes[0].detail.find("Destination publication not confirmed") != std::string::npos &&
           receiptPage.outcomes[0].detail.find("staged path") != std::string::npos &&
           receiptPage.outcomes[0].detail.find("backup path") != std::string::npos,
       "uncertain publication and both recovery names survive disk paging");

 std::vector<int64> historyIds;
 for (size_t i = 0; i < OperationQueue::kFinishedReportLimit + 1; ++i) {
   Job folder{Action::MakeDirectory, {}, root + "/history-" + std::to_string(i)};
   historyIds.push_back(window->queue->Enqueue(folder));
 }
 check(wait([&] { return finished(historyIds.back(), 0, true, false, false); }) &&
           !window->queue->Report(pagedId) && !window->queue->Report(historyIds.front()) &&
           window->queue->Report(historyIds[1]) && window->queue->Report(historyIds.back()),
       "history expires the oldest completed reports at its explicit limit");

 window->queue->Enqueue(job(0,401));check(wait([&]{return window->conflicts.size()==5;}),"shutdown fixture is genuinely waiting at conflict");
 window->Lock();
 clearReports->Invoke();
 window->Unlock();
 check(wait([&] { return !window->queue->Report(historyIds.back()); }) &&
           window->queue->Busy(),
       "clearing finished reports leaves the current waiting conflict intact");
 auto start=system_time();window->Lock();window->Quit();check(system_time()-start<2000000,"queue destruction releases blocked conflict without UI-thread deadlock");
 clean(root);std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;
}
