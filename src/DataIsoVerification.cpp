// SPDX-License-Identifier: MIT
#include "DataIsoVerification.h"
#include "ArchiveBackend.h"
#include "ArchiveJobChecks.h"
#include "DynamicLibrary.h"
#include "Sha256.h"
#include <TypeConstants.h>
#include <archive.h>
#include <archive_entry.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fs_attr.h>
#include <vector>

namespace Listi {
using namespace ArchiveJobChecks;
namespace {
// External ISO images can contain boot code and layout metadata that the
// data-disc writer cannot preserve. Only an unchanged ISO written and
// verified by Listi can be rebuilt by the archive editor.
constexpr const char *IsoDigestAttr = "Listi:verified-data-iso-v1";
std::string IsoDigest(int fd, const Hooks &hooks) {
  struct stat before, after;
  Require(!fstat(fd, &before) && S_ISREG(before.st_mode),
          "Cannot inspect data ISO.");
  Sha256 digest;
  std::vector<char> buffer(131072);
  off_t at = 0;
  for (;;) {
    if (hooks.proceed && !hooks.proceed())
      throw Cancelled();
    auto n = pread(fd, buffer.data(), buffer.size(), at);
    if (n < 0 && errno == EINTR)
      continue;
    Require(n >= 0, "Cannot read data ISO.");
    if (!n)
      break;
    digest.Update(buffer.data(), n);
    at += n;
  }
  Require(at == before.st_size && !fstat(fd, &after) &&
              Unchanged(before, after),
          "Data ISO changed while being verified.");
  return digest.Finish();
}
} // namespace

void SealDataIso(int fd, const Hooks &hooks) {
  const auto digest = IsoDigest(fd, hooks);
  Require(fs_write_attr(fd, IsoDigestAttr, B_STRING_TYPE, 0, digest.data(),
                        digest.size()) == ssize_t(digest.size()),
          "Cannot store data ISO verification on this filesystem.");
}

void CopyDataIsoSeal(int source, int destination) {
  // Copy the digest of the verified stage, not a new digest of whatever the
  // visible output happens to contain. Subsequent edits verify it again.
  char recorded[64];
  attr_info info;
  Require(fs_stat_attr(source, IsoDigestAttr, &info) == 0 &&
              info.type == B_STRING_TYPE && info.size == sizeof(recorded) &&
              fs_read_attr(source, IsoDigestAttr, B_STRING_TYPE, 0, recorded,
                           sizeof(recorded)) == sizeof(recorded) &&
              fs_write_attr(destination, IsoDigestAttr, B_STRING_TYPE, 0, recorded,
                            sizeof(recorded)) == sizeof(recorded),
          "Cannot preserve the verified data ISO digest.");
}

void VerifyDataIsoOutput(DynamicLibrary &lib, int output,
                         std::map<std::string, std::string> members,
                         const Hooks &hooks) {
  struct Reader {
    DynamicLibrary &lib;
    archive *a;
    ~Reader() {
      if (a)
        LIBCALL(lib, archive_read_free)(a);
    }
  } reader{lib, LIBCALL(lib, archive_read_new)()};
  const auto a = reader.a;
  Require(
      a && lseek(output, 0, SEEK_SET) == 0 &&
          LIBCALL(lib, archive_read_support_format_iso9660)(a) == ARCHIVE_OK &&
          LIBCALL(lib, archive_read_open_fd)(a, output, 131072) == ARCHIVE_OK,
      "Cannot reopen staged data ISO. Nothing was published.");
  std::vector<char> decoded(65536), original(65536);
  archive_entry *entry = nullptr;
  int status;
  while ((status = LIBCALL(lib, archive_read_next_header)(a, &entry)) ==
         ARCHIVE_OK) {
    if (hooks.proceed && !hooks.proceed())
      throw Cancelled();
    auto name = SafeArchivePath(LIBCALL(lib, archive_entry_pathname)(entry));
    if (name.empty())
      continue;
    const auto member = members.find(name);
    Require(member != members.end(),
            "Data ISO changed an entry name or wrote an extra entry.");
    const auto path = member->second;
    members.erase(member);
    struct stat st;
    Require(!lstat(path.c_str(), &st) &&
                (st.st_mode & 0777) ==
                    LIBCALL(lib, archive_entry_perm)(entry) &&
                (st.st_mode & S_IFMT) ==
                    LIBCALL(lib, archive_entry_filetype)(entry) &&
                !LIBCALL(lib, archive_entry_hardlink)(entry),
            "Data ISO could not preserve entry type or permissions: " + path);
    if (S_ISDIR(st.st_mode))
      continue;
    FD input(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK));
    struct stat opened;
    Require(input.n >= 0 && !fstat(input.n, &opened) && Unchanged(st, opened) &&
                st.st_size == LIBCALL(lib, archive_entry_size)(entry),
            "Data ISO could not preserve entry size: " + path);
    off_t total = 0;
    for (;;) {
      if (hooks.proceed && !hooks.proceed())
        throw Cancelled();
      const auto n =
          LIBCALL(lib, archive_read_data)(a, decoded.data(), decoded.size());
      Require(n >= 0, "Cannot decode staged data ISO: " + path);
      if (!n)
        break;
      size_t have = 0;
      while (have < size_t(n)) {
        const auto got = read(input.n, original.data() + have, n - have);
        if (got < 0 && errno == EINTR)
          continue;
        Require(got > 0, "Cannot verify data ISO source: " + path);
        have += got;
      }
      Require(!memcmp(decoded.data(), original.data(), n),
              "Data ISO could not preserve entry bytes: " + path);
      total += n;
    }
    Require(total == st.st_size, "Data ISO entry is incomplete: " + path);
  }
  Require(status == ARCHIVE_EOF && members.empty(),
          "Data ISO is damaged or missing entries. Nothing was published.");
}

void RequireVerifiedDataIso(int fd, const Hooks &hooks) {
  char recorded[64];
  attr_info info;
  Require(
      fs_stat_attr(fd, IsoDigestAttr, &info) == 0 &&
          info.type == B_STRING_TYPE && info.size == sizeof(recorded) &&
          fs_read_attr(fd, IsoDigestAttr, B_STRING_TYPE, 0, recorded,
                       sizeof(recorded)) == sizeof(recorded) &&
          IsoDigest(fd, hooks) == std::string(recorded, sizeof(recorded)),
      "Only unmodified Listi data ISOs can be edited. Other ISO images can be browsed and copied out without risking boot or disc metadata.");
}
} // namespace Listi
