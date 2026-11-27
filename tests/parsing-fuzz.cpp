// SPDX-License-Identifier: MIT
// Pure parsing, no network operations, files or teardown state in a case.
#include "Sha256.h"
#include "VirtualFile.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

namespace {
void Require(bool ok) {
  if (!ok)
    std::abort();
}
template <class Operation> void RequireRejected(Operation operation) {
  try {
    operation();
  } catch (const std::runtime_error &) {
    return;
  }
  std::abort();
}
void VerifyLocationRegressions() {
  using Listi::ChildLocation;
  using Listi::ParseLocation;
  using Listi::ResolveVirtualRelativePath;

  const std::string archive = "archive:///fixture%0D%0A%21%25.zip!/";
  const auto location = ParseLocation(archive);
  Require(location.archive == "/fixture\r\n!%.zip");
  Require(location.URL() == archive);
  RequireRejected([] { ParseLocation("archive:///fixture%00.zip!/"); });
  for (const auto &invalid : std::vector<std::string>{
           "\r", "\n", "%0D", "%0A", "%00", std::string(1, '\0')}) {
    RequireRejected([&] { ParseLocation("sftp://fixture/" + invalid); });
    RequireRejected([&] { ParseLocation(archive + invalid); });
  }
  RequireRejected([] { ParseLocation("sftp://:22/folder"); });
  Require(ParseLocation("sftp://[::1]:22/folder").host == "[::1]");

  Require(ResolveVirtualRelativePath(archive + "old", "../a/b/./c") ==
          archive + "a/b/c");
  Require(ResolveVirtualRelativePath(archive, "../..") == archive);
  Require(ResolveVirtualRelativePath(archive, "%2e%2e/%2f") ==
          archive + "%252e%252e/%252f");
  const std::string remote = "sftp://fixture:22/";
  Require(ResolveVirtualRelativePath(remote + "old", "../a/b/../c") ==
          remote + "a/c");
  Require(ResolveVirtualRelativePath(remote + "old", ".") == remote + "old");
  Require(ResolveVirtualRelativePath(remote + "old", "../../new") ==
          remote + "new");
  RequireRejected([&] { ResolveVirtualRelativePath(remote, "a\r\n/.."); });
  for (const std::string name : {"a/b", ".", ".."})
    RequireRejected([&] { ChildLocation(archive, name); });
}
void Exercise(const std::string &input) {
  auto path = input;
  for (auto &byte : path)
    if (byte == '\0' || byte == '\r' || byte == '\n')
      byte = '_';
  Require(Listi::DecodePath(Listi::EncodePath(path)) == path);

  for (const std::string prefix : {"", "sftp://Fixture/", "archive:///fixture!/"}) {
    Listi::Location location;
    try {
      location = Listi::ParseLocation(prefix + input);
    } catch (const std::exception &) {
      continue; // Invalid inputs may be rejected without a partial result.
    }
    if (!location.Local()) {
      const auto roundTrip = Listi::ParseLocation(location.URL());
      Require(roundTrip.URL() == location.URL());
    }
  }

  Listi::Sha256 whole, split;
  whole.Update(input.data(), input.size());
  const auto boundary = input.size() / 3;
  split.Update(input.data(), boundary);
  split.Update(input.data() + boundary, input.size() - boundary);
  Require(whole.Finish() == split.Finish());
}
} // namespace

int main() {
  VerifyLocationRegressions();
  for (const std::string seed : {"", "/", ".", "..", "%00", "a%2fb",
         "ftp://[::1]:65535/../.", "smb://example.invalid/share",
         "archive:///sample.zip!/%E2%82%AC", "sftp://example.invalid:0/"})
    Exercise(seed);
  std::mt19937 random(0x4c697374); // Reproducible failures.
  constexpr unsigned cases = 25000;
  for (unsigned i = 0; i < cases; ++i) {
    std::string input(random() % 512, '\0');
    for (auto &byte : input)
      byte = static_cast<char>(random() & 0xff);
    Exercise(input);
  }
  std::cout << cases << " seeded fuzz inputs passed\n";
}
