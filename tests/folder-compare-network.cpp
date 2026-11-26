// SPDX-License-Identifier: MIT
// Optional integration lane. Requires task-owned SMB/FTP/SFTP fixtures.
#include "FolderCompare.h"
#include <Application.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace Listi;

static bool ExpectedCoverage(const CompareResult &result, bool ftp) {
  if (result.unverified != 0 || result.cancelled)
    return false;
  if (ftp)
    return !result.complete && result.error.find("Only entries shown by the FTP server") != std::string::npos;
  return result.complete && result.error.empty();
}

int main(int argc, char **argv) {
  BApplication app("application/x-vnd.Listi-compare-network");
  if (argc != 1 && argc != 5) {
    std::cerr << "Usage: folder-compare-network [known_hosts smb_url ftp_url sftp_url]\n";
    return 2;
  }
  char pattern[] = "/tmp/Listi-compare-network-XXXXXX";
  if (!mkdtemp(pattern))
    return 2;
  const std::string local = pattern;
  std::filesystem::create_directory(local + "/folder");
  std::ofstream(local + "/Årsrapport.txt") << "UTF-8 test\n";
  std::ofstream(local + "/folder/nested.txt") << "nested\n";
  Connection connection;
  connection.username = "listitest";
  connection.password = "fixture-only";
  connection.knownHosts = argc == 5 ? argv[1] : "/boot/home/listi-compare-known-hosts";
  const std::vector<std::string> urls = argc == 5
      ? std::vector<std::string>{argv[2], argv[3], argv[4]}
      : std::vector<std::string>{"smb://127.0.0.1:1545/files", "ftp://127.0.0.1:2221/", "sftp://127.0.0.1:2322/"};
  int failures = 0;
  for (const auto &url : urls) {
    const bool ftp = ParseLocation(url).scheme == "ftp";
    SetConnection(url, connection);
    for (const bool changed : {false, true}) {
      std::ofstream(local + "/hello world.txt") << (changed ? "Xisti" : "Listi") << " network fixture\n";
      const auto result = CompareFolders({local, url, true});
      const bool ok = ExpectedCoverage(result, ftp) &&
                      result.different == (changed ? 1u : 0u) &&
                      result.verified == (changed ? 2u : 3u);
      std::cout << (ok ? "PASS " : "FAIL ") << url
                << (changed ? " same-size content mismatch" : " matching file hashes")
                << ": " << result.error << std::endl;
      failures += !ok;
      if (!ok)
        for (const auto &entry : result.entries)
          std::cout << entry.relative << " " << CompareLabel(entry.state) << " " << entry.detail << std::endl;
    }
  }
  std::filesystem::remove_all(local);
  return failures ? 1 : 0;
}
