// SPDX-License-Identifier: MIT
// Optional native test; start network-metadata-fixture.py first.
#include "FileOperation.h"
#include "FolderCompare.h"
#include "JobSnapshot.h"
#include "VirtualFile.h"
#include <Application.h>
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
      Expect(result.complete == (mode == "mlsd") && result.different == 1 &&
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
                 result.errors.front().find("Source changed") !=
                     std::string::npos &&
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
