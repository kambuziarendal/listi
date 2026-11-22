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
void Exercise(const std::string &input) {
  auto path = input;
  std::replace(path.begin(), path.end(), '\0', '0');
  Require(Listi::DecodePath(Listi::EncodePath(path)) == path);

  for (const std::string prefix : {"", "sftp://Fixture/", "archive:///fixture!"}) {
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
