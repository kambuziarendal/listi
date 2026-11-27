// SPDX-License-Identifier: MIT
#include "ArchiveEditor.h"
#include "ArchiveJobs.h"
#include "ArchivePasswords.h"
#include "ArchiveVolumes.h"
#include "VirtualFile.h"
#include <Application.h>
#include "Pane.h"
#include "MainWindowMenu.h"
#include <Roster.h>
#include <Window.h>
#include <mutex>
#include <Directory.h>
#include <Entry.h>
#include <Path.h>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
using namespace Listi;
namespace {
int checks = 0, failures = 0, serial = 0;
std::string root;
void check(bool ok, const std::string &label) {
  ++checks;
  failures += !ok;
  std::cout << (ok ? "PASS " : "FAIL ") << label << std::endl;
}
void writeFile(const std::string &p, const std::string &s) {
  std::ofstream f(p, std::ios::binary);
  f << s;
  f.close();
}
std::string bytes(const std::string &p) {
  std::string out;
  ReadFile(p, [&](const char *b, size_t n) { out.append(b, n); });
  return out;
}
void clean(const std::string &p) {
  BDirectory d(p.c_str());
  BEntry e;
  while (d.GetNextEntry(&e, false) == B_OK) {
    struct stat s;
    e.GetStat(&s);
    if (S_ISDIR(s.st_mode)) {
      BPath path;
      e.GetPath(&path);
      clean(path.Path());
    }
    e.Remove();
  }
  rmdir(p.c_str());
}
template <class F> bool rejects(F f) {
  try {
    f();
    return false;
  } catch (const std::exception &) {
    return true;
  }
}
std::string archive(const std::string &format, bool encrypted = false) {
  const auto source = root + (format == "ar" ? "/in/folder/edit.txt" : "/in/folder");
  auto job = PrepareArchiveJob(false, {source, root + "/in/keep"});
  job.archiveFormat = format;
  if (encrypted) {
    job.archiveEncryptNames = format == "7z";
    job.archivePasswordToken = HoldArchiveCreationPassword(
        std::make_shared<ArchiveSecret>("Listi-edit-test"));
  }
  SetArchiveDestination(job, root, "archive" + std::to_string(++serial));
  auto r = Run(job, {});
  if (r.completed != 1 || !r.errors.empty())
    throw std::runtime_error("Cannot create editor fixture");
  return OpenArchive(job.destination);
}
std::string member(const std::string &a) {
  return ChildLocation(ChildLocation(a, "folder"), "edit.txt");
}
std::string physical(const std::string &a) { return ParseLocation(a).archive; }
std::string fingerprint(const std::string &a) {
  return FindArchiveVolumes(physical(a)).Fingerprint();
}
// Observe the exact B_REFS_RECEIVED file handed to the external editor,
// without launching or closing an unrelated desktop application in the VM.
std::mutex launchedMutex;
std::string launchedFile;
void EditFromRefreshedPane(const std::string &url, const std::string &savedArchive,
                           const std::string &savedBytes) {
  Preferences prefs;
  auto *pane = new Pane(0, prefs);
  auto *window = new BWindow(BRect(20, 30, 640, 480), "Archive editor test", B_TITLED_WINDOW, 0);
  window->AddChild(pane);
  window->Show();
  window->Lock();
  pane->Navigate(ChildLocation(url, "folder").c_str());
  const auto timeout = system_time() + 10000000;
  while (pane->IsScanning() && system_time() < timeout) {
    window->Unlock();
    snooze(20000);
    window->Lock();
  }
  check(pane->LocationValid() && pane->MatchPattern("edit.txt", true) == 1 &&
            pane->HandleOpenCommand(kEditExternally, "/boot/system/bin/true", {}, {}),
        "open external editor from newly selected archive row");
  window->Unlock();
  std::string work;
  const auto until = system_time() + 15000000;
  while (work.empty() && system_time() < until) {
    snooze(20000);
    std::lock_guard<std::mutex> lock(launchedMutex);
    work = launchedFile;
  }
  if (work.empty()) throw std::runtime_error("external editor never received its working file");
  check(bytes(work) == "intentional edit", "external editor receives refreshed member");
  writeFile(work, "external save");
  const auto limit = system_time() + 20000000;
  while (bytes(member(url)) != "external save" && system_time() < limit)
    snooze(100000);
  check(bytes(member(url)) == "external save" && bytes(ChildLocation(url, "keep")) == "keep" &&
            bytes(savedArchive) == savedBytes, "automatic editor save updates only the chosen archive");
  BMessenger editorWindow;
  for (int32 i = 0; i < be_app->CountWindows(); ++i) {
    auto *other = be_app->WindowAt(i);
    if (other && other != window && !strcmp(other->Title(), "Listi: Edit archive file")) {
      editorWindow = BMessenger(other);
      break;
    }
  }
  editorWindow.SendMessage(B_QUIT_REQUESTED);
  const auto closing = system_time() + 5000000;
  while (editorWindow.IsValid() && system_time() < closing) snooze(20000);
  if (editorWindow.IsValid()) throw std::runtime_error("archive editor did not close");
  clean(ParentLocation(ParentLocation(work)));
  window->Lock();
  window->Quit();
}
} // namespace
extern "C" status_t __wrap__ZNK7BRoster6LaunchEPK9entry_refPK8BMessagePi(
    const BRoster *, const entry_ref *, const BMessage *refs, team_id *) {
  entry_ref ref;
  if (!refs || refs->what != B_REFS_RECEIVED || refs->FindRef("refs", &ref) != B_OK)
    return B_BAD_VALUE;
  BPath path(&ref);
  if (path.InitCheck() != B_OK) return path.InitCheck();
  std::lock_guard<std::mutex> lock(launchedMutex);
  launchedFile = path.Path();
  return B_OK;
}
int main() {
  BApplication app("application/x-vnd.Listi-archive-editor-test");
  char pattern[] = "/tmp/Listi-editor-XXXXXX";
  auto made = mkdtemp(pattern);
  if (!made)
    return 2;
  root = made;
  mkdir((root + "/in").c_str(), 0700);
  mkdir((root + "/in/folder").c_str(), 0700);
  writeFile(root + "/in/folder/edit.txt", "original");
  chmod((root + "/in/folder/edit.txt").c_str(), 0640);
  writeFile(root + "/in/keep", "keep");
  const auto sessions = root + "/sessions";
  try {
    {
      const auto displayed = archive("zip");
      writeFile(root + "/in/folder/edit.txt", "replacement member");
      const auto replacement = archive("zip");
      writeFile(root + "/in/folder/edit.txt", "original");
      const auto originalBytes = bytes(physical(displayed));
      const auto replacementBytes = bytes(physical(replacement));
      const auto saved = physical(displayed) + ".original";
      Job shown;
      shown.expectedArchiveSets[physical(displayed)] = fingerprint(displayed);
      check(rename(physical(displayed).c_str(), saved.c_str()) == 0 &&
                rename(physical(replacement).c_str(), physical(displayed).c_str()) == 0,
            "replace the shown ZIP with a different version");
      const auto path = member(displayed);
      const auto editRoot = root + "/version-edits";
      check(rejects([&] { ArchiveEditSession::Open(path, editRoot, {}, shown); }) &&
                ListArchiveEdits(editRoot).empty() && bytes(saved) == originalBytes &&
                bytes(physical(displayed)) == replacementBytes,
            "stale panel edit leaves both archives untouched and creates no working copy");
      shown.expectedArchiveSets[physical(displayed)] = fingerprint(displayed);
      auto current = ArchiveEditSession::Open(path, editRoot, {}, shown);
      check(bytes(current->WorkingFile()) == "replacement member",
            "fresh panel edit opens the displayed replacement bytes");
      writeFile(current->WorkingFile(), "intentional edit");
      check(current->Save() && bytes(path) == "intentional edit" &&
                bytes(ChildLocation(displayed, "keep")) == "keep" && bytes(saved) == originalBytes,
            "refreshed edit saves only the selected replacement member");
      EditFromRefreshedPane(displayed, saved, originalBytes);
    }
    {
      const auto a = archive("zip"), other = archive("zip");
      const auto archivePath = physical(a), externalPath = physical(other);
      const auto externalBytes = bytes(externalPath);
      auto session = ArchiveEditSession::Open(member(a), sessions);
      const auto saved = Join(session->Directory(), "saved");
      const auto journal = Join(session->Directory(), "session");
      const auto oldJournal = bytes(journal), oldVersion = session->ArchiveVersion();
      writeFile(session->WorkingFile(), "private draft");
      bool replaced = false;
      Hooks replaceAfterPublication;
      replaceAfterPublication.fileDone = [&](const std::string &, const std::string &destination) {
        if (destination == archivePath && bytes(member(a)) == "private draft")
          replaced = rename(externalPath.c_str(), archivePath.c_str()) == 0;
      };
      check(rejects([&] { session->Save(replaceAfterPublication); }) && replaced &&
                bytes(archivePath) == externalBytes && bytes(saved) == "original" &&
                bytes(journal) == oldJournal && session->ArchiveVersion() == oldVersion &&
                bytes(session->WorkingFile()) == "private draft",
            "external replacement before editor receipt keeps the private draft and old journal");
      writeFile(session->WorkingFile(), "later private draft");
      check(rejects([&] { session->Observe(); }) &&
                rejects([&] { ArchiveEditSession::Resume(session->Directory())->Save(); }) &&
                bytes(session->WorkingFile()) == "later private draft" &&
                bytes(archivePath) == externalBytes,
            "automatic observation and resumed writeback refuse the unrelated archive");
    }
    for (const auto &format : WritableArchiveFormats()) {
      const auto a = archive(format.id);
      const auto m = format.id == "ar" ? ChildLocation(a, "edit.txt") : member(a);
      FileEntry initial;
      check(StatFile(m, initial), format.id + " original member exists");
      const bool dosMode = format.id == "cab" || format.id == "arj";
      check(dosMode || (initial.permissions & 0777) == 0640,
            format.id + " source permissions represented");
      auto before = fingerprint(a);
      auto session = ArchiveEditSession::Open(m, sessions);
      struct stat dir, file;
      stat(session->Directory().c_str(), &dir);
      stat(session->WorkingFile().c_str(), &file);
      check(!(dir.st_mode & 0077) && !(file.st_mode & 0077) &&
                bytes(session->WorkingFile()) == "original",
            format.id + " creates private exact working copy");
      check(!session->Save() && fingerprint(a) == before,
            format.id + " unchanged working copy never rewrites archive");
      writeFile(session->WorkingFile(), "Edited snapshot " + format.id);
      check(session->Observe() == ArchiveEditSession::Waiting &&
                session->Observe() == ArchiveEditSession::Waiting &&
                session->Observe() == ArchiveEditSession::Changed,
            format.id + " auto-save waits for stable observations");
      bool saved = session->Save();
      FileEntry info;
      StatFile(m, info);
      check(saved && bytes(m) == "Edited snapshot " + format.id &&
                bytes(ChildLocation(a, "keep")) == "keep" &&
                info.permissions == initial.permissions,
            format.id + " verified save preserves unrelated member and "
                        "original permissions");
      check(PrepareArchiveEditTarget(physical(a)).archiveFormat == format.id,
            format.id + " preserves archive container and compression");
      auto path = session->Directory();
      session.reset();
      session = ArchiveEditSession::Resume(path);
      before = fingerprint(a);
      check(!session->Save() && fingerprint(a) == before,
            format.id + " recovery recognizes already saved baseline");
      auto replacement = session->WorkingFile() + ".replacement";
      writeFile(replacement, "atomic editor save");
      rename(replacement.c_str(), session->WorkingFile().c_str());
      check(session->Save() && bytes(m) == "atomic editor save",
            format.id + " supports atomic editor file replacement");
      check(ArchiveEditSession::Open(m, sessions)->Directory() == path,
            format.id + " reopening member resumes retained session");
    }
    for (const auto &format : WritableArchiveFormats())
      if (format.encryption) {
        SetArchivePasswordPrompt(
            [](const std::string &, bool, const TransferControl &) {
              return std::make_shared<ArchiveSecret>("Listi-edit-test");
            });
        auto a = archive(format.id, true), m = member(a);
        auto session = ArchiveEditSession::Open(m, sessions);
        writeFile(session->WorkingFile(), "encrypted edit");
        ForgetArchivePasswords();
        check(session->Save(),
              format.id +
                  " encrypted edit saves with memory-only password prompt");
        ForgetArchivePasswords();
        int prompts = 0;
        SetArchivePasswordPrompt(
            [&](const std::string &, bool, const TransferControl &) {
              ++prompts;
              return std::make_shared<ArchiveSecret>("Listi-edit-test");
            });
        check(bytes(m) == "encrypted edit" && prompts == 1,
              format.id + " updated archive remains encrypted");
        check(bytes(Join(session->Directory(), "session"))
                      .find("Listi-edit-test") == std::string::npos,
              format.id + " recovery record contains no password");
        ForgetArchivePasswords();
        SetArchivePasswordPrompt({});
      }
    auto a = archive("zip"), m = member(a);
    auto session = ArchiveEditSession::Open(m, sessions);
    writeFile(session->WorkingFile(), "local changes");
    auto before = bytes(physical(a));
    Hooks cancel;
    cancel.proceed = [] { return false; };
    bool cancelled = false;
    try {
      session->Save(cancel);
    } catch (const Cancelled &) {
      cancelled = true;
    }
    check(cancelled && bytes(physical(a)) == before &&
              bytes(session->WorkingFile()) == "local changes",
          "cancel retains original archive and edited working file");
    auto newFile = root + "/outside";
    writeFile(newFile, "external");
    Run(PrepareArchiveCopy(Job{Action::Copy, {newFile}, a}), {});
    before = bytes(physical(a));
    check(rejects([&] { session->Save(); }) && bytes(physical(a)) == before &&
              bytes(session->WorkingFile()) == "local changes",
          "external archive change blocks save without losing local changes");
    session = ArchiveEditSession::Resume(session->Directory());
    check(rejects([&] { session->Save(); }),
          "recovery never silently rebases a stale archive snapshot");
    auto oldWorking = session->WorkingFile();
    // Restart can wait on an archive read or password prompt. Until it
    // publishes its replacement, the existing draft must remain discoverable.
    const auto oldDirectory = session->Directory();
    bool checkedDuringRestart = false, missingDraft = false;
    Hooks duringRestart;
    duringRestart.proceed = [&] {
      checkedDuringRestart = true;
      bool found = false;
      for (const auto &record : ListArchiveEdits(sessions))
        if (record.member == m && record.directory == oldDirectory)
          found = true;
      missingDraft |= !found;
      return true;
    };
    session = session->StartFresh(duringRestart);
    check(checkedDuringRestart && !missingDraft,
          "restart keeps the original journal discoverable until publication");
    check(session->WorkingFile() != oldWorking &&
              bytes(oldWorking) == "local changes" &&
              bytes(session->WorkingFile()) == "original",
          "explicit fresh edit retains old working copy and loads current "
          "archive member");
    check(ArchiveEditSession::Open(m, sessions)->Directory() ==
              session->Directory(),
          "reopen selects fresh session, not retired conflicting edit");
    // Simulate interrupted retirement after the replacement journal reached
    // disk. Only the published replacement may resume for this member.
    check(rename(Join(oldDirectory, "previous-session").c_str(),
                 Join(oldDirectory, "session").c_str()) == 0,
          "restore unretired predecessor journal for recovery test");
    int visible = 0;
    for (const auto &record : ListArchiveEdits(sessions))
      if (record.member == m)
        ++visible;
    check(visible == 1 &&
              ArchiveEditSession::Open(m, sessions)->Directory() == session->Directory() &&
              bytes(oldWorking) == "local changes",
          "interrupted retirement selects the new session and keeps the old draft");
    const auto intermediate = session;
    session = session->StartFresh();
    visible = 0;
    for (const auto &record : ListArchiveEdits(sessions))
      if (record.member == m)
        ++visible;
    check(visible == 1 && ArchiveEditSession::Open(m, sessions)->Directory() == session->Directory(),
          "another restart does not revive an unretired older journal");
    check(rename(Join(intermediate->Directory(), "previous-session").c_str(),
                 Join(intermediate->Directory(), "session").c_str()) == 0,
          "restore unretired intermediate journal for stale-handle checks");
    writeFile(intermediate->WorkingFile(), "draft in retired editor");
    check(rejects([&] { intermediate->Save(); }) &&
              rejects([&] { intermediate->StartFresh(); }) &&
              bytes(m) == "original" &&
              bytes(intermediate->WorkingFile()) == "draft in retired editor",
          "a superseded live handle cannot publish or replace the current edit");
    // Two editors can have identically named documents without sharing a
    // working file. Changing one must not silently rebase the other's draft.
    mkdir((root + "/in/other").c_str(), 0700);
    writeFile(root + "/in/other/edit.txt", "original sibling");
    a = archive("zip");
    auto add = PrepareArchiveCopy(Job{Action::Copy, {root + "/in/other"}, a});
    check(Run(add, {}).completed == 1, "created same-basename sibling fixture");
    const auto firstMember = member(a);
    const auto otherMember = ChildLocation(ChildLocation(a, "other"), "edit.txt");
    auto first = ArchiveEditSession::Open(firstMember, sessions);
    auto other = ArchiveEditSession::Open(otherMember, sessions);
    writeFile(first->WorkingFile(), "first edit");
    writeFile(other->WorkingFile(), "sibling draft");
    check(first->Save() && rejects([&] { other->Save(); }) &&
              bytes(firstMember) == "first edit" && bytes(otherMember) == "original sibling" &&
              bytes(other->WorkingFile()) == "sibling draft",
          "same-basename edits keep separate working copies and reject stale writeback");
    // Stable unchanged documents can still be large. Cancel while comparing
    // their second block, before any archive write or change to the baseline.
    const std::string large(3 * 65536, 'a');
    writeFile(root + "/in/folder/edit.txt", large);
    a = archive("tar");
    auto largeSession = ArchiveEditSession::Open(member(a), sessions);
    auto unchanged = fingerprint(a);
    auto cancelledComparison = [&](bool manual) {
      int polls = 0;
      Hooks stop;
      stop.proceed = [&] { return ++polls < 3; };
      try {
        if (manual)
          largeSession->Save(stop);
        else
          largeSession->Observe(stop);
      } catch (const Cancelled &) {
        return polls == 3;
      }
      return false;
    };
    check(largeSession->Observe() == ArchiveEditSession::Waiting &&
              largeSession->Observe() == ArchiveEditSession::Waiting &&
              cancelledComparison(false) &&
              largeSession->Observe() == ArchiveEditSession::Unchanged &&
              cancelledComparison(true) && !largeSession->Save() &&
              fingerprint(a) == unchanged && bytes(largeSession->WorkingFile()) == large,
          "cancel interrupts automatic and manual baseline comparisons; retry is unchanged");
    writeFile(root + "/in/folder/edit.txt", "original");
    a = archive("tar");
    m = member(a);
    session = ArchiveEditSession::Open(m, sessions);
    writeFile(session->WorkingFile(), "first saved version");
    bool changed = false;
    Hooks newer;
    newer.progress = [&](const std::string &, uint64_t) {
      if (!changed) {
        changed = true;
        writeFile(session->WorkingFile(), "later editor version");
      }
    };
    check(session->Save(newer) && changed &&
              bytes(m) == "first saved version" &&
              bytes(session->WorkingFile()) == "later editor version",
          "editor changes during packing remain local without changing "
          "in-flight snapshot");
    check(session->Save() && bytes(m) == "later editor version",
          "later editor save is applied by the following transaction");
    // Replace a working file with a link: neither target nor archive is
    // touched.
    a = archive("zip");
    m = member(a);
    session = ArchiveEditSession::Open(m, sessions);
    unlink(session->WorkingFile().c_str());
    symlink(newFile.c_str(), session->WorkingFile().c_str());
    before = bytes(physical(a));
    check(rejects([&] { session->Save(); }) && bytes(physical(a)) == before &&
              bytes(newFile) == "external",
          "symlink working file rejected");
    unlink(session->WorkingFile().c_str());
    writeFile(session->WorkingFile(), "restored");
    auto old = session->Directory() + "-old";
    rename(session->Directory().c_str(), old.c_str());
    mkdir(session->Directory().c_str(), 0700);
    mkdir((session->Directory() + "/work").c_str(), 0700);
    writeFile(session->WorkingFile(), "replacement");
    check(rejects([&] { session->Save(); }) && bytes(physical(a)) == before,
          "replaced private working directory is rejected");
    a = archive("zip");
    m = member(a);
    session = ArchiveEditSession::Open(m, sessions);
    writeFile(session->WorkingFile(), "retained after process restart");
    auto directory = session->Directory();
    session.reset();
    session = ArchiveEditSession::Resume(directory);
    check(session->Save() && bytes(m) == "retained after process restart",
          "unsaved disk copy survives session recreation and saves on explicit "
          "resume");
    const auto broken = sessions + "/edit-broken";
    mkdir(broken.c_str(), 0700);
    writeFile(broken + "/session", "not a recovery record");
    const auto records = ListArchiveEdits(sessions);
    bool brokenReported = false;
    for (const auto &record : records)
      if (record.directory == broken && !record.error.empty())
        brokenReported = true;
    check(brokenReported && ArchiveEditSession::Open(m, sessions)->Directory() == directory &&
              bytes(broken + "/session") == "not a recovery record",
          "a damaged independent journal remains visible without blocking a healthy edit");
    // Two independently prepared operations on one archive must not both
    // publish.
    auto one = root + "/one", two = root + "/two";
    writeFile(one, "one");
    writeFile(two, "two");
    auto j1 = PrepareArchiveCopy(Job{Action::Copy, {one}, a}),
         j2 = PrepareArchiveCopy(Job{Action::Copy, {two}, a});
    Result r1, r2;
    std::thread t1([&] { r1 = Run(j1, {}); }), t2([&] { r2 = Run(j2, {}); });
    t1.join();
    t2.join();
    check(r1.completed + r2.completed == 1 &&
              r1.errors.size() + r2.errors.size() == 1,
          "competing editor/queue archive transactions serialize and reject "
          "stale second job");
  } catch (const std::exception &e) {
    check(false, std::string("unexpected exception: ") + e.what());
  }
  clean(root);
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
