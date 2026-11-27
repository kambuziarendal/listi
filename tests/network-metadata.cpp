// SPDX-License-Identifier: MIT
// Optional native test; start network-metadata-fixture.py first.
#include "FileOperation.h"
#include "FolderCompare.h"
#include "JobSnapshot.h"
#include "RecoveryInventory.h"
#include "VirtualFile.h"
#include <Application.h>
#include <TextView.h>
#include <Window.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/stat.h>

using namespace Listi;
namespace {
void Expect(bool condition, const std::string &label) {
  if (!condition)
    throw std::runtime_error(label);
  std::cout << "PASS " << label << std::endl;
}
// Opt-in so protocol request counts do not lengthen the standard fixture.
void CheckScaling(const std::string &server, const std::string &local, int n) {
  const bool ftp = ParseLocation(server).scheme == "ftp";
  const auto reference = local + "/reference-" + std::to_string(n);
  std::filesystem::create_directory(reference);
  for (int i = 0; i < n; ++i)
    std::ofstream(reference + "/" + std::to_string(i)) << i;
  const auto compare = CompareFolders({reference, ChildLocation(server, "scaling-compare-" +
                                      std::to_string(n)), true});
  Expect(compare.complete == !ftp && compare.verified == size_t(n) && !compare.different &&
         (ftp ? compare.error.find("Only entries shown") != std::string::npos : compare.error.empty()),
         server + " compared every visible file: " + std::to_string(n));
  const auto target = local + "/" + ParseLocation(server).scheme;
  std::filesystem::create_directory(target);
  const auto name = "scaling-download-" + std::to_string(n);
  auto result = Run({Action::Copy, {ChildLocation(server, name)}, target}, {});
  Expect(ftp ? result.partialCopies == 1 && !result.completed : result.completed == 1 && result.errors.empty(),
         server + " reports the download scope");
  for (int i = 0; i < n; ++i) {
    std::ifstream file(target + "/" + name + "/" + std::to_string(i));
    std::string contents;
    if (!(file >> contents) || contents != std::to_string(i))
      throw std::runtime_error("Incorrect benchmark download: " + name);
  }
}
FileEntry Stat(const std::string &base, const std::string &name) {
  FileEntry entry;
  if (!StatFile(ChildLocation(base, name), entry))
    throw std::runtime_error("Missing fixture: " + name);
  return entry;
}
std::string Read(const std::string &path) {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), {}};
}
std::string RemoteBytes(const std::string &path) {
  std::string bytes;
  ReadFile(path, [&](const char *data, size_t n) { bytes.append(data, n); });
  return bytes;
}
Result Copy(const std::string &source, const std::string &destination,
            const Hooks &hooks = {}) {
  Job job{Action::Copy, {source}, destination};
  FreezeLocalJob(job, destination);
  return Run(job, hooks);
}
void CheckVisibleOnlyWindow(const std::string &remote, const std::string &local) {
  auto window = OpenFolderCompare(remote, local);
  Expect(window != nullptr, "opened the FTP comparison window");
  BMessenger target(window);
  target.SendMessage('cmha');
  std::string text;
  bool rendered = false;
  const auto deadline = system_time() + 30000000;
  while (!rendered && system_time() < deadline) {
    if (window->LockWithTimeout(20000) == B_OK) {
      auto summary = dynamic_cast<BTextView *>(window->FindView("compare summary"));
      if (summary)
        text = summary->Text();
      window->Unlock();
      rendered = text.find("Incomplete:") != std::string::npos &&
                 text.find("Only entries shown") != std::string::npos &&
                 text.find("1 SHA-256 matches") != std::string::npos &&
                 text.find("No differences found") == std::string::npos;
    }
    snooze(10000);
  }
  target.SendMessage(B_QUIT_REQUESTED);
  Expect(rendered, "MLSD comparison window scopes its verified match: " + text);
}
// Use the revision available in the displayed listing, not a later (potentially
// richer) stat. Round-trip it as the rename question does.
Job SelectedRename(const std::string &source, const std::string &target) {
  Job job{Action::Rename, {source}, target};
  for (const auto &entry : ListFiles(ParentLocation(source)))
    if (entry.path == source)
      job.expectedRemoteSources.emplace(source, entry);
  Expect(job.expectedRemoteSources.size() == 1, "captured selection: " + source);
  return DecodeJob(EncodeJob(job, 'test'));
}
void CheckUncertainOperations(const std::string &ftp, const std::string &local) {
  const auto source = ChildLocation(ChildLocation(ftp, "guarded-rename"), "visible");
  const auto target = ChildLocation(ParentLocation(source), ".settings");
  auto rename = Run(SelectedRename(source, target), {});
  FileEntry marker;
  Expect(!rename.completed && !rename.errors.empty() &&
             !StatFile(ChildLocation(ftp, "rename-command-seen"), marker) &&
             RemoteBytes(source) == "visible\n" && RemoteBytes(target) == "hidden\n",
         "uncertain FTP target blocks rename before RNFR or RNTO; both files survive");

  for (const std::string mode : {"rename-sensitive", "rename-insensitive"}) {
    const auto folder = ChildLocation(ftp, mode);
    const auto alias = ChildLocation(folder, "report");
    const auto third = ChildLocation(folder, "third");
    const bool insensitive = mode == "rename-insensitive";
    Expect(StatFile(alias, marker) == insensitive &&
               (!insensitive || RemoteBytes(alias) == "original"),
           mode + ": named FTP lookup distinguishes alias from missing file");
    const auto result = Run(SelectedRename(third, alias), {});
    Expect(!result.completed && !result.errors.empty() && RemoteBytes(third) == "third" &&
               RemoteBytes(ChildLocation(folder, "Report")) == "original" &&
               !StatFile(ChildLocation(ftp, "rename-command-seen"), marker),
           mode + ": no destructive FTP rename, even when target is currently absent");
  }
  for (const std::string name : {"hidden-only", "outer", "mlsd-hidden", "mlsd-outer"}) {
    const auto remote = ChildLocation(ftp, name);
    const auto child = name == "outer" ? ChildLocation(remote, "mixed-nested") :
                       name == "mlsd-outer" ? ChildLocation(remote, "mlsd-mixed") : remote;
    Expect(RemoteBytes(ChildLocation(child, ".settings")) == "hidden\n",
           "partial copy fixture contains a directly readable hidden child");
    std::vector<std::string> notifications;
    Hooks hooks;
    hooks.fileDone = [&](const auto &src, const auto &) { notifications.push_back(src); };
    auto result = Copy(remote, local, hooks);
    Expect(!result.completed && !result.errors.empty() &&
               std::find(notifications.begin(), notifications.end(), remote) == notifications.end(),
           name + ": uncertain nested listing does not complete the selected folder");
    auto destination = local + "/" + name;
    if (name == "outer" || name == "mlsd-outer") {
      destination += "/" + LocationLeaf(child);
      Expect(result.partialCopies == 1 && Read(destination + "/visible") == "visible\n" &&
                 std::any_of(result.outcomes.begin(), result.outcomes.end(), [&](const auto &row) {
                   return row.destination == destination + "/visible" && row.status == "copied";
                 }), "mixed nested FTP folder retains the delivered file and its receipt");
    }
    Expect(!std::filesystem::exists(destination + "/.settings"), "hidden child was not delivered");
    Expect(std::filesystem::is_directory(local + "/" + name) && result.partialCopies == 1,
           name + ": visible folder is delivered without a completeness promise");
    const auto compared = CompareFolders({remote, local + "/" + name, true});
    Expect(!compared.complete && !compared.different &&
               compared.verified == size_t(child != remote) &&
               compared.error.find("Only entries shown") != std::string::npos &&
               std::any_of(result.errors.begin(), result.errors.end(), [](const auto &text) {
                 return text.find("Only entries shown") != std::string::npos;
               }), name + ": copy receipt and comparison limit their claims to visible entries");
    if (name == "mlsd-outer")
      CheckVisibleOnlyWindow(remote, local + "/" + name);
  }
}
void CheckPrivateDownload(const std::string &sftp, const std::string &local) {
  const auto source = ChildLocation(sftp, "Private");
  Expect(Stat(sftp, "Private").permissions == 0700 && Stat(source, "inner").permissions == 0700 &&
             Stat(ChildLocation(source, "inner"), "memo").permissions == 0644,
         "private SFTP fixture exposes nested 0700 folders and a readable memo");
  const auto mode = [](const std::string &path) -> int {
    struct stat st;
    return lstat(path.c_str(), &st) == 0 ? st.st_mode & 0777 : -1;
  };
  const auto reports = [](const Result &r, const std::string &path, const std::string &status) {
    return std::any_of(r.outcomes.begin(), r.outcomes.end(), [&](const EntryOutcome &entry) {
      return entry.destination == path && entry.status == status;
    });
  };
  for (const std::string stage : {"root", "nested", "file"}) {
    const auto base = local + "/cancel-" + stage;
    std::filesystem::create_directory(base);
    const auto root = base + "/Private", nested = root + "/inner", memo = nested + "/memo";
    bool exposed = false;
    Hooks stop;
    stop.proceed = [&] {
      const auto outer = mode(root), inner = mode(nested);
      exposed |= (outer >= 0 && outer != 0700) || (inner >= 0 && inner != 0700);
      return stage == "root" ? outer < 0 : stage == "nested" ? inner < 0 : mode(memo) < 0;
    };
    const auto interrupted = Copy(source, base, stop);
    Expect(interrupted.cancelled && !interrupted.completed && interrupted.partialCopies == 1 &&
               !exposed && mode(root) == 0700 && reports(interrupted, root, "folder created") &&
               (stage == "root" || (mode(nested) == 0700 && reports(interrupted, nested, "folder created"))),
           "download stays private during and after cancellation at " + stage);
    Hooks replace;
    replace.conflict = [](const auto &) { return Decision::Replace; };
    const auto retry = Copy(source, base, replace);
    Expect(retry.completed == 1 && retry.errors.empty() && reports(retry, root, "existing folder") &&
               mode(root) == 0700 && mode(nested) == 0700 && Read(memo) == "private payload",
           "retry identifies existing folder and preserves nested privacy: " + stage);
  }
  const auto base = local + "/existing";
  const auto root = base + "/Private";
  std::filesystem::create_directories(root);
  chmod(root.c_str(), 0755);
  std::ofstream(root + "/original") << "unrelated";
  const auto blocked = Copy(source, base);
  Expect(!blocked.completed && !blocked.errors.empty() && mode(root) == 0755 &&
             Read(root + "/original") == "unrelated" && mode(root + "/inner") < 0,
         "a pre-existing more accessible target is neither populated nor chmodded");
}
void CheckSftpAlias(const std::string &sftp) {
  const auto folder = ChildLocation(sftp, "aliases");
  const auto third = ChildLocation(folder, "third");
  const auto alias = ChildLocation(folder, "report");
  FileEntry marker;
  const auto result = Run(SelectedRename(third, alias), {});
  Expect(!result.completed && !result.errors.empty() &&
             !StatFile(ChildLocation(sftp, "sftp-rename-seen"), marker) &&
             !StatFile(ChildLocation(folder, "absent"), marker) &&
             RemoteBytes(alias) == "original" && RemoteBytes(third) == "third",
         "SFTP name alias blocks rename without conflating a truly missing name");
  const auto scope = ChildLocation(sftp, "rename-control");
  const auto original = ChildLocation(scope, "original");
  const auto renamed = ChildLocation(scope, "renamed");
  auto control = Run(SelectedRename(original, renamed), {});
  Expect(control.completed == 1 && control.errors.empty() && RemoteBytes(renamed) == "control" &&
             !StatFile(original, marker), "unchanged SFTP selection renames to its new name");
  control = Run(SelectedRename(renamed, original), {});
  Expect(control.completed == 1 && control.errors.empty() && RemoteBytes(original) == "control" &&
             !StatFile(renamed, marker), "fresh SFTP selection renames back without leaving a duplicate");
}
void CheckIdentity(const std::string &ftp, const std::string &local) {
  const auto folder = local + "/identity";
  std::filesystem::create_directory(folder);
  std::ofstream(folder + "/sample") << "aaaa";
  for (const std::string mode : {"identity-stable", "identity-compare", "identity-copy"}) {
    const auto remote = ChildLocation(ftp, mode);
    const auto path = ChildLocation(remote, "sample");
    const auto before = Stat(remote, "sample");
    Expect(!before.providerIdentity.empty(), mode + " exposes an MLSD unique identity");
    if (mode != "identity-copy") {
      auto compared = CompareFolders({folder, remote, true});
      const bool stable = mode == "identity-stable";
      Expect(!compared.complete && compared.verified == size_t(stable) &&
                 compared.unverified == size_t(!stable),
             mode + ": comparison accepts only stable identity across the read");
    }
    if (mode != "identity-compare") {
      std::ofstream(folder + "/sample") << "original destination";
      Hooks hooks;
      hooks.conflict = [](const auto &) { return Decision::Replace; };
      auto copied = Copy(path, folder, hooks);
      const bool stable = mode == "identity-stable";
      Expect(copied.completed == int(stable) && copied.errors.empty() == stable &&
                 Read(folder + "/sample") == (stable ? "aaaa" : "original destination"),
             mode + ": transfer publishes only with stable identity");
    }
    const auto after = Stat(remote, "sample");
    const bool stable = mode == "identity-stable";
    Expect(before.size == after.size && before.SameModification(after) &&
               !after.providerIdentity.empty() &&
               (before.providerIdentity == after.providerIdentity) == stable &&
               RemoteBytes(path) == (stable ? "aaaa" : "bbbb"),
           mode + ": independent read confirms bytes, size, time and identity");
  }
}
void CheckStagedRecovery(const std::string &ftp, const std::string &sftp,
                         const std::string &local) {
  const std::string payload = "stage payload";
  const std::string source = local + "/upload.txt";
  std::ofstream(source) << payload;
  const auto signals = ChildLocation(sftp, "signals");
  for (const std::string mode : {"upload-uncertain", "upload-cancel", "upload-mlsd", "cancel-mlsd", "upload-cleanup"}) {
    const bool interrupt = mode == "upload-cancel" || mode == "cancel-mlsd";
    const auto remote = ChildLocation(ftp, mode);
    bool sent = false, cancel = false;
    Hooks hooks;
    if (interrupt) {
      hooks.proceed = [&] { return !cancel; };
      hooks.progress = [&](const auto &, uint64_t n) {
        if (n)
          sent = true;
        else if (sent) {
          // Independent side channel. The fixture signals only upon RETR of
          // the .part file, after the client has received the STOR reply.
          FileEntry signal;
          if (StatFile(ChildLocation(signals, mode), signal)) {
            cancel = true;
            throw Cancelled();
          }
        }
      };
    }
    auto result = Copy(source, remote, hooks);
    Expect(!result.completed && result.cancelled == interrupt &&
               (interrupt || !result.errors.empty()), mode + ": not reported as published");
    const auto row = std::find_if(result.outcomes.begin(), result.outcomes.end(),
                                  [](const auto &r) { return r.upload.has_value(); });
    if (mode == "upload-cleanup") {
      const auto records = ReviewRecovery("", RecoveryReview::JournalOnly);
      const auto remaining = ListFiles(remote);
      Expect(row != result.outcomes.end() && row->upload->contentVerified &&
                 row->upload->bytesWritten == payload.size() && row->upload->recoveryPaths.empty() &&
                 remaining.size() == 1 && remaining.front().name == "upload.txt" &&
                 RemoteBytes(remaining.front().path) == "other client" &&
                 records.find(remote + "/.listi-") == std::string::npos,
             "verified FTP cleanup removes actual stage and retires its journal");
      continue;
    }
    Expect(row != result.outcomes.end() && row->upload->bytesWritten == payload.size() &&
               row->upload->sourceRetained && !row->upload->contentVerified &&
               row->upload->publication == Publication::NotAttempted &&
               row->upload->recoveryPaths.size() == 1,
           mode + ": acknowledged bytes and unpublished recovery path in receipt");
    const auto &stage = row->upload->recoveryPaths.front();
    const auto review = ReviewRecovery("", RecoveryReview::JournalOnly);
    const auto visible = ListFiles(remote);
    Expect(std::none_of(visible.begin(), visible.end(), [&](const auto &e) { return e.path == stage; }) &&
               ParentLocation(stage) == remote && RemoteBytes(stage) == payload &&
               review.find(stage) != std::string::npos && Read(source) == payload,
           mode + ": receipt and journal identify actual retained upload");
    FileEntry marker;
    Expect(!StatFile(ChildLocation(ftp, "rename-command-seen"), marker),
           mode + ": no publication command reached server");
  }
}
void CheckFtpCompare(const std::string &ftp, const std::string &local) {
  auto folder = local + "/ftp-compare";
  std::filesystem::create_directory(folder);
  std::ofstream(folder + "/name") << "fixture contents\n";
  for (const std::string mode : {"mlsd", "list-all", "list-only"}) {
    const auto remote = ChildLocation(ftp, mode);
    std::string hidden;
    ReadFile(ChildLocation(remote, ".settings"),
             [&](const char *p, size_t n) { hidden.append(p, n); });
    Expect(hidden == "hidden\n", remote + " has a readable dotfile");
    auto result = CompareFolders({folder, remote, true});
    Expect(result.verified == 1, remote + " verifies the visible file pair: " + result.error);
    if (mode != "list-only") {
      Expect(!result.complete && result.different == 1 &&
                 result.entries.front().relative == ".settings" &&
                 result.entries.front().state == CompareState::OnlyRight,
             mode + " reports the visible dotfile as a difference");
    } else {
      Expect(!result.complete && !result.different &&
                 result.error.find("Only entries shown") != std::string::npos,
             "legacy FTP listing cannot certify a whole tree");
      std::ofstream(folder + "/.settings") << hidden;
      result = CompareFolders({remote, folder, true});
      Expect(!result.complete && !result.different && result.verified == 1 &&
                 result.unverified == 1 &&
                 result.entries.front().state == CompareState::Unverified,
             "absence from legacy FTP LIST is not a proven difference");
    }
  }
}
}

int main() {
  BApplication app("application/x-vnd.Listi-network-metadata");
  const auto *knownHosts = getenv("LISTI_METADATA_KNOWN_HOSTS");
  if (!knownHosts) {
    std::cerr
        << "Set LISTI_METADATA_KNOWN_HOSTS to the fixture known_hosts file.\n";
    return 2;
  }
  const std::string ftp = "ftp://127.0.0.1:2127/";
  const std::string sftp = "sftp://127.0.0.1:2227/";
  Connection auth;
  auth.username = "listitest";
  auth.password = "fixture-only";
  auth.knownHosts = knownHosts;
  SetConnection(ftp, auth);
  SetConnection(sftp, auth);
  char pattern[] = "/tmp/Listi-network-metadata-XXXXXX";
  if (!mkdtemp(pattern))
    return 2;
  const std::string local = pattern;
  int status = 0;
  try {
    if (const auto size = std::getenv("LISTI_NETWORK_SCALING")) {
      const int n = std::stoi(size);
      Expect(n == 4 || n == 8 || n == 16 || n == 32, "known scaling fixture size");
      CheckScaling(ftp, local, n);
      if (n <= 8)
        CheckScaling(sftp, local, n);
      std::filesystem::remove_all(local);
      return 0;
    }
    CheckFtpCompare(ftp, local);
    CheckUncertainOperations(ftp, local);
    CheckSftpAlias(sftp);
    CheckPrivateDownload(sftp, local);
    const auto unknownFolder = ChildLocation(ftp, "missing-mode");
    const auto memo = ChildLocation(unknownFolder, "private-memo");
    const auto folderEntry = Stat(ftp, "missing-mode");
    const auto memoEntry = Stat(unknownFolder, "private-memo");
    Expect(!folderEntry.permissionsKnown && !memoEntry.permissionsKnown &&
               folderEntry.permissions == 0700 && memoEntry.permissions == 0600,
           "omitted MLSD permissions are unknown, not a grant");
    const auto modeOf = [](const std::string &path) -> int {
      struct stat st;
      return !lstat(path.c_str(), &st) ? st.st_mode & 0777 : -1;
    };
    const auto finalFolder = local + "/missing-mode";
    Hooks stopAtFolder;
    stopAtFolder.proceed = [&] { return modeOf(finalFolder) < 0; };
    const auto stopped = Copy(unknownFolder, local, stopAtFolder);
    Expect(stopped.cancelled && modeOf(finalFolder) == 0700,
           "partial FTP folder without access facts starts private");
    const auto retried = Copy(unknownFolder, local);
    Expect(retried.partialCopies && modeOf(finalFolder) == 0700 &&
               modeOf(finalFolder + "/private-memo") == 0600 && Read(finalFolder + "/private-memo") == "private",
           "retry delivers private FTP contents without claiming listing completeness");
    Expect(Copy(memo, local).completed && modeOf(local + "/private-memo") == 0600,
           "standalone FTP file without a reported mode stays private");
    Expect(Stat(ftp, "reported-mode").permissionsKnown &&
               Copy(ChildLocation(ftp, "reported-mode"), local).completed && modeOf(local + "/reported-mode") == 0640,
           "explicit FTP file mode survives publication");
    std::filesystem::remove(finalFolder + "/private-memo");
    chmod(finalFolder.c_str(), 0755);
    const auto tooOpen = Copy(unknownFolder, local);
    Expect(!tooOpen.completed && !tooOpen.errors.empty() && modeOf(finalFolder) == 0755 &&
               modeOf(finalFolder + "/private-memo") < 0, "unknown FTP mode refuses a more accessible merge");
    CheckIdentity(ftp, local);
    CheckStagedRecovery(ftp, sftp, local);
    // An explicit epoch is not the same as an omitted or malformed fact.
    const auto epoch = Stat(ftp, "epoch");
    Expect(epoch.modifiedKnown && epoch.modified == 0, "MLSD explicit epoch");
    for (const auto *name :
         {"unknown", "wrong-day", "wrong-hour", "short", "suffix", "bare-dot",
          "bad-fraction", "duplicates"})
      Expect(!Stat(ftp, name).modifiedKnown,
             std::string("unknown MLSD: ") + name);
    Expect(!Stat(ChildLocation(ftp, "list-only"), "name").modifiedKnown,
           "fallback FTP LIST does not guess a timestamp");
    Expect(!epoch.SameModification(Stat(ftp, "unknown")),
           "known epoch is a different revision from unknown");
    auto stable = Stat(ftp, "stable");
    constexpr int64_t modified = 1709209800; // 2024-02-29 12:30:00 UTC
    Expect(
        stable.modifiedKnown && stable.modified == modified &&
            stable.modifiedFraction == "123456789" &&
            stable.ModifiedNanoseconds() == 123456789,
        "mixed-case Modify uses Gregorian UTC and normalizes fractional zeros");

    int completed = 0;
    int notified = 0;
    Hooks hooks;
    hooks.conflict = [](const std::string &) { return Decision::Replace; };
    hooks.fileDone = [&](const std::string &, const std::string &) {
      ++notified;
    };
    auto copy = [&](const std::string &server, const char *name) {
      Job job{Action::Copy, {ChildLocation(server, name)}, local};
      FreezeLocalJob(job, local);
      auto result = Run(job, hooks);
      completed += result.completed;
      return result;
    };
    auto stableCopy = copy(ftp, "stable");
    struct stat info{};
    struct stat precision{};
    const timespec reference[] = {{0, UTIME_OMIT}, {modified, 123456789}};
    const auto probe = local + "/precision";
    std::ofstream(probe) << "precision probe";
    Expect(!utimensat(AT_FDCWD, probe.c_str(), reference, 0) &&
               !stat(probe.c_str(), &precision), "measure destination timestamp precision");
    Expect(stableCopy.errors.empty() && completed == 1 && notified == 1 &&
               Read(local + "/stable") == "fixture contents\n" &&
               !stat((local + "/stable").c_str(), &info) &&
               info.st_mtime == modified && info.st_mtim.tv_nsec == precision.st_mtim.tv_nsec,
           "ordinary FTP download preserves content and known UTC mtime");
    auto unknownCopy = copy(ftp, "unknown");
    Expect(unknownCopy.errors.empty() && completed == 2 && notified == 2 &&
               Read(local + "/unknown") == "fixture contents\n" &&
               !stat((local + "/unknown").c_str(), &info) &&
               info.st_mtime > modified && (info.st_mode & 0777) == 0600,
           "FTP download with absent metadata stays private and does not stamp the epoch");

    std::ofstream(local + "/changed") << "original destination";
    auto before = Stat(ftp, "changed");
    auto changedCopy = copy(ftp, "changed");
    auto after = Stat(ftp, "changed");
    Expect(
        before.modifiedKnown && after.modifiedKnown &&
            before.size == after.size && before.modified == after.modified &&
            before.ModifiedNanoseconds() == after.ModifiedNanoseconds() &&
            !before.SameModification(after) && !changedCopy.errors.empty() &&
            changedCopy.errors.front().find("Source changed") !=
                std::string::npos &&
            completed == 2 && notified == 2 &&
            Read(local + "/changed") == "original destination",
        "same-size FTP change during download, below nanosecond precision, preserves existing destination");

    const auto sftpKnown = Stat(sftp, "known");
    Expect(sftpKnown.modifiedKnown && sftpKnown.modified == modified,
           "SFTP curl STAT reports the server timestamp");
    Expect(!Stat(sftp, "unknown").modifiedKnown,
           "SFTP missing STAT timestamp stays unknown");
    std::ofstream(local + "/tightens") << "original";
    const auto tightens = ChildLocation(sftp, "tightens");
    const auto firstMode = Stat(sftp, "tightens").permissions;
    const auto denied = copy(sftp, "tightens");
    Expect(firstMode == 0644 && Stat(sftp, "tightens").permissions == 0600 &&
               !denied.completed && !denied.errors.empty() && Read(local + "/tightens") == "original",
           "SFTP permission change during read retains original local file");
    Hooks retryHooks;
    retryHooks.conflict = hooks.conflict;
    const auto privateRetry = Copy(tightens, local, retryHooks);
    Expect(privateRetry.completed == 1 && !stat((local + "/tightens").c_str(), &info) &&
               (info.st_mode & 0777) == 0600 && Read(local + "/tightens") == "private",
           "fresh SFTP download preserves tightened mode and contents");
    auto sftpCopy = copy(sftp, "known");
    Expect(sftpCopy.errors.empty() && completed == 3 && notified == 3 &&
               Read(local + "/known") == "fixture contents\n" &&
               !stat((local + "/known").c_str(), &info) &&
               info.st_mtime == modified,
           "SFTP download preserves reliable STAT mtime");
    std::filesystem::create_directory(local + "/compare");
    std::ofstream(local + "/compare/known") << "fixture contents\n";
    auto compare = CompareFolders(
        {local + "/compare", ChildLocation(sftp, "compare"), true});
    Expect(compare.complete && compare.different == 0 && compare.verified == 1,
           "SFTP folder comparison verifies content with before/after STAT");

    std::filesystem::create_directory(local + "/recursive");
    chmod((local + "/recursive").c_str(), 0700);
    const auto destination = local + "/recursive/changed";
    for (const auto &server : {ftp, sftp}) {
      std::ofstream(destination) << "original destination";
      const auto folder = ChildLocation(server, "recursive");
      const auto beforeChild = Stat(folder, "changed");
      const auto result = copy(server, "recursive");
      const auto afterChild = Stat(folder, "changed");
      Expect(beforeChild.modifiedKnown && afterChild.modifiedKnown &&
                 beforeChild.size == afterChild.size &&
                 !beforeChild.SameModification(afterChild) &&
                 !result.errors.empty() &&
                 std::any_of(result.errors.begin(), result.errors.end(), [](const auto &message) {
                   return message.find("Source changed") != std::string::npos;
                 }) &&
                 completed == 3 && notified == 3 &&
                 Read(destination) == "original destination",
             server + " recursive copy uses fresh post-read metadata");
    }
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << std::endl;
    status = 1;
  }
  std::filesystem::remove_all(local);
  return status;
}
