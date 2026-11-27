// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "MainWindowMenu.h"
#include "ClipboardFlow.h"
#include "FileClipboard.h"
#include "FileDrop.h"
#include "FlatView.h"
#include "MainWindow.h"
#include "PanelTools.h"
#include "WorkspaceSplit.h"
#include "SavedSearches.h"
#include <MessageFilter.h>
#include <ColumnListView.h>
#include "FileOperationFlow.h"
#include "InlineAction.h"
#include "JobSnapshot.h"
#include "OperationQueue.h"
#include "Pane.h"
#include "VirtualFile.h"
#include <Application.h>
#include <LayoutBuilder.h>
#include <Window.h>
#include <atomic>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace Listi;
namespace fs = std::filesystem;
namespace {
std::string notice, countedDirectory;
std::atomic<bool> counting{false};
std::atomic<int> directoryReads{0};

void Check(bool ok, const std::string &label) {
  if (!ok) throw std::runtime_error(label);
  std::cout << "PASS " << label << std::endl;
}
std::string Bytes(const fs::path &file) {
  std::ifstream input(file, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot read " + file.string());
  return {std::istreambuf_iterator<char>(input), {}};
}
std::string Member(const std::string &folder, const std::string &name) {
  std::string value;
  ReadFile(ChildLocation(folder, name), [&](const char *data, size_t size) { value.append(data, size); });
  return value;
}
using Contents = std::map<std::string, std::string>;
Contents Files(const std::string &folder, const std::string &prefix = {}) {
  Contents contents;
  for (const auto &entry : ListFiles(folder)) {
    const auto path = prefix + entry.name;
    if (entry.directory) {
      const auto children = Files(entry.path, path + "/");
      contents.emplace(path + "/", "");
      contents.insert(children.begin(), children.end());
    } else
      contents.emplace(path, Member(folder, entry.name));
  }
  return contents;
}
void CreateZip(const fs::path &work, const fs::path &archive, const std::string &value) {
  fs::create_directories(work / "sub");
  for (const auto *name : {"note.txt", "sub/note.txt", "sub/kept", "kept"}) {
    std::ofstream out(work / name);
    out << value << name;
    if (!out) throw std::runtime_error("Cannot make source");
  }
  auto job = PrepareArchiveJob(false, {(work / "note.txt").string(), (work / "kept").string(),
                                       (work / "sub").string()});
  SetArchiveDestination(job, archive.parent_path().string(), archive.filename().string());
  auto result = Run(job, {});
  if (!result.errors.empty()) throw std::runtime_error(result.errors.front());
}
bool WaitForPane(BWindow *window, Pane *pane, const std::string &path) {
  const auto deadline = system_time() + 15000000;
  while (pane->IsScanning() && system_time() < deadline) {
    window->Unlock();
    snooze(20000);
    window->Lock();
  }
  return !pane->IsScanning() && pane->LocationValid() && path == pane->Path();
}
template <typename F> bool Stale(F action) {
  try { action(); }
  catch (const std::exception &error) {
    return std::string(error.what()).find("Reload") != std::string::npos ||
           std::string(error.what()).find("changed") != std::string::npos;
  }
  return false;
}
class BrowserReplies : public BMessageFilter {
public:
  BrowserReplies() : BMessageFilter(B_ANY_DELIVERY, B_ANY_SOURCE) {}
  std::map<std::string, int> miller;
  std::function<void()> tryLocalMove;
  filter_result Filter(BMessage *message, BHandler **recipient) override {
    if (message->what == 'tMov') {
      if (tryLocalMove) tryLocalMove();
      return B_SKIP_MESSAGE;
    }
    if (message->what == kInlineNotice)
      notice = message->GetString("error", "");
    if (message->what == kScanDone && message->HasString("archiveVersion") &&
        *recipient && (*recipient)->Name() && std::string((*recipient)->Name()) == "Miller columns")
      ++miller[message->GetString("path", "")];
    return B_DISPATCH_MESSAGE;
  }
};
template <typename F> bool Until(BWindow *window, F ready) {
  const auto deadline = system_time() + 10000000;
  while (!ready() && system_time() < deadline) {
    window->Unlock();
    snooze(20000);
    window->Lock();
  }
  return ready();
}
BButton *PromptButton(BWindow *window, const std::string &label) {
  const auto question = window->FindView("question");
  if (!question) return nullptr;
  auto *parent = question->Parent();
  auto *buttons = parent->ChildAt(parent->CountChildren() - 1);
  for (int i = 0; i < buttons->CountChildren(); ++i) {
    auto *button = dynamic_cast<BButton *>(buttons->ChildAt(i));
    if (button && label == button->Label()) return button;
  }
  return nullptr;
}
void LocalDestinationCases(BWindow *window, WorkspaceSplit &split, BrowserReplies &replies,
                           const fs::path &root) {
  enum Route { PanelMove, FlatMove, PasteCut, PasteMixed, PasteArchive };
  for (auto route : {PanelMove, FlatMove, PasteCut, PasteMixed, PasteArchive}) {
    const bool flat = route == FlatMove, paste = route >= PasteCut;
    auto &source = split.Panel(0);
    auto &destination = split.Panel(1).Files();
    const auto work = root / ("local-route-" + std::to_string(route));
    const auto inputs = work / "inputs", shown = work / "shown", held = work / "held";
    fs::create_directories(inputs);
    fs::create_directory(shown);
    const auto input = inputs / "incoming.txt";
    std::ofstream(input) << "original bytes";
    source.Select(PanelPage::Files);
    source.Files().Navigate(inputs.c_str());
    destination.Navigate(shown.c_str());
    Check(WaitForPane(window, &source.Files(), inputs.string()) &&
              WaitForPane(window, &destination, shown.string()), "local transfer panels loaded");
    split.SetSource(0);
    if (flat) {
      BMessage open(kFlatView);
      window->MessageReceived(&open);
      Check(Until(window, [&] {
        auto *rows = dynamic_cast<BColumnListView *>(source.FindView("flat results"));
        return rows && rows->CountRows() == 1;
      }), "flat local source loaded");
      auto *rows = static_cast<BColumnListView *>(source.FindView("flat results"));
      rows->MakeFocus(true);
      rows->SetFocusRow(rows->RowAt(0), true);
      Check(FlatSelection(*source.Page(PanelPage::Flat)).sources ==
                std::vector<std::string>{input.string()}, "flat Move selects the intended local file");
    } else {
      Check(source.Files().MatchPattern("incoming.txt", true) == 1, "local source selected");
    }
    const auto extra = inputs / "copied.txt";
    std::string archiveView;
    if (route == PasteArchive) {
      const auto archive = work / "source.zip";
      CreateZip(work / "zip-input", archive, "original ");
      archiveView = OpenArchive(archive.string());
      PutFileClipboard({ChildLocation(archiveView, "note.txt")}, false);
    } else if (route == PasteMixed) {
      std::ofstream(extra) << "retained bytes";
      auto selection = MakeFileSelection({input.string(), extra.string()}, true);
      for (const auto &item : ReadFileClipboard(selection)) {
        if (item.path == extra.string()) {
          auto mode = item.key;
          mode[0] = 'm';
          Check(selection.ReplaceInt32(mode.c_str(), 'Tcsl') == B_OK, "set Tracker copy mode");
        }
      }
      Check(be_clipboard->Lock(), "open Tracker clipboard");
      be_clipboard->Clear();
      *be_clipboard->Data() = selection;
      const auto committed = be_clipboard->Commit();
      be_clipboard->Unlock();
      Check(committed == B_OK, "publish mixed Tracker selection");
    } else if (route == PasteCut) {
      BMessage cut(kClipboardCut);
      window->MessageReceived(&cut);
    }
    auto submit = [&](bool replace) {
      // Run on the window looper: inspect the displayed identity, swap the
      // folder and dispatch the real command before a refresh can intervene.
      bool dispatched = false;
      struct stat expected;
      Check(stat(shown.c_str(), &expected) == 0, "read intended local target identity");
      replies.tryLocalMove = [&] {
        if (dispatched || destination.IsScanning() || !destination.LocationValid()) return;
        const auto [device, node] = destination.FolderIdentity();
        if (device != expected.st_dev || node != expected.st_ino) return;
        dispatched = true;
        split.SetSource(paste ? 1 : 0);
        if (replace) {
          fs::rename(shown, held);
          fs::create_directory(shown);
        }
        BMessage move(paste ? kClipboardPaste : kMove);
        window->MessageReceived(&move);
      };
      const bool delivered = Until(window, [&] {
        if (!dispatched) window->PostMessage('tMov');
        return dispatched;
      });
      replies.tryLocalMove = {};
      Check(delivered, "dispatch Move from a completed destination listing");
    };
    notice.clear();
    submit(true);
    const bool refused = Until(window, [&] { return !notice.empty(); });
    if (!refused || notice.find("destination folder was replaced") == std::string::npos)
      throw std::runtime_error("Missing stale-target refusal. Notice: " + notice +
                               "; source panel " + std::to_string(split.Source()) +
                               "; destination " + destination.Path());
    Check(Bytes(input) == "original bytes" && fs::is_empty(shown) && fs::is_empty(held) &&
              (route != PasteMixed || Bytes(extra) == "retained bytes") &&
              (route != PasteArchive || Member(archiveView, "note.txt") == "original note.txt"),
          "Move refuses the replacement and leaves original and both destinations untouched");
    destination.Reload();
    Check(WaitForPane(window, &destination, shown.string()), "explicitly reload local destination");
    submit(false);
    if (route == PasteArchive) {
      Check(Until(window, [&] { return fs::exists(shown / "note.txt"); }) &&
                Bytes(shown / "note.txt") == "original note.txt" &&
                Member(archiveView, "note.txt") == "original note.txt" && fs::is_empty(held),
            "archive Paste after reload keeps source and copies only to the refreshed folder");
      continue;
    }
    Check(Until(window, [&] { return !fs::exists(input) && fs::exists(shown / "incoming.txt"); }) &&
              Bytes(shown / "incoming.txt") == "original bytes" && fs::is_empty(held),
          "Move after reload delivers only to the newly displayed destination");
    if (route == PasteMixed)
      Check(Bytes(extra) == "retained bytes" && Bytes(shown / "copied.txt") == "retained bytes",
            "mixed Tracker Paste retains the copy source and delivers both jobs after reload");
    if (flat) source.CloseCurrent();
  }
}
void InboundCases(const fs::path &root) {
  Preferences prefs;
  prefs.workspace = Workspace::tviskipt;
  auto *window = CreateMainWindow(prefs, {}, {}, false, {}, false);
  auto *replies = new BrowserReplies;
  window->AddCommonFilter(replies);
  window->Show();
  window->Lock();
  auto *panel = dynamic_cast<PanelTools *>(window->FindView("first panel tools"));
  auto *split = panel ? dynamic_cast<WorkspaceSplit *>(panel->Parent()) : nullptr;
  Check(split, "dual-pane browser opened");
  LocalDestinationCases(window, *split, *replies, root);
  enum Route { FlatCopy, FlatMove, Details, Grid, MillerAncestor };
  for (auto route : {FlatCopy, FlatMove, Details, Grid, MillerAncestor}) {
    const auto work = root / ("inbound-" + std::to_string(route)), inputs = work / "inputs";
    fs::create_directories(inputs);
    const auto input = inputs / "incoming.txt";
    std::ofstream(input) << "incoming bytes";
    const auto archive = work / "shown.zip", replacement = work / "next.zip", held = work / "old.zip";
    CreateZip(work / "old", archive, "old ");
    CreateZip(work / "new", replacement, "new ");
    const auto url = OpenArchive(archive.string()), folder = ChildLocation(url, "sub");
    const auto oldBytes = Bytes(archive), newBytes = Bytes(replacement);
    const auto oldFiles = Files(url), newFiles = Files(OpenArchive(replacement.string()));
    const int source = int(route) % 2;
    auto &from = split->Panel(source), &to = split->Panel(1 - source);
    from.Select(PanelPage::Files);
    to.Select(PanelPage::Files);
    auto &destination = to.Files();
    from.Files().Navigate(inputs.c_str());
    const bool flat = route == FlatCopy || route == FlatMove;
    if (!flat) {
      // Set the view before opening the fixture. Switching view while inside
      // an archive starts and then replaces its initial secondary scan.
      destination.Navigate(inputs.c_str());
      Check(WaitForPane(window, &destination, inputs.string()), "ready to choose receiving view");
      BMessage mode(kGridMode);
      mode.AddInt32("mode", route == Details ? 0 : route == Grid ? 1 : MillerMode);
      destination.MessageReceived(&mode);
      Check(WaitForPane(window, &destination, inputs.string()), "receiving view switched");
    }
    destination.Navigate(folder.c_str());
    Check(WaitForPane(window, &from.Files(), inputs.string()) &&
              WaitForPane(window, &destination, folder), "both inbound panels loaded");
    if (flat) {
      split->SetSource(source);
      BMessage open(kFlatView);
      window->MessageReceived(&open);
      Check(Until(window, [&] {
        auto *rows = dynamic_cast<BColumnListView *>(from.FindView("flat results"));
        return rows && rows->CountRows() == 1;
      }), "Flat View scanned local source");
      auto *rows = static_cast<BColumnListView *>(from.FindView("flat results"));
      rows->SetFocusRow(rows->RowAt(0), true);
    } else if (route == MillerAncestor) {
      Check(Until(window, [&] { return replies->miller[url] > 0; }), "receiving Miller ancestor was listed");
    }
    auto submit = [&] {
      split->SetSource(source); // Destination is not the active pane.
      if (flat) {
        window->PostMessage(route == FlatMove ? kMove : kCopy);
        return;
      }
      BMessage drag(B_SIMPLE_DATA);
      if (route == Grid)
        drag = MakeFileDrag({input.string()});
      else {
        entry_ref ref;
        Check(get_ref_for_path(input.c_str(), &ref) == B_OK && drag.AddRef("refs", &ref) == B_OK,
              "external Tracker-compatible refs without private selection fields");
      }
      // The source does not get to nominate the receiver's archive or proof.
      drag.AddString("destination", inputs.c_str());
      drag.AddString("listi:destination-archive-version", "untrusted");
      if (route == Details) {
        auto *list = dynamic_cast<BColumnListView *>(destination.FindView("files"));
        Check(list != nullptr, "details receiving surface");
        list->MessageDropped(&drag, BPoint(-1, -1));
      } else {
        BView *surface = destination.FindView("file grid");
        if (route == MillerAncestor) {
          auto *columns = destination.FindView("Miller columns");
          surface = nullptr;
          for (int i = 0; columns && columns->ChildAt(i); ++i) {
            auto *col = columns->ChildAt(i);
            auto *heading = dynamic_cast<BStringView *>(col->FindView("Miller heading"));
            if (heading && archive.filename().string() == heading->Text())
              surface = col->FindView("Miller rows");
          }
        }
        Check(surface != nullptr, "correct grid or secondary Miller drop surface");
        drag.AddBool("test-delivered-drop", true);
        surface->MessageReceived(&drag);
      }
    };
    fs::rename(archive, held);
    fs::rename(replacement, archive);
    notice.clear();
    submit();
    Check(Until(window, [&] { return notice.find("Reload") != std::string::npos; }) &&
              !PromptButton(window, "Update archive") && !PromptButton(window, "Move entries"),
          "stale archive destination refuses delivery and confirmation");
    Check(Bytes(input) == "incoming bytes" && Bytes(archive) == newBytes && Bytes(held) == oldBytes &&
              Files(url) == newFiles && Files(OpenArchive(held.string())) == oldFiles,
          "both archives, all members and incoming source unchanged");
    const auto before = replies->miller[url];
    destination.Reload();
    Check(WaitForPane(window, &destination, folder), "destination reload succeeds");
    if (route == MillerAncestor)
      Check(Until(window, [&] { return replies->miller[url] > before; }), "Miller ancestor reloaded");
    submit();
    const char *label = route == FlatMove ? "Move entries" : "Update archive";
    Check(Until(window, [&] { return PromptButton(window, label) != nullptr; }),
          "refreshed incoming action reaches confirmation");
    Check(PromptButton(window, label)->Invoke() == B_OK, "accept refreshed action");
    const auto receiving = route == MillerAncestor ? url : folder;
    Check(Until(window, [&] {
      try {
        return Member(receiving, "incoming.txt") == "incoming bytes" &&
               (route != FlatMove || !fs::exists(input));
      } catch (const std::exception &) { return false; }
    }), "refreshed action delivers bytes to the receiving column");
    auto expected = newFiles;
    expected[route == MillerAncestor ? "incoming.txt" : "sub/incoming.txt"] = "incoming bytes";
    Check(Files(url) == expected && Bytes(held) == oldBytes &&
              (route == FlatMove ? !fs::exists(input) : Bytes(input) == "incoming bytes"),
          "only accepted Move removes source; archive retains existing members");
    if (flat) from.CloseCurrent();
  }
  window->Quit();
}
}
// Supply only the app_server's drop bit; the real view resolves the target,
// binds its listing, and posts through the normal window confirmation route.
extern "C" bool __real__ZNK8BMessage10WasDroppedEv(const BMessage *);
extern "C" bool __wrap__ZNK8BMessage10WasDroppedEv(const BMessage *message) {
  return message->GetBool("test-delivered-drop", false) || __real__ZNK8BMessage10WasDroppedEv(message);
}
extern "C" DIR *__real_opendir(const char *);
extern "C" DIR *__wrap_opendir(const char *path) {
  if (counting && countedDirectory == path) ++directoryReads;
  return __real_opendir(path);
}
extern "C" void __wrap__Z20PostMainWindowNoticePKc(const char *text) { notice = text; }

int main() {
  BApplication app("application/x-vnd.Listi-ArchivePanelVersionTest");
  char temp[] = "/tmp/Listi-archive-view-XXXXXX";
  if (!mkdtemp(temp)) return 2;
  const auto root = fs::canonical(temp), out = root / "out";
  fs::create_directory(out);
  Preferences prefs;
  auto *source = new Pane(0, prefs), *dest = new Pane(1, prefs);
  auto *window = new BWindow(BRect(0, 0, 900, 480), "Archive version", B_TITLED_WINDOW, 0);
  auto *question = new InlineAction(BMessenger(window));
  BLayoutBuilder::Group<>(window, B_VERTICAL)
      .AddGroup(B_HORIZONTAL).Add(source).Add(dest).End().Add(question);
  window->Show();
  window->Lock();
  int status = 0;
  try {
    OperationQueue queue{BMessenger(window)};
    ClipboardFlow clipboard(*question, queue);
    FileOperationFlow operations(*question, queue, clipboard, BMessenger(window));
    BClipboard privateClip("Listi-archive-view-version-test");
    dest->Navigate(out.c_str());
    Check(WaitForPane(window, dest, out.string()), "local destination opens");
    auto select = [&](const std::string &folder) {
      source->Navigate(folder.c_str());
      Check(WaitForPane(window, source, folder) && !source->ExportModel().archiveVersion.empty() &&
                source->MatchPattern("note.txt", true) == 1, "select displayed archive member");
      // A preceding copy may have triggered a watched-folder reload. Finish
      // it before swapping a source; these commands test the source, not the
      // independent rejection of an incomplete local destination.
      Check(WaitForPane(window, dest, out.string()), "local destination ready for stale source check");
    };
    auto rejectCommands = [&] {
      const auto openWindows = app.CountWindows();
      notice.clear();
      Check(source->HandleOpenCommand(kEditExternally, "", {}, {}) &&
                notice.find("Reload") != std::string::npos && app.CountWindows() == openWindows,
            "stale external edit rejected without opening an editor");
      const auto clipboardCount = be_clipboard->SystemCount();
      for (auto cmd : {kClipboardCopy, kClipboardCut}) {
        notice.clear();
        clipboard.Begin(cmd, *source);
        Check(notice.find("Reload") != std::string::npos &&
                  be_clipboard->SystemCount() == clipboardCount,
              "stale clipboard command rejected without replacing clipboard");
      }
      for (auto cmd : {kCopy, kMove, kRemoveArchive, kMkdir}) {
        notice.clear();
        const bool handled = operations.Handle(BMessage(cmd), *source, *dest, false);
        Check(handled && notice.find("Reload") != std::string::npos && !queue.Busy() && !question->Pending(),
              "stale pane command rejected before confirmation or queueing (" +
                  std::to_string(cmd) + ": " + notice + ")");
      }
    };
    for (bool nested : {false, true}) {
      const auto work = root / (nested ? "nested" : "root");
      fs::create_directory(work);
      const auto archive = work / "shown.zip", fresh = work / "next.zip", held = work / "old.zip";
      CreateZip(work / "old", archive, "old ");
      CreateZip(work / "new", fresh, "new ");
      const std::string oldBytes = Bytes(archive), newBytes = Bytes(fresh);
      const auto url = OpenArchive(archive.string());
      const auto oldFiles = Files(url), newFiles = Files(OpenArchive(fresh.string()));
      const auto folder = nested ? ChildLocation(url, "sub") : url;
      select(folder);
      Job shown;
      source->ExportModel().BindArchiveView(shown, source->Path());
      PutFileClipboard(source->Selected(), false, &privateClip, {}, shown.expectedArchiveSets);
      const auto drag = MakePaneFileDrag(source->ExportModel());
      fs::rename(archive, held);
      fs::rename(fresh, archive);
      rejectCommands();
      const auto otherFolder = nested ? url : ChildLocation(url, "sub");
      dest->Navigate(otherFolder.c_str());
      Check(WaitForPane(window, dest, otherFolder) &&
                dest->ExportModel().archiveVersion != source->ExportModel().archiveVersion,
            "two panels show different revisions of the same archive");
      for (auto command : {kCopy, kMove}) {
        notice.clear();
        Check(operations.Handle(BMessage(command), *source, *dest, false) &&
                  notice.find("Reload") != std::string::npos && !queue.Busy() && !question->Pending(),
              "same physical archive cannot merge inconsistent panel revisions");
      }
      dest->Navigate(out.c_str());
      Check(WaitForPane(window, dest, out.string()), "return to local destination");
      Check(Stale([&] { PrepareFilePaste(out.string(), &privateClip); }) &&
                Stale([&] { PrepareFileDrop(drag, out.string()); }),
            "clipboard and drag captured before replacement remain bound");
      Check(fs::is_empty(out) && Bytes(held) == oldBytes && Bytes(archive) == newBytes &&
                Files(OpenArchive(held.string())) == oldFiles && Files(url) == newFiles,
            "neither container, output nor unselected member changed");
      if (nested) source->Navigate(folder.c_str()); else source->Reload();
      Check(WaitForPane(window, source, folder) && source->Selected().empty(),
            "refresh does not select the replacement namesake");
      Check(source->MatchPattern("note.txt", true) == 1, "explicitly select refreshed row");
      auto job = PrepareFileDrop(MakePaneFileDrag(source->ExportModel()), out.string());
      const auto copied = Run(job, {});
      Check(copied.errors.empty() && copied.completed == 1 &&
                Bytes(out / "note.txt") == (nested ? "new sub/note.txt" : "new note.txt"),
            "refreshed Copy transfers the displayed replacement");
      fs::remove(out / "note.txt");
      ArchiveActions actions;
      actions.InspectRemoval(*source, BMessenger(window));
      Job command, target;
      bool prepared = false;
      Check(Until(window, [&] {
        if (actions.Inspecting())
          prepared = actions.PollInspection(*source, command, target);
        return !actions.Inspecting();
      }) && prepared, "removal inspection accepts the refreshed selection");
      const auto prompt = ArchiveActions::ConfirmRemoval(target, command.sources.size());
      const auto removed = Run(DecodeJob(prompt.accepted), {});
      auto expected = newFiles;
      expected.erase(nested ? "sub/note.txt" : "note.txt");
      Check(removed.errors.empty() && Files(url) == expected &&
                Files(OpenArchive(held.string())) == oldFiles && Bytes(held) == oldBytes,
            "refreshed Remove affects only the new selected member");
    }
    InboundCases(root);
    const auto parts = root / "parts";
    const fs::path fixture("tests/fixtures/multipart");
    fs::create_directory(parts);
    std::vector<fs::path> volumes;
    for (int n = 1; n <= 4; ++n) {
      const auto name = "plain.7z.00" + std::to_string(n);
      fs::copy_file(fixture / name, parts / name);
      volumes.push_back(parts / name);
    }
    const auto url = OpenArchive(volumes[0].string());
    select(url);
    fs::copy_file(volumes[1], parts / "new-part");
    fs::rename(volumes[1], parts / "held-part");
    fs::rename(parts / "new-part", volumes[1]);
    rejectCommands();
    source->Reload();
    Check(WaitForPane(window, source, url) && source->Selected().empty() &&
              source->MatchPattern("note.txt", true) == 1, "split archive refresh requires reselection");
    auto refreshed = PrepareFileDrop(MakePaneFileDrag(source->ExportModel()), out.string());
    auto copied = Run(refreshed, {});
    Check(copied.errors.empty() && copied.completed == 1 &&
              Bytes(out / "note.txt") == Bytes(fixture / "original/note.txt") &&
              Member(url, "payload.bin") == Bytes(fixture / "original/payload.bin"),
          "refreshed multipart copy succeeds and unselected payload survives");
    for (const auto &volume : volumes)
      Check(Bytes(volume) == Bytes(fixture / volume.filename()), "unchanged multipart volume bytes");
    countedDirectory = parts.string();
    auto reads = [&](std::vector<std::string> paths) {
      Job job{Action::Copy, std::move(paths), out.string()};
      directoryReads = 0;
      counting = true;
      FreezeArchiveTargets(job);
      counting = false;
      return directoryReads.load();
    };
    auto first = ChildLocation(url, "note.txt"), second = ChildLocation(url, "payload.bin");
    const int one = reads({first});
    Check(one > 0 && reads({first, second}) == one,
          "one source archive inspection regardless of selected member count");
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << std::endl;
    status = 1;
  }
  window->Quit();
  fs::remove_all(root);
  return status;
}
