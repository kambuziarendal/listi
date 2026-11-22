// SPDX-License-Identifier: MIT
#include "DirectoryScanner.h"
#include "ArchiveJobs.h"
#include "VirtualFile.h"
#include "Common.h"
#include "Pane.h"
#include <Window.h>
#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <Path.h>
#include <TextView.h>
#include <algorithm>
#include <atomic>
#include <fstream>
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
std::atomic<bool> holdNextRows{false};
sem_id held, resume, released;
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
  int count=0;std::set<std::string> names;std::string error;
  Receiver():BLooper("scan test receiver"){}
  ~Receiver(){delete_sem(done);}
  void MessageReceived(BMessage *m) override {
    if(m->what==kRows) { BMessage row;for(int32 i=0;m->FindMessage("entry",i,&row)==B_OK;++i)if(!row.GetBool("parent",false)){++count;names.insert(row.GetString("name",""));} }
    else if(m->what==kScanDone){error=m->GetString("error","");release_sem(done);}
    else BLooper::MessageReceived(m);
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
 holdNextRows = true;
 pane->Navigate(folder.c_str());
 window->Unlock();
 check(acquire_sem_etc(held, 1, B_RELATIVE_TIMEOUT, 5000000) == B_OK,
       "old reader is blocked before media loss");
 window->Lock();
 BMessage interrupted(kJobDone);
 interrupted.AddString("interruptedCopyFolder", pane->Path());
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
 window->Quit();
 delete_sem(held);
 delete_sem(resume);
 delete_sem(released);
 clean(root);std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;
}
