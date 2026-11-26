// SPDX-License-Identifier: MIT
#include "DirectoryScanner.h"
#include "ArchiveJobs.h"
#include "VirtualFile.h"
#include "Common.h"
#include "Pane.h"
#include "UnavailableVolume.h"
#include <Window.h>
#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <Path.h>
#include <TextView.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <dirent.h>
#include <limits.h>
#include <cstring>
#include <iostream>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
using namespace Listi;
namespace {
std::atomic<int> rowFailures{0}, doneFailures{0};
std::atomic<int> rowAttempts{0}, doneAttempts{0};
std::atomic<int64> lastDoneSerial{-1};
std::shared_ptr<std::atomic<int64>> generation;
bool cancelOnFailure=false;
std::atomic<bool> holdNextRows{false}, holdNextDone{false};
sem_id held, resume, released;
std::string blockedPath;
std::atomic<bool> blockOpen{false};
std::atomic<thread_id> blockedReader{-1};
std::atomic<thread_id> watchedReader{-1};
std::atomic<int> readsAfterLoss{0};
sem_id openEntered, allowOpen;
}

static std::atomic<int> inspectionError{0};
extern "C" int __real_fstatat(int, const char *, struct stat *, int);
extern "C" int __wrap_fstatat(int fd, const char *name, struct stat *st, int flags) {
  const int error = inspectionError.load();
  if (error && strcmp(name, "file-159") == 0) {
    errno = error;
    return -1;
  }
  return __real_fstatat(fd, name, st, flags);
}
extern "C" dirent *__real_readdir(DIR *);
extern "C" dirent *__wrap_readdir(DIR *dir) {
  if (watchedReader.load() == find_thread(nullptr))
    ++readsAfterLoss;
  return __real_readdir(dir);
}
extern "C" DIR *__real_opendir(const char *);
extern "C" DIR *__wrap_opendir(const char *path) {
  if (blockOpen.load() && blockedPath == path) {
    thread_info info;
    if (get_thread_info(find_thread(nullptr), &info) == B_OK &&
        strcmp(info.name, "directory reader") == 0 && blockOpen.exchange(false)) {
      blockedReader = info.thread;
      release_sem(openEntered);
      // The timeout releases a broken candidate, so a regression fails
      // rather than wedging the native suite on the window lock.
      acquire_sem_etc(allowOpen, 1, B_RELATIVE_TIMEOUT, 6000000);
    }
  }
  return __real_opendir(path);
}
extern "C" status_t __real__ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl(const BMessenger*,BMessage*,BHandler*,bigtime_t);
extern "C" status_t __wrap__ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl(const BMessenger* self,BMessage* message,BHandler* reply,bigtime_t timeout) {
  if (message->what == kRows && holdNextRows.exchange(false)) {
    release_sem(held);
    acquire_sem(resume);
    release_sem(released);
  }
  auto fail=[&](std::atomic<int>& n) { int old=n.load(); if(old>0) {n.fetch_sub(1);return true;} return false; };
  if(message->what==kRows) { ++rowAttempts; if(fail(rowFailures)) {if(cancelOnFailure)generation->fetch_add(1);return B_TIMED_OUT;} }
  if (message->what == kScanDone) {
    if (holdNextDone.exchange(false)) {
      release_sem(held);
      acquire_sem_etc(resume, 1, B_RELATIVE_TIMEOUT, 5000000);
    }
    ++doneAttempts;
    lastDoneSerial = message->GetInt64("serial", -1);
    if (fail(doneFailures))
      return B_TIMED_OUT;
  }
  return __real__ZNK10BMessenger11SendMessageEP8BMessageP8BHandlerl(self,message,reply,timeout);
}
class Receiver:public BLooper {
public:
  sem_id done=create_sem(0,"scan test done");
  int count=0;std::set<std::string> names;std::string error, warning;
  Receiver():BLooper("scan test receiver"){}
  ~Receiver(){delete_sem(done);}
  void MessageReceived(BMessage *m) override {
    if(m->what==kRows) { BMessage row;for(int32 i=0;m->FindMessage("entry",i,&row)==B_OK;++i)if(!row.GetBool("parent",false)){++count;names.insert(row.GetString("name",""));} }
    else if(m->what==kScanDone){error=m->GetString("error","");warning=m->GetString("inspectionWarning","");release_sem(done);}
    else BLooper::MessageReceived(m);
  }
};
class PacedReceiver : public Receiver {
public:
  sem_id batch = create_sem(0, "pane handled a row batch");
  ~PacedReceiver() { delete_sem(batch); }
  void MessageReceived(BMessage *message) override {
    Receiver::MessageReceived(message);
    if (message->what == kRows)
      release_sem(batch);
  }
};
void clean(const std::string &p){BDirectory d(p.c_str());BEntry e;while(d.GetNextEntry(&e,false)==B_OK){struct stat s;e.GetStat(&s);if(S_ISDIR(s.st_mode)){BPath path;e.GetPath(&path);clean(path.Path());}e.Remove();}rmdir(p.c_str());}
int main() {
 BApplication app("application/x-vnd.Listi-DirectoryDeliveryTest");
 char temp[]="/tmp/Listi-delivery-XXXXXX";if(!mkdtemp(temp))return 2;
 std::string root=temp,folder=root+"/files";mkdir(folder.c_str(),0700);
 for(int i=0;i<160;++i)std::ofstream(folder+"/file-"+std::to_string(i))<<"payload";
 auto create=PrepareArchiveJob(false,{folder});create.archiveFormat="zip";SetArchiveDestination(create,root,"fixture");if(Run(create,{}).completed!=1)return 3;
 auto archive=ChildLocation(OpenArchive(create.destination),"files");
 int checks=0,failures=0;
 auto check=[&](bool ok,const std::string &label){++checks;failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<label<<std::endl;};
 for(auto path:{folder,archive}) for(int mode=0;mode<6;++mode) {
  auto receiver=new Receiver;receiver->Run();generation=std::make_shared<std::atomic<int64>>(1);
  auto delivery=std::make_shared<DirectoryScanDelivery>();
  auto scan=CreateDirectoryScanner(BMessenger(receiver),generation,delivery);scan->Run();
  rowFailures=mode==1?1:(mode==2||mode==5?100:0);doneFailures=mode==3?1:(mode==4?100:0);
  rowAttempts=0;doneAttempts=0;cancelOnFailure=mode==5;
  auto request=[&](int64 serial){BMessage m(kScan);m.AddInt64("serial",serial);m.AddString("path",path.c_str());m.AddBool("icons",false);scan->PostMessage(&m);};
  request(1);
  auto status=acquire_sem_etc(receiver->done,1,B_RELATIVE_TIMEOUT,(mode==4||mode==5)?1000000:10000000);
  receiver->Lock();
  auto label=std::string(path==folder?"local":"archive")+" mode "+std::to_string(mode);
  if(mode==4)check(status==B_TIMED_OUT&&delivery->failedSerial==1&&doneAttempts==3,label+" bounded completion failure has out-of-band signal");
  else if(mode==5)check(status==B_TIMED_OUT&&generation->load()==2&&rowAttempts==1&&doneAttempts==0,label+" stale retry cancelled without completion");
  else if(mode==2)check(status==B_OK&&!receiver->error.empty()&&rowAttempts==3,label+" exhausted row retries report incomplete");
  else check(status==B_OK&&receiver->error.empty()&&receiver->count==160&&receiver->names.size()==160,label+" all entries exactly once");
  receiver->count=0;receiver->names.clear();receiver->error.clear();receiver->Unlock();
  rowFailures=0;doneFailures=0;cancelOnFailure=false;generation->store(3);request(3);
  status=acquire_sem_etc(receiver->done,1,B_RELATIVE_TIMEOUT,10000000);
  receiver->Lock();check(status==B_OK&&receiver->error.empty()&&receiver->count==160&&receiver->names.size()==160,label+" next scan recovers");receiver->Unlock();
  generation->store(4);if(scan->Lock())scan->Quit();if(receiver->Lock())receiver->Quit();
 }
 {
   auto *receiver = new Receiver;
   receiver->Run();
   auto epoch = std::make_shared<std::atomic<int64>>(0);
   auto *reader = CreateDirectoryScanner(BMessenger(receiver), epoch, {});
   reader->Run();
   for (int failure : {EACCES, EIO, ENOENT, 0}) {
     inspectionError = failure;
     BMessage request(kScan);
     request.AddString("path", folder.c_str());
     request.AddBool("icons", false);
     request.AddInt64("serial", ++*epoch);
     reader->PostMessage(&request);
     const auto status = acquire_sem_etc(receiver->done, 1, B_RELATIVE_TIMEOUT, 5000000);
     receiver->Lock();
     check(status == B_OK && receiver->error.empty() &&
           receiver->names.size() == (failure ? 159u : 160u) &&
           (failure == EACCES || failure == EIO
                ? receiver->warning.find("file-159") != std::string::npos
                : receiver->warning.empty()),
           "partial listing, disappearance, or recovered reload: " + std::to_string(failure));
     receiver->names.clear();
     receiver->Unlock();
   }
   if (reader->Lock()) reader->Quit();
   if (receiver->Lock()) receiver->Quit();
 }
 // A successful port send is not a UI receipt. Keep only one batch in flight
 // for this fixture; retiring an unacknowledged scan must not join the pane.
 {
   auto *receiver = new PacedReceiver;
   receiver->Run();
   auto epoch = std::make_shared<std::atomic<int64>>(1);
   auto delivery = std::make_shared<DirectoryScanDelivery>(1);
   auto *reader = CreateDirectoryScanner(BMessenger(receiver), epoch, delivery);
   reader->Run();
   BMessage request(kScan);
   request.AddString("path", folder.c_str());
   request.AddBool("icons", false);
   request.AddInt64("serial", 1);
   reader->PostMessage(&request);
   auto receive = [&] {
     return acquire_sem_etc(receiver->batch, 1, B_RELATIVE_TIMEOUT, 2000000) == B_OK;
   };
   check(receive(), "paced scan delivers its first batch immediately");
   check(acquire_sem_etc(receiver->batch, 1, B_RELATIVE_TIMEOUT, 150000) == B_TIMED_OUT,
         "next batch waits for a pane receipt");
   delivery->ReleaseBatch(1);
   check(receive(), "pane receipt releases the next batch");
   delivery->ReleaseBatch(1);
   check(receive(), "final partial batch is delivered");
   delivery->ReleaseBatch(1);
   check(acquire_sem_etc(receiver->done, 1, B_RELATIVE_TIMEOUT, 2000000) == B_OK &&
         receiver->count == 160 && receiver->names.size() == 160,
         "paced scan retains every row and completion");
   request.ReplaceInt64("serial", 2);
   epoch->store(2);
   reader->PostMessage(&request);
   check(receive(), "second paced scan starts");
   epoch->store(3);
   const bool cancelled = reader->LockWithTimeout(2000000) == B_OK;
   check(cancelled, "obsolete paced scan exits without a UI receipt");
   if (cancelled) {
     reader->Unlock();
     request.ReplaceInt64("serial", 3);
     reader->PostMessage(&request);
     check(receive(), "fresh scan is not charged for an obsolete queued batch");
     epoch->store(4);
     if (reader->Lock()) reader->Quit();
   }
   if (receiver->Lock()) receiver->Quit();
 }
 // Exercise the actual pane's out-of-band completion recovery, not just the
 // producer. All view changes are made under the owning window lock.
 auto window=new BWindow(BRect(10,10,810,610),"Directory recovery test",B_TITLED_WINDOW,0);
 Preferences prefs; prefs.icons=false;
 auto pane=new Pane(0,prefs);window->AddChild(pane);window->Show();
 auto ready=[&] {
   for(int i=0;i<200;++i) {
     window->Lock();bool done=!pane->IsScanning();window->Unlock();
     if(done)return true;snooze(50000);
   }
   return false;
 };
 check(ready(),"native pane initial scan settles");
 for(bool completion:{false,true}) {
   window->Lock();rowFailures=completion?0:100;doneFailures=completion?100:0;
   pane->Navigate(folder.c_str());window->Unlock();
   check(ready(),completion?"pane exits reading after lost completion":"pane exits reading after lost rows");
   window->Lock();
   auto status=dynamic_cast<BTextView*>(pane->FindView("status"));
   check(status&&std::string(status->Text()).find("Reload")!=std::string::npos,"incomplete pane visibly offers reload");
   pane->Select(1);check(pane->Selected().empty(),"incomplete entries cannot become Select All input");
   rowFailures=0;doneFailures=0;pane->Reload();window->Unlock();
   check(ready(),"native pane reload recovers");
   window->Lock();pane->Select(1);check(pane->Selected().size()==160,"recovered pane selects all 160 files");window->Unlock();
 }
 window->Lock();
 auto replaced = folder + "/file-0";
 auto retired = root + "/retired";
 bool swapped = rename(replaced.c_str(), retired.c_str()) == 0 &&
                bool(std::ofstream(replaced) << "replacement");
 pane->Reload();
 window->Unlock();
 check(swapped && ready(),"same-name replacement and reload settle");
 window->Lock();
 auto selected = pane->Selected();
 check(selected.size() == 159 &&
       std::find(selected.begin(), selected.end(), replaced) == selected.end(),
       "reload keeps untouched selections, not a replacement at the same path");
 // A server failure and a delivery failure both arrive as kScanDone
 // errors. Inject the latter here so retry coverage needs no network.
 rowFailures = 100;
 pane->Navigate(folder.c_str());
 window->Unlock();
 check(ready(), "failed location attempt settles");
 window->Lock();
 auto *status = dynamic_cast<BTextView *>(pane->FindView("status"));
 auto *pathControl = dynamic_cast<BTextControl *>(pane->FindView("path"));
 auto *filterControl = dynamic_cast<BTextControl *>(pane->FindView("filter"));
 if (status && pathControl && filterControl) {
   const std::string error = status->Text();
   filterControl->SetText("file");
   BMessage changed(kFilterChanged);
   pane->MessageReceived(&changed);
   pane->ShowStatus("Unrelated status update");
   check(error.find("Cannot open:") == 0 && status->Text() == error,
         "filter and unrelated status leave the navigation error visible");

   const int64 failedSerial = lastDoneSerial;
   pathControl->MakeFocus(true);
   BMessenger editor(pathControl->TextView());
   window->Unlock();
   const char enter[] = {B_ENTER, 0};
   BMessage key(B_KEY_DOWN);
   key.AddString("bytes", enter);
   key.AddInt8("byte", B_ENTER);
   key.AddInt32("raw_char", B_ENTER);
   key.AddInt32("key", 0);
   key.AddInt32("modifiers", 0);
   BMessage frame(B_GET_PROPERTY), reply;
   frame.AddSpecifier("Frame");
   const bool delivered = editor.SendMessage(&key) == B_OK &&
       editor.SendMessage(&frame, &reply, 2000000, 2000000) == B_OK;
   const bool retried = delivered && ready();
   window->Lock();
   check(retried && lastDoneSerial == failedSerial + 1 &&
             status->Text() == error,
         "Enter on unchanged path retries exactly once and shows the error again");

   const int64 retrySerial = lastDoneSerial;
   rowFailures = 0;
   // Connect sends a kNavigate with an explicit path to the originating pane.
   BMessage connect(kNavigate);
   connect.AddString("path", folder.c_str());
   pane->MessageReceived(&connect);
   window->Unlock();
   const bool recovered = ready();
   window->Lock();
   check(recovered && lastDoneSerial == retrySerial + 1 &&
             pane->LocationValid() &&
             std::string(status->Text()).find("Cannot open:") == std::string::npos,
         "explicit same-path navigation replaces the error after recovery");
 } else {
   check(false, "native pane has controls for the navigation retry test");
 }
 // Park a native reader with its own looper locked. Navigating to an
 // independent location must complete before the obsolete reader returns.
 held = create_sem(0, "held directory reader");
 resume = create_sem(0, "resume directory reader");
 released = create_sem(0, "released directory reader");
 // A reader enumerates one opened directory. Replacing the pathname during
 // enumeration must not publish a valid mixed listing from the replacement.
 {
   auto *receiver = new Receiver;
   receiver->Run();
   auto epoch = std::make_shared<std::atomic<int64>>(1);
   auto *reader = CreateDirectoryScanner(BMessenger(receiver), epoch);
   reader->Run();
   BMessage request(kScan);
   request.AddInt64("serial", 1);
   request.AddString("path", folder.c_str());
   request.AddBool("icons", false);
   holdNextRows = true;
   reader->PostMessage(&request);
   const bool paused = acquire_sem_etc(held, 1, B_RELATIVE_TIMEOUT, 5000000) == B_OK;
   const auto moved = folder + "-moved";
   const bool renamed = paused && rename(folder.c_str(), moved.c_str()) == 0;
   const bool replaced = renamed && mkdir(folder.c_str(), 0700) == 0;
   if (replaced)
     std::ofstream(folder + "/replacement") << "different folder";
   release_sem(resume);
   if (paused)
     acquire_sem_etc(released, 1, B_RELATIVE_TIMEOUT, 5000000);
   const bool completed = acquire_sem_etc(receiver->done, 1, B_RELATIVE_TIMEOUT, 5000000) == B_OK;
   receiver->Lock();
   check(replaced && completed &&
       receiver->error == "Folder changed while reading. Reload to try again." &&
       receiver->count == 160 && receiver->names.count("replacement") == 0,
       "directory replacement does not authorize a mixed listing");
   receiver->Quit();
   if (reader->Lock())
     reader->Quit();
   if (replaced) {
     unlink((folder + "/replacement").c_str());
     rmdir(folder.c_str());
   }
   if (renamed && rename(moved.c_str(), folder.c_str()) != 0)
     return 4;
 }
 // Hold a scan after its first batch. Latch the path as a failed transfer
 // would, without any UI notification, cancellation or serial change.
 {
   const auto fixture = root + "/interrupted-scan";
   if (mkdir(fixture.c_str(), 0700))
     return 4;
   char canonical[PATH_MAX];
   if (!realpath(fixture.c_str(), canonical))
     return 4;
   for (int i = 0; i < 20; ++i)
     std::ofstream(fixture + "/" + std::to_string(i)) << "row";
   auto *receiver = new Receiver;
   receiver->Run();
   auto epoch = std::make_shared<std::atomic<int64>>(1);
   auto *reader = CreateDirectoryScanner(BMessenger(receiver), epoch);
   reader->Run();
   BMessage request(kScan);
   request.AddInt64("serial", 1);
   request.AddString("path", canonical);
   request.AddBool("icons", false);
   request.AddBool("firstScreen", true);
   holdNextRows = true;
   reader->PostMessage(&request);
   const bool paused = acquire_sem_etc(held, 1, B_RELATIVE_TIMEOUT, 5000000) == B_OK;
   InterruptedVolumes().Block(canonical);
   watchedReader = reader->Thread();
   release_sem(resume);
   const bool completed = acquire_sem_etc(receiver->done, 1, B_RELATIVE_TIMEOUT, 5000000) == B_OK;
   if (paused)
     acquire_sem_etc(released, 1, B_RELATIVE_TIMEOUT, 5000000);
   watchedReader = -1;
   receiver->Lock();
   check(paused && completed && readsAfterLoss == 0 &&
             receiver->error == kUnavailableVolumeText,
         "active scan stops before readdir after disk loss without waiting for a window broadcast");
   receiver->Quit();
   if (reader->Lock())
     reader->Quit();
 }
 // Mutate the directory after enumeration and before the terminal message.
 holdNextDone = true;
 pane->Navigate(folder.c_str());
 window->Unlock();
 bool pausedDone = acquire_sem_etc(held, 1, B_RELATIVE_TIMEOUT, 5000000) == B_OK;
 // Pane rows carry canonical paths: on Haiku /tmp is /boot/system/cache/tmp.
 char watchedPath[PATH_MAX];
 if (!realpath(folder.c_str(), watchedPath))
   return 4;
 auto removed = std::string(watchedPath) + "/file-159";
 auto added = std::string(watchedPath) + "/new-entry";
 bool mutated = pausedDone && unlink(removed.c_str()) == 0 &&
                bool(std::ofstream(added) << "new");
 release_sem(resume);
 bool converged = false;
 auto deadline = system_time() + 5000000;
 while (mutated && !converged && system_time() < deadline) {
   window->Lock();
   if (!pane->IsScanning()) {
     pane->Select(1);
     auto rows = pane->Selected();
     converged = rows.size() == 160 &&
         std::find(rows.begin(), rows.end(), added) != rows.end() &&
         std::find(rows.begin(), rows.end(), removed) == rows.end();
   }
   window->Unlock();
   snooze(20000);
 }
 check(converged, "pre-completion creation and removal converge without manual reload");
 // This simulated lost volume must be independent of the healthy folder
 // used by the ordinary blocked-opendir test below. The latch lasts until exit.
 const auto lostFolder = root + "/removed-volume";
 if (mkdir(lostFolder.c_str(), 0700) != 0 ||
     !(std::ofstream(lostFolder + "/entry") << "disconnected file"))
   return 4;
 char canonicalLost[PATH_MAX];
 if (!realpath(lostFolder.c_str(), canonicalLost))
   return 4;
 window->Lock();
 holdNextRows = true;
 pane->Navigate(canonicalLost);
 window->Unlock();
 check(acquire_sem_etc(held, 1, B_RELATIVE_TIMEOUT, 5000000) == B_OK,
       "old reader is blocked before media loss");
 window->Lock();
 InterruptedVolumes().Block(canonicalLost);
 BMessage interrupted(kJobDone);
 interrupted.AddString("interruptedCopyFolder", canonicalLost);
 pane->MessageReceived(&interrupted);
 BMessage unrelated(kJobDone);
 pane->MessageReceived(&unrelated);
 check(!pane->LocationValid() && !pane->IsScanning(),
       "interrupted panel disables file actions and does not automatically reread the disk");
 pane->Navigate(root.c_str());
 window->Unlock();
 check(ready(), "new directory settles before the retired reader returns");
 release_sem(resume);
 check(acquire_sem_etc(released, 1, B_RELATIVE_TIMEOUT, 5000000) == B_OK,
       "old reader can unwind");
 window->Lock();
 pane->Select(1);
 check(pane->LocationValid() && !pane->Selected().empty() && pane->Selected().size() < 160,
       "late old rows do not replace the new directory");
 window->Unlock();

 // No operation or interrupted-copy notification participates in these
 // two cases: the held call is an ordinary native directory scan.
 char canonicalFolder[PATH_MAX], canonicalRoot[PATH_MAX];
 if (!realpath(folder.c_str(), canonicalFolder) || !realpath(root.c_str(), canonicalRoot))
   return 4;
 blockedPath = canonicalFolder;
 openEntered = create_sem(0, "opendir entered");
 allowOpen = create_sem(0, "release opendir");
 auto holdScan = [&] {
   blockedReader = -1;
   blockOpen = true;
   window->Lock();
   pane->Navigate(folder.c_str());
   window->Unlock();
   return acquire_sem_etc(openEntered, 1, B_RELATIVE_TIMEOUT, 2000000) == B_OK;
 };
 auto drainScan = [&] {
   release_sem(allowOpen);
   const auto id = blockedReader.load();
   if (id < 0)
     return false;
   thread_info info;
   auto deadline = system_time() + 2000000;
   while (get_thread_info(id, &info) == B_OK && system_time() < deadline)
     snooze(10000);
   return get_thread_info(id, &info) == B_BAD_THREAD_ID;
 };
 check(holdScan(), "ordinary navigation held inside opendir without a copy job");
 auto start = system_time();
 window->Lock();
 pane->Navigate(root.c_str());
 window->Unlock();
 check(ready() && system_time() - start < 2000000,
       "healthy navigation completes within two seconds of replacing a blocked read");
 check(drainScan(), "obsolete ordinary reader exits after opendir returns");
 window->Lock();
 pane->Select(1);
 check(pane->LocationValid() && std::string(pane->Path()) == canonicalRoot &&
           !pane->Selected().empty() && pane->Selected().size() < 160,
       "late scan cannot replace the current path, validity or rows");
 window->Unlock();

 check(holdScan(), "ordinary navigation blocked again before closing its window");
 start = system_time();
 window->Lock();
 window->Quit();
 thread_info info;
 check(system_time() - start < 2000000 &&
           get_thread_info(blockedReader.load(), &info) == B_OK,
       "window closes within two seconds while its old read is still blocked");
 check(drainScan(), "reader safely exits after the window has been destroyed");
 delete_sem(openEntered);
 delete_sem(allowOpen);
 delete_sem(held);
 delete_sem(resume);
 delete_sem(released);
 clean(root);std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;
}
