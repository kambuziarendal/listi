// SPDX-License-Identifier: MIT
#include "Pane.h"
#include "PaneModel.h"
#include "FileGrid.h"
#include "ReadWorker.h"
#include "ImagePreview.h"
#include "SearchWindow.h"
#include "SavedSearches.h"
#include <Application.h>
#include <MessageFilter.h>
#include <Window.h>
#include <iostream>
#include <cstring>
#include <vector>

// Hold a real owned reader inside its looper while the owner closes. This
// checks that shutdown returns before release, and that the reader subsequently exits.
struct Hold {
  sem_id entered = create_sem(0, "reader entered"), release = create_sem(0, "release reader");
  sem_id closed = create_sem(0, "owner closed");
  ~Hold() { delete_sem(entered); delete_sem(release); delete_sem(closed); }
};
class Delay : public BMessageFilter {
  Hold &hold;
public:
  explicit Delay(Hold &h) : BMessageFilter('hold'), hold(h) {}
  filter_result Filter(BMessage *, BHandler **) override {
    release_sem(hold.entered);
    acquire_sem(hold.release);
    return B_SKIP_MESSAGE;
  }
};
// Thread names are system-wide on Haiku. Never hold or wait for a worker
// belonging to a running file manager or a second test process.
thread_id FindReader(const char *name) {
  int32 cookie = 0;
  thread_info info;
  while (get_next_thread_info(be_app->Team(), &cookie, &info) == B_OK) {
    if (strcmp(info.name, name) == 0)
      return info.thread;
  }
  return B_NAME_NOT_FOUND;
}

int main() {
  BApplication app("application/x-vnd.Listi-WorkerLifecycleTest");
  int checks=0, failures=0;
  auto check=[&](bool ok,const std::string &label) {
    ++checks; failures+=!ok; std::cout<<(ok?"PASS ":"FAIL ")<<label<<std::endl;
  };
  for (const char *name : {"thumbnail reader", "image preview reader", "directory reader", "folder tree reader", "file search"}) {
    auto window=new BWindow(BRect(10,10,810,610),"Worker lifecycle test",B_TITLED_WINDOW,0);
    window->AddChild(new Pane(0,Preferences{}));
    window->AddChild(new ImagePreview());
    window->Show();
    BMessenger search=OpenFileSearch(BMessenger(window),0,"/tmp",Listi::SavedSearches().Message());
    thread_id reader=FindReader(name);
    check(reader>=0,std::string(name)+" created by real view");
    if(reader<0)return 2;
    Hold hold;
    auto worker=BLooper::LooperForThread(reader);
    if(!worker || !worker->Lock())return 3;
    worker->AddCommonFilter(new Delay(hold));
    worker->PostMessage('hold');worker->Unlock();
    if(acquire_sem_etc(hold.entered,1,B_RELATIVE_TIMEOUT,10000000)!=B_OK)return 4;
    auto release=spawn_thread([](void *p)->int32 {
      auto &hold = *static_cast<Hold *>(p);
      // Failsafe: an unfixed owner can join without wedging the test.
      acquire_sem_etc(hold.closed, 1, B_RELATIVE_TIMEOUT, 3000000);
      release_sem(hold.release);
      return 0;
    }, "reader test release", B_NORMAL_PRIORITY, &hold);
    resume_thread(release);
    bigtime_t start=system_time();
    if(!strcmp(name,"file search")) {
      BLooper *owner=nullptr;search.Target(&owner);
      if(!owner || !owner->Lock())return 5;
      owner->Quit();
    } else {window->Lock();window->Quit();window=nullptr;}
    thread_info info;
    check(get_thread_info(reader,&info)==B_OK,std::string(name)+" remains held after owner closes");
    check(system_time()-start<1000000,std::string(name)+" owner closes within one second");
    release_sem(hold.closed);
    status_t result;
    wait_for_thread(release,&result);
    status_t waited = wait_for_thread_etc(reader, B_RELATIVE_TIMEOUT, 2000000, &result);
    check(waited==B_OK || waited==B_BAD_THREAD_ID, std::string(name)+" exits after release");
    if(window){window->Lock();window->Quit();}
    if(strcmp(name,"file search")) {BLooper *owner=nullptr;search.Target(&owner);if(owner&&owner->Lock())owner->Quit();}
    check(Listi::DrainReadWorkers(2000000), "readers drained before application teardown");
  }
  std::vector<Listi::ReadWorker *> readers;
  for (size_t n = 0; n < Listi::kReadWorkerLimit; ++n) {
    Listi::ReadWorker *worker = new Listi::ReadWorker("capacity probe");
    if (!Listi::StartReadWorker(worker))
      break;
    readers.push_back(worker);
  }
  check(readers.size() == Listi::kReadWorkerLimit, "fill all reader slots");
  Listi::ReadWorker *extra = new Listi::ReadWorker("excess reader");
  check(!Listi::StartReadWorker(extra) && !extra, "capacity refuses a worker without leaking its looper");
  if (extra)
    Listi::RetireReadWorker(extra);
  auto *gridWindow = new BWindow(BRect(10, 10, 400, 350), "Grid recovery", B_TITLED_WINDOW, 0);
  gridWindow->SetPulseRate(50000);
  Pane pane(0, Preferences{});
  PaneModel model;
  auto row = std::make_unique<PaneEntry>();
  row->name = "preview.png";
  row->path = "/tmp/listi-thumbnail-capacity-fixture.png";
  row->iconRequested = true; // Let the released slot belong to the thumbnail reader.
  model.AddScanned(std::move(row), "", false);
  auto *grid = CreateFileGrid(&pane, &model);
  grid->ResizeTo(350, 300);
  grid->SetMode(3);
  gridWindow->AddChild(grid);
  gridWindow->Show();
  auto drawGrid = [&] {
    // Thumbnail submission starts in Draw. An idle VM's screen blanker can
    // legitimately suppress app_server updates to this window.
    gridWindow->Lock();
    grid->Draw(grid->Bounds());
    gridWindow->Unlock();
  };
  drawGrid();
  check(FindReader("thumbnail reader") < 0, "attached thumbnail grid respects the full pool");
  Listi::RetireReadWorker(readers.back());
  const auto deadline = system_time() + 3000000;
  while (FindReader("thumbnail reader") < 0 && system_time() < deadline) {
    drawGrid();
    snooze(20000);
  }
  check(FindReader("thumbnail reader") >= 0, "same attached thumbnail grid recovers after one slot opens");
  extra = new Listi::ReadWorker("excess reader");
  check(!Listi::StartReadWorker(extra) && !extra, "recovered thumbnail grid still respects the worker cap");
  Listi::RetireReadWorker(extra);
  gridWindow->Lock();
  gridWindow->Quit();
  for (auto &worker : readers)
    Listi::RetireReadWorker(worker);
  check(Listi::DrainReadWorkers(2000000), "all capacity probes exited");
  extra = new Listi::ReadWorker("replacement reader");
  check(Listi::StartReadWorker(extra), "exited reader slots can be reused");
  Listi::RetireReadWorker(extra);
  check(Listi::DrainReadWorkers(2000000), "replacement joined before teardown");
  std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;
  return failures?1:0;
}
