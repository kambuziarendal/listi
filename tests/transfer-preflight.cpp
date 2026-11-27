// SPDX-License-Identifier: MIT
#include "OperationQueue.h"
#include "TransferCommand.h"
#include <Application.h>
#include <LayoutBuilder.h>
#include <Window.h>
#include <atomic>
#include <cstdio>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <string_view>

using namespace Listi;
namespace fs = std::filesystem;
namespace {
const uint32 kPrepare = 'prep', kCancelFromWindow = 'cncl';
constexpr bigtime_t timeout = 15000000;
std::atomic<thread_id> uiThread{-1};
sem_id scanPaused, resumeScan;
std::atomic<bool> pauseScan{false};
std::atomic<int> reads{0}, uiOpens{0};
thread_local DIR *watched;

struct Fixture {
  fs::path root;
  Fixture() {
    char name[] = "/tmp/Listi-preflight-XXXXXX";
    if (!mkdtemp(name))
      throw std::runtime_error("Cannot create fixture");
    root = fs::canonical(name);
  }
  ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
};
void Require(bool ok, const char *label) {
  if (!ok)
    throw std::runtime_error(label);
}
class TransferWindow : public BWindow {
public:
  OperationQueue queue;
  sem_id done = create_sem(0, "transfer done");
  Job request;
  BMessage terminal;
  int64 operation = 0;

  TransferWindow() : BWindow(BRect(0, 0, 600, 300), "Transfer test", B_TITLED_WINDOW, 0),
                     queue(BMessenger(this)) {
    BLayoutBuilder::Group<>(this, B_VERTICAL).Add(queue.View());
    queue.Start();
  }
  ~TransferWindow() { delete_sem(done); }
  void MessageReceived(BMessage *m) override {
    if (m->what == kPrepare) {
      BMessage reply;
      try {
        operation = queue.Enqueue(PrepareTransfer(request));
        reply.AddBool("ok", operation != 0);
      } catch (const std::exception &error) {
        fprintf(stderr, "Window preparation failed: %s\n", error.what());
      }
      m->SendReply(&reply);
    } else if (m->what == kCancelFromWindow) {
      BMessage reply;
      reply.AddBool("ok", queue.CancelOperation(operation));
      m->SendReply(&reply);
    } else if (m->what == kQueueWake) {
      for (const auto &event : queue.TakeEvents()) {
        if (event.what == kJobDone) {
          terminal = event;
          release_sem(done);
        }
      }
    } else {
      BWindow::MessageReceived(m);
    }
  }
};
bool Send(TransferWindow *window, uint32 what) {
  BMessage message(what), reply;
  return BMessenger(window).SendMessage(&message, &reply, timeout, timeout) == B_OK &&
         reply.GetBool("ok", false);
}
void RunCase(TransferWindow *window, const fs::path &base, int count) {
  const auto source = base / "in/set", out = base / "out", protectedFile = out / "set/saved";
  fs::create_directories(source);
  fs::create_directories(protectedFile.parent_path());
  std::ofstream(protectedFile) << "keep";
  auto first = base / "in/first";
  std::ofstream(first) << "first write";
  for (int i = 0; i < count; ++i)
    std::ofstream(source / std::to_string(i)).put('x');
  const Job job{Action::Copy, {first.string(), source.string(), protectedFile.string()}, out.string()};
  window->Lock();
  window->request = job;
  window->Unlock();
  reads = uiOpens = 0;
  CheckTransferOutputTrees(job.sources, job.destination, {});
  Require(reads >= count, "disjoint preflight did not inspect the full merge");
  reads = 0;
  pauseScan = true;
  Require(Send(window, kPrepare), "window did not enqueue disjoint transfer");
  Require(acquire_sem_etc(scanPaused, 1, B_RELATIVE_TIMEOUT, timeout) == B_OK,
          "worker did not pause inside preflight enumeration");
  const bool responsive = Send(window, kCancelFromWindow);
  release_sem(resumeScan);
  Require(responsive, "window did not accept cancellation while preflight was blocked");
  Require(acquire_sem_etc(window->done, 1, B_RELATIVE_TIMEOUT, timeout) == B_OK,
          "cancelled job did not finish");
  window->Lock();
  const bool cancelled = window->terminal.GetBool("cancelled", false);
  window->Unlock();
  Require(cancelled && uiOpens == 0 && reads < count, "preflight was not interrupted on worker");
  std::ofstream(source / "saved") << "different";
  reads = 0;
  Require(Send(window, kPrepare), "window did not enqueue colliding transfer");
  Require(acquire_sem_etc(window->done, 1, B_RELATIVE_TIMEOUT, timeout) == B_OK,
          "colliding transfer did not finish");
  window->Lock();
  const bool refused = !window->terminal.GetBool("complete", true) &&
                       !window->terminal.GetBool("cancelled", true);
  window->Unlock();
  std::string retained;
  std::ifstream(protectedFile) >> retained;
  Require(refused && !fs::exists(out / "first") && !fs::exists(out / "saved") &&
              retained == "keep" && uiOpens == 0 && reads > 0,
          "collision was not refused on the worker before publication");
  printf("PASS %d entries, disjoint and colliding: zero UI tree opens; "
         "window handled Cancel during worker enumeration; no roots published\n", count);
}
}

extern "C" DIR *__real_opendir(const char *);
extern "C" dirent *__real_readdir(DIR *);
extern "C" int __real_closedir(DIR *);
extern "C" DIR *__wrap_opendir(const char *path) {
  DIR *dir = __real_opendir(path);
  const std::string_view name(path), suffix("/in/set");
  if (name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix) {
    watched = dir;
    if (find_thread(nullptr) == uiThread)
      ++uiOpens;
  }
  return dir;
}
extern "C" dirent *__wrap_readdir(DIR *dir) {
  auto entry = __real_readdir(dir);
  if (entry && dir == watched && ++reads == 16 && pauseScan.exchange(false)) {
    release_sem(scanPaused);
    acquire_sem_etc(resumeScan, 1, B_RELATIVE_TIMEOUT, timeout);
  }
  return entry;
}
extern "C" int __wrap_closedir(DIR *dir) {
  if (dir == watched)
    watched = nullptr;
  return __real_closedir(dir);
}

int main() {
  BApplication app("application/x-vnd.Listi-TransferPreflight");
  try {
    const std::string selected = "sftp://FIXTURE:22/%77ork";
    for (auto action : {Action::Copy, Action::Move}) {
      for (auto target : {"sftp://fixture/work", "sftp://fixture/work/sub"}) {
        bool refused = false;
        try {
          PrepareTransfer({action, {"sftp://fixture/document", selected}, target});
        } catch (const std::runtime_error &error) {
          refused = std::string_view(error.what()).find("contains the destination") != std::string_view::npos;
        }
        Require(refused, "overlapping network selection must be refused at submission");
      }
      PrepareTransfer({action, {"sftp://fixture/document", selected}, "sftp://fixture/workcopy"});
    }
    puts("PASS network preflight rejects component overlap, not a shared prefix");
    Fixture fixture;
    scanPaused = create_sem(0, "preflight paused");
    resumeScan = create_sem(0, "resume preflight");
    auto window = new TransferWindow;
    window->Show();
    uiThread = window->Thread();
    for (int count : {128, 4096})
      RunCase(window, fixture.root / std::to_string(count), count);
    window->Lock();
    window->Quit();
    delete_sem(scanPaused);
    delete_sem(resumeScan);
    return 0;
  } catch (const std::exception &error) {
    fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
  }
}
