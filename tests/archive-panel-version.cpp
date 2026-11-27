// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "ClipboardFlow.h"
#include "FileClipboard.h"
#include "FileDrop.h"
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
    };
    auto rejectCommands = [&] {
      const auto clipboardCount = be_clipboard->SystemCount();
      for (auto cmd : {kClipboardCopy, kClipboardCut}) {
        notice.clear();
        clipboard.Begin(cmd, *source);
        Check(notice.find("Reload") != std::string::npos &&
                  be_clipboard->SystemCount() == clipboardCount,
              "stale clipboard command rejected without replacing clipboard");
      }
      for (auto cmd : {kCopy, kMove, kRemoveArchive}) {
        notice.clear();
        Check(operations.Handle(BMessage(cmd), *source, *dest, false) &&
                  notice.find("Reload") != std::string::npos && !queue.Busy() && !question->Pending(),
              "stale pane command rejected before confirmation or queueing");
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
      const auto removed = Run(DecodeJob(ArchiveActions::ConfirmRemoval(*source).accepted), {});
      auto expected = newFiles;
      expected.erase(nested ? "sub/note.txt" : "note.txt");
      Check(removed.errors.empty() && Files(url) == expected &&
                Files(OpenArchive(held.string())) == oldFiles && Bytes(held) == oldBytes,
            "refreshed Remove affects only the new selected member");
    }
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
