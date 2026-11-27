// SPDX-License-Identifier: MIT
#include "ArchiveBackend.h"
#include "ArchiveJobs.h"
#include "ArchiveMetadata.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace Listi;
namespace fs = std::filesystem;

namespace {
struct Fixture {
  char path[40] = "/tmp/Listi-archive-metadata-XXXXXX";

  Fixture() {
    if (!mkdtemp(path))
      throw std::runtime_error("Cannot create test directory");
  }

  ~Fixture() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};

std::string Pack(const fs::path &input, const std::string &destination,
                 const std::string &format) {
  auto job = PrepareArchiveJob(false, {input.string()});
  job.archiveFormat = format;
  SetArchiveDestination(job, input.parent_path().string(), destination);
  const auto result = Run(job, {});
  if (result.completed != 1 || !result.errors.empty())
    throw std::runtime_error("Cannot create " + format + " fixture");
  return job.destination;
}

void Check(bool pass, const std::string &label) {
  if (!pass)
    throw std::runtime_error(label);
  std::cout << "PASS " << label << std::endl;
}

using ListWithBudget = std::vector<FileEntry> (*)(const Location &,
                                                 const TransferControl &,
                                                 ArchiveMetadataBudget &);

void TestAdapter(const fs::path &root, const std::string &format,
                 ListWithBudget list) {
  const auto shortArchive = Pack(root / "short", "short-names." + format, format);
  const auto nestedArchive = Pack(root / "nested", "nested-names." + format, format);
  auto location = ParseLocation(OpenArchive(nestedArchive));
  ArchiveMetadataBudget smallBudget(24 * 1024);
  bool refused = false;
  try {
    list(location, {}, smallBudget);
  } catch (const std::runtime_error &error) {
    refused =
        std::string_view(error.what()) ==
        "Archive metadata exceeds the safe listing limit";
  }
  Check(refused, format + " refuses nested metadata under a small budget");
  const auto normal = ListFiles(location.URL());
  Check(normal.size() == 1 && normal.front().name == "nested" &&
            normal.front().directory,
        format + " default listing is unaffected by the rejected scan");

  const auto nested = normal.front().path;
  Check(ListFiles(ChildLocation(nested, "empty")).empty(),
        format + " lists an existing empty folder");

  ArchiveMetadataBudget shortBudget(96 * 1024);
  auto shortLocation =
      ParseLocation(ChildLocation(OpenArchive(shortArchive), "short"));
  const auto entries = list(shortLocation, {}, shortBudget);
  Check(entries.size() == 128 && entries.front().name == "item-1000" &&
            entries.back().name == "item-1127",
        format + " lists 128 short names with a modest budget");
  auto rejectFolder = [&](const std::string &path, std::string_view expected) {
    bool rejected = false;
    try {
      ListFiles(path);
    } catch (const std::runtime_error &error) {
      rejected = error.what() == expected;
    }
    Check(rejected, format + " " + std::string(expected));
  };
  rejectFolder(entries.front().path, "Archive member is not a folder.");
  rejectFolder(ChildLocation(shortLocation.URL(), "item"),
               "Archive folder does not exist.");
  std::string contents;
  ReadFile(entries.front().path, [&](const char *bytes, size_t count) {
    contents.append(bytes, count);
  });
  Check(contents == "short", format + " independent archive read stays intact");
}
} // namespace

int main() {
  try {
    const Fixture fixture;
    const fs::path root(fixture.path);
    fs::create_directory(root / "short");
    for (int i = 1000; i < 1128; ++i)
      std::ofstream(root / "short" / ("item-" + std::to_string(i))) << "short";

    // Small ordinary archives: a single visible root with nested metadata,
    // so rejection cannot merely be caused by materializing output rows.
    for (int i = 0; i < 32; ++i) {
      auto directory = root / "nested" / ("branch-" + std::to_string(i));
      for (char component = 'a'; component <= 'e'; ++component)
        directory /= std::string(86, component);
      fs::create_directories(directory);
      std::ofstream(directory / "member") << "nested";
    }
    fs::create_directory(root / "nested" / "empty");

    TestAdapter(root, "zip", LibarchiveList);
    TestAdapter(root, "7z", SevenZipList);
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << std::endl;
    return 1;
  }
}
