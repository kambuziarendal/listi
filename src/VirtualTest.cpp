// SPDX-License-Identifier: MIT
#include "FileOperation.h"
#include "VirtualFile.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
namespace Listi {
int VirtualTest(const std::string &fixtures, const std::string &hosts) {
  int checks = 0, failures = 0;
  auto check = [&](bool ok, const std::string &name) {
    ++checks;
    if (!ok)
      ++failures;
    std::cout << (ok ? "PASS " : "FAIL ") << name << std::endl;
  };
  auto contents = [](const std::string &p) {
    std::string text;
    ReadFile(p, [&](const char *s, size_t n) { text.append(s, n); });
    return text;
  };
  auto rejects = [&](auto fn) {
    try {
      fn();
      return false;
    } catch (const std::exception &) {
      return true;
    }
  };
  char pattern[] = "/tmp/Listi-virtual-test-XXXXXX";
  char *base = mkdtemp(pattern);
  if (!base)
    return 1;
  std::string root = base;
  std::string local = ChildLocation(root, "upload space.txt");
  {
    std::ofstream f(local);
    f << "upload contents\n";
  }
  Hooks hooks;
  hooks.proceed = [] { return true; };
  hooks.conflict = [](const std::string &) { return Decision::Replace; };
  check(ParseLocation("smb://host/share/a%20b").path == "/share/a b",
        "URL path decoding");
  check(ChildLocation("sftp://host/folder", "Å file.txt") ==
            "sftp://host/folder/%C3%85%20file.txt",
        "UTF-8 URL encoding");
  check(rejects([] { ParseLocation("ftp://user:password@server/"); }),
        "credentials rejected in URLs");
  check(rejects([] { ParseLocation("ftp://host/a%0D%0Ab"); }),
        "FTP command injection rejected");
  for (auto name :
       {"sample.zip", "sample.tar.gz", "sample.tar.xz", "sample.tar.bz2"})
    try {
      auto a = OpenArchive(ChildLocation(fixtures, name));
      check(!a.empty(), std::string(name) + " recognized without Expander");
      if (a.empty())
        continue;
      auto list = ListFiles(a);
      check(std::any_of(
                list.begin(), list.end(),
                [](auto &e) { return e.name == "folder" && e.directory; }),
            std::string(name) + " implied directories");
      auto entry = ChildLocation(ChildLocation(a, "folder"), "hello world.txt");
      check(contents(entry) == "archive content\n",
            std::string(name) + " selected entry read");
      auto result = Run({Action::Copy, {entry}, root}, hooks);
      check(result.errors.empty() &&
                contents(ChildLocation(root, "hello world.txt")) ==
                    "archive content\n",
            std::string(name) + " copy out");
      FileEntry original, extracted;
      check(StatFile(entry, original) &&
                StatFile(ChildLocation(root, "hello world.txt"), extracted) &&
                original.permissions == extracted.permissions &&
                (!original.modified || original.modified == extracted.modified),
            std::string(name) +
                " extraction preserves available mode and mtime");
      std::string folderTarget =
          ChildLocation(root, std::string(name) + "-tree");
      MakeFolder(folderTarget);
      auto directory = ChildLocation(a, "folder");
      auto copied = Run({Action::Copy, {directory}, folderTarget}, hooks);
      check(copied.completed == 1 && copied.errors.empty() &&
                contents(ChildLocation(ChildLocation(folderTarget, "folder"),
                                       "hello world.txt")) ==
                    "archive content\n",
            std::string(name) + " recursive extraction completes");
      int successEvents = 0;
      Hooks skipped = hooks;
      skipped.conflict = [](const std::string &) { return Decision::Skip; };
      skipped.fileDone = [&](const std::string &, const std::string &) {
        ++successEvents;
      };
      auto partial = Run({Action::Copy, {directory}, folderTarget}, skipped);
      check(
          partial.completed == 0 && partial.skipped == 1 && successEvents == 0,
          std::string(name) + " skipped child never marks its parent complete");
      check(ParentLocation(a) == fixtures,
            std::string(name) + " parent exits archive");
    } catch (const std::exception &e) {
      check(false, std::string(name) + ": " + e.what());
    }
  for (auto name : {"single.txt.gz", "single.txt.bz2", "single.txt.xz"}) {
    try {
      auto archive = OpenArchive(ChildLocation(fixtures, name));
      auto entries = ListFiles(archive);
      check(entries.size() == 1 && entries[0].name == "single.txt",
            std::string(name) + " single-file name");
      auto entry = ChildLocation(archive, "single.txt");
      check(contents(entry) == "single content\n",
            std::string(name) + " single-file read");
      auto copied = Run({Action::Copy, {entry}, root}, hooks);
      check(copied.errors.empty() && copied.completed == 1 &&
                contents(ChildLocation(root, "single.txt")) ==
                    "single content\n",
            std::string(name) + " single-file extraction");
    } catch (const std::exception &e) {
      check(false, std::string(name) + ": " + e.what());
    }
  }
  try {
    auto archive = OpenArchive(ChildLocation(fixtures, "metadata.tar"));
    auto directory = ChildLocation(archive, "explicit");
    auto copied = Run({Action::Copy, {directory}, root}, hooks);
    FileEntry folder, file;
    check(copied.errors.empty() && copied.completed == 1 &&
              StatFile(ChildLocation(root, "explicit"), folder) &&
              folder.permissions == 0750 && folder.modified == 1600000000,
          "explicit archive directory mode and time preserved");
    check(StatFile(ChildLocation(ChildLocation(root, "explicit"), "run.sh"),
                   file) &&
              file.permissions == 0751 && file.modified == 1600000020,
          "archive executable mode preserved without setuid");
    chmod(ChildLocation(root, "explicit").c_str(), 0700);
    copied = Run({Action::Copy, {directory}, root}, hooks);
    check(copied.errors.empty() &&
              StatFile(ChildLocation(root, "explicit"), folder) &&
              folder.permissions == 0700,
          "existing extraction directory permissions retained");
  } catch (const std::exception &e) {
    check(false, std::string("metadata archive: ") + e.what());
  }
  check(OpenArchive(local).empty(), "ordinary text is not an archive");
  check(rejects([&] {
          ListFiles(OpenArchive(ChildLocation(fixtures, "unsafe.zip")));
        }),
        "archive traversal rejected");
  auto link = ChildLocation(OpenArchive(ChildLocation(fixtures, "links.tar")),
                            "escape");
  check(rejects([&] { contents(link); }), "archive symlink not extracted");
  auto large = ChildLocation(OpenArchive(ChildLocation(fixtures, "sample.zip")),
                             "large.bin");
  bool cancel = false;
  hooks.proceed = [&] { return !cancel; };
  hooks.progress = [&](const std::string &, uint64_t n) {
    if (n)
      cancel = true;
  };
  auto stopped = Run({Action::Copy, {large}, root}, hooks);
  FileEntry stat;
  check(stopped.cancelled && !StatFile(ChildLocation(root, "large.bin"), stat),
        "cancelled extraction exposes no partial target");
  hooks.proceed = [] { return true; };
  hooks.progress = {};
  const std::string changing = ChildLocation(root, "changing.txt");
  {
    std::ofstream f(changing);
    f << "original";
  }
  bool changed = false;
  TransferControl mutation;
  mutation.progress = [&](uint64_t n) {
    if (n && !changed) {
      changed = true;
      std::ofstream f(changing, std::ios::app);
      f << "changed during read";
    }
  };
  check(rejects(
            [&] { ReadFile(changing, [](const char *, size_t) {}, mutation); }),
        "local source mutation detected while staging");
  const std::string pipe = ChildLocation(root, "not-a-file");
  mkfifo(pipe.c_str(), 0600);
  check(rejects([&] { contents(pipe); }),
        "special files rejected without waiting for a writer");
  const char *configuredHost = getenv("LISTI_TEST_NETWORK_HOST");
  const std::string host = configuredHost ? configuredHost : "10.0.2.2";
  std::vector<std::pair<std::string, std::string>> endpoints{
      {"smb", ":1445/files"}, {"ftp", ":2121/"}, {"sftp", ":2222/"}};
  if (getenv("LISTI_TEST_FTPS"))
    endpoints.emplace_back("ftps", ":2143/");
  for (const auto &[protocol, suffix] : endpoints) {
    const std::string url = protocol + "://" + host + suffix;
    Connection auth;
    auth.username = "listitest";
    auth.password = "fixture-only";
    auth.knownHosts = hosts;
    SetConnection(url, auth);
    auto folder =
        ChildLocation(url, "test-" + std::to_string(getpid()) + protocol);
    try {
      auto list = ListFiles(url);
      check(std::any_of(list.begin(), list.end(),
                        [](auto &e) { return e.name == "hello world.txt"; }),
            std::string(protocol) + " list filenames with spaces");
      check(contents(ChildLocation(url, "hello world.txt")) ==
                "Listi network fixture\n",
            std::string(protocol) + " download");
      check(contents(ChildLocation(url, "Årsrapport.txt")) == "UTF-8 test\n",
            std::string(protocol) + " UTF-8 filename");
      MakeFolder(folder);
      auto transfer = Run({Action::Copy, {local}, folder}, hooks);
      check(transfer.errors.empty(), std::string(protocol) + " staged upload");
      for (auto &error : transfer.errors)
        std::cerr << error << std::endl;
      auto remote = ChildLocation(folder, LocationLeaf(local));
      check(contents(remote) == "upload contents\n",
            std::string(protocol) + " uploaded content verified");
      {
        std::ofstream f(local);
        f << "replacement\n";
      }
      hooks.conflict = [](const std::string &) { return Decision::Skip; };
      auto skip = Run({Action::Copy, {local}, folder}, hooks);
      check(skip.skipped == 1 && contents(remote) == "upload contents\n",
            std::string(protocol) + " conflict skip");
      hooks.conflict = [](const std::string &) { return Decision::Replace; };
      auto replaced = Run({Action::Copy, {local}, folder}, hooks);
      check(replaced.errors.empty() && contents(remote) == "replacement\n",
            std::string(protocol) + " conflict replace");
      bool stopUpload = false;
      int completedFiles = 0;
      Hooks interrupted = hooks;
      interrupted.proceed = [&] { return !stopUpload; };
      interrupted.progress = [&](const std::string &, uint64_t n) {
        if (n)
          stopUpload = true;
      };
      interrupted.fileDone = [&](const std::string &, const std::string &) {
        ++completedFiles;
      };
      auto cancelled = Run({Action::Copy, {local}, folder}, interrupted);
      auto remaining = ListFiles(folder);
      check(cancelled.cancelled && completedFiles == 0 &&
                contents(remote) == "replacement\n" && remaining.size() == 1,
            std::string(protocol) +
                " cancelled upload preserves original and removes staging");
      auto renamed = ChildLocation(folder, "renamed space.txt");
      RenameFile(remote, renamed);
      check(contents(renamed) == "replacement\n",
            std::string(protocol) + " rename");
      auto download = Run({Action::Copy, {renamed}, root}, hooks);
      check(download.errors.empty() &&
                contents(ChildLocation(root, "renamed space.txt")) ==
                    "replacement\n",
            std::string(protocol) + " download through jobs");
      FileEntry remoteInfo, downloadedInfo;
      check(StatFile(renamed, remoteInfo) &&
                StatFile(ChildLocation(root, "renamed space.txt"),
                         downloadedInfo) &&
                remoteInfo.permissions == downloadedInfo.permissions &&
                (!remoteInfo.modified ||
                 remoteInfo.modified == downloadedInfo.modified),
            std::string(protocol) +
                " download preserves available mode and mtime");
      auto trash = Run({Action::Trash, {renamed}, root}, hooks);
      check(!trash.errors.empty() && StatFile(renamed, stat),
            std::string(protocol) + " no permanent delete fallback");
      RemoveFile(renamed, false);
      RemoveFile(folder, true);
      check(!StatFile(folder, stat),
            std::string(protocol) + " remove file and folder");
      auth.password = "incorrect";
      SetConnection(url, auth);
      check(rejects([&] { ListFiles(url); }),
            std::string(protocol) + " bad credentials rejected");
      auth.password = "fixture-only";
      SetConnection(url, auth);
    } catch (const std::exception &e) {
      check(false, std::string(protocol) + ": " + e.what());
    }
    {
      std::ofstream f(local);
      f << "upload contents\n";
    }
  }
  std::cout << checks << " virtual checks, " << failures << " failures"
            << std::endl;
  return failures ? 1 : 0;
}
} // namespace Listi
