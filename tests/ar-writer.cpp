// SPDX-License-Identifier: MIT
#include "ArWriter.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace Listi;
namespace {
void Require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct FD {
  int value;
  ~FD() {
    if (value >= 0)
      close(value);
  }
};

struct Scratch {
  std::string path;
  Scratch() {
    char pattern[] = "/tmp/listi-ar-test-XXXXXX";
    Require(mkdtemp(pattern), "Cannot create Unix ar test folder");
    path = pattern;
  }
  ~Scratch() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

void Write(const std::string &path, const std::string &bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), bytes.size());
  out.close();
  Require(bool(out), "Cannot write Unix ar fixture");
}

std::string Read(const std::string &address) {
  std::string bytes;
  ArRead(ParseLocation(address),
         [&](const char *data, size_t length) { bytes.append(data, length); },
         {});
  return bytes;
}

std::string Format(const std::string &path) {
  FD input{open(path.c_str(), O_RDONLY)};
  Require(input.value >= 0, "Cannot open ar format fixture");
  return ArEditFormat(input.value, path);
}

template <class F> void Reject(const F &call, const char *expected) {
  try {
    call();
  } catch (const std::runtime_error &error) {
    Require(std::string(error.what()).find(expected) != std::string::npos,
            error.what());
    return;
  }
  throw std::runtime_error(std::string("Expected refusal: ") + expected);
}

std::string Header(std::string name, size_t size, const char *uid = "0") {
  auto field = [](std::string value, size_t width) {
    value.resize(width, ' ');
    return value;
  };
  return field(name, 16) + field("0", 12) + field(uid, 6) + field("0", 6) +
         field("100644", 8) + field(std::to_string(size), 10) + "`\n";
}

void RoundTrip(const Scratch &scratch) {
  const auto first = scratch.path + "/-odd";
  const auto longName = "føø archive entry longer than 16";
  const auto second = scratch.path + "/" + longName;
  const auto third = scratch.path + "/empty";
  const auto spacedName = " white ";
  const auto fourth = scratch.path + "/" + spacedName;
  const struct timespec stamp[] = {{0, UTIME_OMIT}, {1600000200, 0}};
  const auto data = std::string(230000, 'Q') + std::string("\0\xFF\n", 3);
  Write(first, data);
  Write(second, "with Unicode and spaces");
  Write(third, "");
  Write(fourth, "with leading and trailing whitespace");
  Require(!chmod(first.c_str(), 0751) &&
              !utimensat(AT_FDCWD, first.c_str(), stamp, 0),
          "Cannot set Unix ar fixture metadata");
  for (const bool bsd : {false, true}) {
    const auto archive = scratch.path + (bsd ? "/bsd.ar" : "/gnu.ar");
    FD fd{open(archive.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600)};
    Require(fd.value >= 0, "Cannot open ar output");
    ArCreate(fd.value, {first, second, third, fourth}, bsd, {});
    Require(Format(archive) == (bsd ? "ar-bsd" : "ar"),
            "Ar writer did not retain its naming format");
    const auto root = ArOpen(archive, {});
    const auto members = ArList(ParseLocation(root), {});
    Require(members.size() == 4 && members.front().name == spacedName &&
                members[1].permissions == 0751 &&
                members[1].modified == 1600000200,
            "Ar listing contains unexpected names or metadata");
    Require(Read(ChildLocation(root, "-odd")) == data &&
                Read(ChildLocation(root, longName)) ==
                    "with Unicode and spaces" &&
                Read(ChildLocation(root, "empty")).empty() &&
                Read(ChildLocation(root, spacedName)) ==
                    "with leading and trailing whitespace",
            "Ar did not preserve member bytes or names");
  }

  const auto invalid = scratch.path + "/not-written.ar";
  FD fd{open(invalid.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600)};
  Require(fd.value >= 0, "Cannot open ar rejection fixture");
  Reject([&] { ArCreate(fd.value, {scratch.path}, false, {}); },
         "regular files");
  const auto link = scratch.path + "/link";
  Require(!symlink(first.c_str(), link.c_str()),
          "Cannot create ar link fixture");
  Reject([&] { ArCreate(fd.value, {link}, false, {}); }, "regular files");
  Reject([&] { ArCreate(fd.value, {first, first}, false, {}); }, "unique");
  Hooks stop;
  stop.proceed = [] { return false; };
  bool cancelled = false;
  try {
    ArCreate(fd.value, {first}, false, stop);
  } catch (const Cancelled &) {
    cancelled = true;
  }
  struct stat state;
  Require(cancelled && !fstat(fd.value, &state) && state.st_size == 0,
          "Invalid or cancelled Unix ar job wrote output");
}

void ExternalFixtures(const Scratch &scratch) {
  const auto archive = scratch.path + "/external.ar";
  const std::string magic = "!<arch>\n";
  Write(archive, magic);
  Require(Format(archive) == "ar" &&
              ArList(ParseLocation(ArOpen(archive, {})), {}).empty(),
          "Empty Unix ar does not round trip");
  Write(archive, magic + Header("/", 4) + std::string("\0\0\0\0", 4) +
                     Header("code.o/", 4) +
                     "\x7f"
                     "ELF");
  Reject([&] { Format(archive); }, "Indexed ar libraries");
  const auto root = ArOpen(archive, {});
  Require(ArList(ParseLocation(root), {}).size() == 1 &&
              Read(ChildLocation(root, "code.o")) == "\x7f"
                                                     "ELF",
          "Object library index leaked as a member or prevented extraction");

  std::string elf = "\x7f"
                    "ELF";
  elf.resize(18, '\0');
  elf[5] = 1;
  elf[16] = 1;
  Write(archive, magic + Header("code/", elf.size()) + elf);
  Reject([&] { Format(archive); }, "object files");
  elf[16] = 2;
  Write(archive, magic + Header("program/", elf.size()) + elf);
  Require(Format(archive) == "ar",
          "Ordinary ar with ELF executable is read-only");
  Write(archive, magic + Header("debian-binary/", 4) + "2.0\n");
  Reject([&] { Format(archive); }, "Debian packages");
  Write(archive, "!<thin>\n");
  Require(ArRecognized(archive), "Thin ar not recognized");
  Reject([&] { ArOpen(archive, {}); }, "Thin ar libraries");
  Reject([&] { Format(archive); }, "Thin ar libraries");
  Write(archive, magic + Header("abc/", 3) + "abc");
  Reject([&] { ArList(ParseLocation(ArOpen(archive, {})), {}); }, "truncated");
  Write(archive, magic + Header("dir/file/", 1) + "x\n");
  const auto nestedRoot = ArOpen(archive, {});
  const auto implicitFolder = ChildLocation(nestedRoot, "dir");
  const auto children = ArList(ParseLocation(implicitFolder), {});
  Require(children.size() == 1 && children.front().name == "file" &&
              !children.front().directory,
          "Cannot list implicit ar directory");
  const auto nestedFile = ChildLocation(implicitFolder, "file");
  Require(Read(nestedFile) == "x",
          "Cannot read ar member with directory path");
  Reject([&] { ArList(ParseLocation(nestedFile), {}); }, "not a folder");
  Reject([&] { ArList(ParseLocation(ChildLocation(nestedRoot, "di")), {}); },
         "folder does not exist");
  Reject([&] { Format(archive); }, "flat ar archives");
  Write(archive, magic + Header("../unsafe/", 0));
  Reject([&] { Format(archive); }, "unsafe parent");
  Write(archive, magic + Header("abc/", 0) + Header("abc/", 0));
  Reject([&] { Format(archive); }, "duplicate");
  Write(archive, magic + Header("abc/", 0, "42"));
  Reject([&] { Format(archive); }, "ownership");
  Write(archive, magic + Header("short-bsd", 0));
  Require(Format(archive) == "ar-bsd",
          "BSD short-name layout was not preserved");
}
} // namespace

int main() {
  try {
    Scratch scratch;
    RoundTrip(scratch);
    ExternalFixtures(scratch);
    std::cout << "PASS GNU/BSD Unix ar names, bytes, metadata, cancellation, "
                 "malformed archives and read-only packages/libraries\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL Unix ar: " << error.what() << '\n';
    return 1;
  }
}
