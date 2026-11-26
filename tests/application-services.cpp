// SPDX-License-Identifier: MIT
#include "ApplicationServices.h"
#include "Common.h"
#include "SavedSearches.h"
#include <Application.h>
#include <OS.h>
#include "SavedPlaces.h"
#include <FindDirectory.h>
#include <Path.h>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

namespace {
char settings[] = "/tmp/listi-navigation-XXXXXX";
std::atomic<bool> failDirectorySync{false};
}

extern "C" status_t __real__Z14find_directory15directory_whichP5BPathbP7BVolume(
    directory_which, BPath *, bool, BVolume *);
extern "C" status_t __wrap__Z14find_directory15directory_whichP5BPathbP7BVolume(
    directory_which which, BPath *path, bool create, BVolume *volume) {
  if (which == B_USER_SETTINGS_DIRECTORY)
    return path->SetTo(settings);
  return __real__Z14find_directory15directory_whichP5BPathbP7BVolume(which, path, create, volume);
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  struct stat st;
  if (failDirectorySync && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
    errno = EIO;
    return -1;
  }
  return __real_fsync(fd);
}

class Receiver : public BLooper {
public:
  Receiver() : BLooper("service test replies") { assert(ready >= B_OK); }
  ~Receiver() override { delete_sem(ready); }

  void MessageReceived(BMessage *message) override {
    last = *message;
    release_sem(ready);
  }

  BMessage Next() {
    assert(acquire_sem_etc(ready, 1, B_RELATIVE_TIMEOUT, 5000000) == B_OK);
    assert(Lock());
    BMessage reply(last);
    Unlock();
    return reply;
  }

private:
  sem_id ready = create_sem(0, "service reply");
  BMessage last;
};

int main() {
  assert(mkdtemp(settings));
  BApplication app("application/x-vnd.Listi-ApplicationServicesTest");
  auto *notices = new Receiver;
  auto *pane = new Receiver;
  notices->Run();
  pane->Run();
  auto *worker = CreateApplicationServices(BMessenger(notices), false);
  worker->Run();

  // Editing a virtual entry must not launch an editor, or fetch the file.
  BMessage open(kOpenPaths);
  open.AddString("mode", "edit");
  open.AddString("path", "archive:///synthetic.zip/entry");
  assert(worker->PostMessage(&open) == B_OK);
  const auto notice = notices->Next();
  assert(notice.what == kServiceError);
  assert(std::string(notice.GetString("error", "")).find("Copy this entry locally") !=
         std::string::npos);

  // Even a rejected image probe answers the requesting pane with its token.
  BMessage probe(kImageDropProbe);
  probe.AddMessenger("replyTo", BMessenger(pane));
  probe.AddInt64("request", 42);
  assert(worker->PostMessage(&probe) == B_OK);
  const auto reply = pane->Next();
  assert(reply.what == kImageDropOutputs && reply.GetInt64("request", -1) == 42);
  assert(reply.GetString("error", "")[0] != '\0');

  // Reject an invalid format before touching settings, echoing this attempt's ID.
  BMessage save(kSaveSearches);
  save.AddInt32("version", -1);
  save.AddInt64("revision", 4);
  save.AddInt64("serial", 12);
  assert(worker->PostMessage(&save) == B_OK);
  const auto rejected = notices->Next();
  assert(rejected.what == kSearchesSaved && rejected.HasString("error"));
  assert(rejected.GetInt64("revision", -1) == 4 && rejected.GetInt64("serial", -1) == 12);

  for (const auto kind : {uint32(kSavePlaces), kSaveSearches}) {
    BMessage request(kind), edit;
    edit.AddString("name", "Work");
    edit.AddInt32("action", 0);
    edit.AddString("path", "/work");
    edit.AddString("root", "/work");
    edit.AddString("expression", "note");
    request.AddMessage("edit", &edit);
    failDirectorySync = true;
    assert(worker->PostMessage(&request) == B_OK);
    auto result = notices->Next();
    failDirectorySync = false;
    BMessage snapshot;
    assert(result.FindMessage("snapshot", &snapshot) == B_OK);
    assert(std::string(result.GetString("error", "")).find("were replaced") != std::string::npos);
    if (kind == kSavePlaces)
      assert(Listi::SavedPlaces::FromMessage(snapshot).Find("/work"));
    else
      assert(Listi::SavedSearches::FromMessage(snapshot).Find("Work"));

    // Retrying the same add is a conflict, not a second insertion.
    assert(worker->PostMessage(&request) == B_OK);
    result = notices->Next();
    assert(result.HasString("error") && !result.HasMessage("snapshot"));
    assert(unlink(SettingsPath(kind == kSavePlaces ? "Lister_places" : "Lister_searches").String()) == 0);
  }
  assert(rmdir(settings) == 0);

  const auto stop = [](BLooper *looper) {
    assert(looper->Lock());
    looper->Quit();
  };
  stop(worker);
  stop(pane);
  stop(notices);
}
