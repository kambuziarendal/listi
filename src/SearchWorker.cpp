// SPDX-License-Identifier: MIT
#include "ReadWorker.h"
#include "UnavailableVolume.h"
#include "SearchWorker.h"
#include "FileSearch.h"
#include <MessageRunner.h>
#include <NodeMonitor.h>
#include <Query.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdlib>
namespace Listi {
namespace {
constexpr uint32 kRefresh = 'sref', kReconcile = 'srec';
class SearchWorker : public Listi::ReadWorker {
  BMessenger target;
  std::shared_ptr<std::atomic<int64>> generation;
  std::shared_ptr<std::atomic<int64>> failedSerial;
  std::unique_ptr<BQuery> query;
  std::unique_ptr<BMessageRunner> debounce, repair;
  SearchRequest request;
  SearchHit root;
  int64 serial = -1, revision = 0, events = 0;
  bigtime_t lastScan = 0;
  bool paneRows = false;
  bool Alive() const { return generation->load() == serial; }
  void Stop() {
    debounce.reset();
    repair.reset();
    query.reset();
  }
  bool Send(BMessage &m) {
    if (!Alive()) return false;
    if (m.AddInt64("serial", serial) == B_OK &&
        m.AddInt64("revision", revision) == B_OK &&
        target.SendMessage(&m, (BHandler *)nullptr, 500000) == B_OK) return true;
    if (failedSerial) failedSerial->store(serial);
    return false;
  }
  void Scan(bool initial) {
    if (!Alive()) {
      Stop();
      return;
    }
    ++revision;
    lastScan = system_time();
    BMessage begin(kSearchBegin);
    begin.AddBool("initial", initial);
    if (!Send(begin)) {
      Stop();
      return;
    }
    BMessage batch(kSearchRows);
    int count = 0;
    int64 total = 0;
    bool buildFailed = false;
    auto flush = [&] {
      if (!count)
        return true;
      bool ok = Send(batch);
      batch.MakeEmpty();
      batch.what = kSearchRows;
      count = 0;
      return ok;
    };
    SearchSummary summary;
    size_t unverifiedHits = 0;
    if (Listi::InterruptedVolumes().Contains(request.root))
      summary.error = Listi::kUnavailableVolumeText;
    else if (!initial && !SameSearchEntry(root))
      summary.error =
          "Search folder moved, disappeared or was replaced. Start a new search.";
    else
      summary = SearchFiles(
          request, [&] { return Alive(); },
          [&](const SearchHit &hit) {
            if (Listi::InterruptedVolumes().Contains(hit.path)) {
              ++unverifiedHits;
              return true;
            }
            BMessage item;
            if (paneRows) {
              struct stat st;
              if (lstat(hit.path.c_str(), &st) != 0) {
                // A vanished path no longer matches. Other errors leave an
                // existing hit unverified and must mark this snapshot partial.
                if (errno != ENOENT && errno != ENOTDIR)
                  ++unverifiedHits;
                return true;
              }
              if (st.st_dev != hit.device || st.st_ino != hit.node)
                return true;
              struct stat target;
              const bool directory = S_ISDIR(st.st_mode) ||
                  (S_ISLNK(st.st_mode) && !Listi::InterruptedVolumes().ContainsResolved(hit.path) &&
                   stat(hit.path.c_str(), &target) == 0 && S_ISDIR(target.st_mode));
              buildFailed = item.AddInt64("size", st.st_size) != B_OK ||
                  item.AddInt64("date", st.st_mtime) != B_OK ||
                  item.AddInt32("kind", S_ISLNK(st.st_mode) ? 2 : S_ISDIR(st.st_mode) ? 1 : 0) != B_OK ||
                  item.AddBool("directory", directory) != B_OK;
            } else if (request.flat) {
              buildFailed = item.AddInt64("size", hit.size) != B_OK ||
                            item.AddInt64("date", hit.modified) != B_OK;
            }
            if (buildFailed || item.AddString("path", hit.path.c_str()) != B_OK ||
                item.AddInt64("device", hit.device) != B_OK ||
                item.AddInt64("node", hit.node) != B_OK ||
                (!hit.relativeFolder.empty() && item.AddString("folder", hit.relativeFolder.c_str()) != B_OK) ||
                batch.AddMessage("hit", &item) != B_OK) {
              buildFailed = true;
              return false;
            }
            ++total;
            return ++count < 40 || flush();
          },
          initial ? query.get() : nullptr);
    summary.skipped += unverifiedHits;
    if (!Alive()) {
      Stop();
      return;
    }
    if (buildFailed)
      summary.error = "Could not build the search result batch. Reload to retry.";
    if (!flush())
      summary.cancelled = true;
    if (!summary.error.empty() || summary.cancelled || summary.limited ||
        summary.skipped)
      Stop();
    BMessage done(kSearchDone);
    if (done.AddInt64("events", events) != B_OK ||
        done.AddInt64("visited", summary.visited) != B_OK ||
        done.AddInt64("skipped", summary.skipped) != B_OK ||
        done.AddBool("limited", summary.limited) != B_OK ||
        done.AddBool("cancelled", summary.cancelled) != B_OK ||
        done.AddBool("live", query != nullptr) != B_OK ||
        done.AddString("error", summary.error.c_str()) != B_OK ||
        done.AddInt64("rowTotal", total) != B_OK) {
      if (failedSerial) failedSerial->store(serial);
      Stop();
      return;
    }
    if (!Send(done))
      Stop();
  }

public:
  SearchWorker(BMessenger receiver, std::shared_ptr<std::atomic<int64>> token,
               std::shared_ptr<std::atomic<int64>> failure)
      : ReadWorker("file search"), target(receiver), generation(token), failedSerial(failure) {}
  void MessageReceived(BMessage *m) override {
    switch (m->what) {
    case kSearchStart: {
      Stop();
      serial = m->GetInt64("serial", -1);
      paneRows = m->GetBool("paneRows", false);
      revision = 0;
      events = 0;
      if (!Alive())
        return;
      try {
        request = SearchRequest::ReadCriteria(*m);
        request.flat = m->GetBool("flat", false);
      } catch (const std::exception &e) {
        BMessage failed(kSearchDone);
        failed.AddString("error", e.what());
        Send(failed);
        return;
      }
      if (request.predicate && m->GetBool("live", false) &&
          !Listi::InterruptedVolumes().ContainsResolved(request.root)) {
        char path[PATH_MAX];
        struct stat st;
        if (realpath(request.root.c_str(), path) && !stat(path, &st)) {
          request.root = path;
          root = {path, st.st_dev, st.st_ino};
          query.reset(new BQuery);
          if (query->SetTarget(BMessenger(this)) != B_OK) {
            BMessage failed(kSearchDone);
            failed.AddString("error",
                             "Could not subscribe to live BFS results.");
            Send(failed);
            Stop();
            return;
          }
        }
      }
      Scan(true);
      if (query) {
        // Query notifications do not cover every ancestor-directory move or
        // relative-time transition. Reconcile periodically as a bounded repair.
        BMessage tick(kReconcile);
        tick.AddInt64("serial", serial);
        repair.reset(new BMessageRunner(BMessenger(this), &tick, 3000000));
        if (repair->InitCheck() != B_OK) {
          BMessage failed(kSearchDone);
          failed.AddString("error", "Could not schedule live reconciliation.");
          Stop();
          Send(failed);
        }
      }
      return;
    }
    case kSearchStop:
      if (m->GetInt64("serial", -1) == serial || !Alive())
        Stop();
      return;
    case B_QUERY_UPDATE:
      ++events;
      if (!Alive()) {
        Stop();
        return;
      }
      if (query && !debounce) {
        BMessage tick(kRefresh);
        tick.AddInt64("serial", serial);
        debounce.reset(new BMessageRunner(BMessenger(this), &tick, 750000, 1));
      }
      return;
    case kRefresh:
    case kReconcile:
      if (m->GetInt64("serial", -1) != serial)
        return;
      if (!Alive()) {
        Stop();
        return;
      }
      if (m->what == kRefresh)
        debounce.reset();
      if (query && system_time() - lastScan >= 500000)
        Scan(false);
      return;
    default:
      BLooper::MessageReceived(m);
      return;
    }
  }
};
}
ReadWorker *NewSearchWorker(BMessenger target,
                         std::shared_ptr<std::atomic<int64>> generation,
                         std::shared_ptr<std::atomic<int64>> failure) {
  return new SearchWorker(target, generation, failure);
}
}
