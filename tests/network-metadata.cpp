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
void CheckUncertainOperations(const std::string &ftp, const std::string &local) {
  const auto source = ChildLocation(ChildLocation(ftp, "guarded-rename"), "visible");
  const auto target = ChildLocation(ParentLocation(source), ".settings");
  auto rename = Run(Job{Action::Rename, {source}, target}, {});
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
    const auto result = Run(Job{Action::Rename, {third}, alias}, {});
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
void CheckSftpAlias(const std::string &sftp) {
  const auto folder = ChildLocation(sftp, "aliases");
  const auto third = ChildLocation(folder, "third");
  const auto alias = ChildLocation(folder, "report");
  FileEntry marker;
  const auto result = Run(Job{Action::Rename, {third}, alias}, {});
  Expect(!result.completed && !result.errors.empty() &&
             !StatFile(ChildLocation(sftp, "sftp-rename-seen"), marker) &&
             !StatFile(ChildLocation(folder, "absent"), marker) &&
             RemoteBytes(alias) == "original" && RemoteBytes(third) == "third",
         "SFTP name alias blocks rename without conflating a truly missing name");
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
    Expect(result.verified == 1, remote + " verifies the visible file pair");
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
    CheckFtpCompare(ftp, local);
    CheckUncertainOperations(ftp, local);
    CheckSftpAlias(sftp);
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
               info.st_mtime > modified,
           "FTP download with absent time does not stamp the epoch");

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
