// SPDX-License-Identifier: MIT
#include "ArchiveWriting.h"
#include "StagingSpace.h"

#include "ArchiveBackend.h"
#include "ArchiveJobChecks.h"
#include "ArchiveJobs.h"
#include "ArchiveMetadata.h"
#include "ArchiveTextLocale.h"
#include "ArchiveVolumes.h"
#include "ArWriter.h"
#include "ArjWriter.h"
#include "CabWriter.h"
#include "DataIsoVerification.h"
#include "DynamicLibrary.h"
#include "LhaWriter.h"
#include "RarStore.h"
#include "VirtualFile.h"
#include <archive.h>
#include <archive_entry.h>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <sys/stat.h>
#include <utility>

namespace Listi {
namespace {
using namespace ArchiveJobChecks;
struct Writer {
  DynamicLibrary lib{"libarchive.so", "libarchive.so.13"};
  archive *a = nullptr;
  int fd = -1;
  Writer() {
    Require(
        lib.Available(),
        "Install the optional libarchive package from HaikuDepot, then try again.");
    a = LIBCALL(lib, archive_write_new)();
    Require(a, "Cannot allocate archive writer.");
  }
  ~Writer() {
    if (a)
      LIBCALL(lib, archive_write_free)(a);
  }
  void Check(int result) {
    if (result == ARCHIVE_OK)
      return;
    const char *error = LIBCALL(lib, archive_error_string)(a);
    throw std::runtime_error(error ? error : "Archive writing failed.");
  }
  void Configure(const std::string &format, int level,
                 const ArchiveSecretPtr &secret = {}, bool preserveTimestamps = false) {
    Require(level >= 0 && level <= 9, "Invalid compression level.");
    if (format == "zip") {
      Check(LIBCALL(lib, archive_write_set_format_zip)(a));
      Check(LIBCALL(lib, archive_write_set_options)(a, "zip:hdrcharset=UTF-8"));
      if (secret) {
        Check(LIBCALL(lib, archive_write_set_passphrase)(a, secret->Text()));
        Check(LIBCALL(lib, archive_write_set_options)(a,
                                                      "zip:encryption=aes256"));
      }
    } else if (format == "7z")
      Check(LIBCALL(lib, archive_write_set_format_7zip)(a));
    else {
      if (format == "cpio")
        Check(LIBCALL(lib, archive_write_set_format_cpio_newc)(a));
      else if (format == "iso") {
        Check(LIBCALL(lib, archive_write_set_format_iso9660)(a));
        Check(LIBCALL(lib, archive_write_set_options)(
            a, "iso9660:iso-level=3,iso9660:rockridge=strict,"
               "iso9660:volume-id=LISTI_DATA"));
      } else if (format == "xar")
        Check(LIBCALL(lib, archive_write_set_format_xar)(a));
      else {
        Require(IsTarFormat(format), "Unknown archive format.");
        if (preserveTimestamps)
          Check(LIBCALL(lib, archive_write_set_format_pax)(a));
        else
          Check(LIBCALL(lib, archive_write_set_format_pax_restricted)(a));
        const char *filter = format == "tgz"     ? "gzip"
                             : format == "txz"   ? "xz"
                             : format == "tbz2"  ? "bzip2"
                             : format == "tlz"   ? "lzma"
                             : format == "tzst"  ? "zstd"
                             : format == "tlz4"  ? "lz4"
                             : format == "taz"   ? "compress"
                             : format == "tlzip" ? "lzip"
                                                 : "none";
        if (format != "tar")
          Check(LIBCALL(lib, archive_write_add_filter_by_name)(a, filter));
      }
    }
    if (format != "tar" && format != "cpio" && format != "taz" && format != "iso") {
      const std::string option = "compression-level=" + std::to_string(level);
      Check(LIBCALL(lib, archive_write_set_options)(a, option.c_str()));
    }
  }
};
const ArchiveFormat formats[] = {
    {"zip", "ZIP", ".zip", true},
    {"7z", "7z", ".7z", true},
    {"tgz", "Tar + gzip", ".tar.gz", true},
    {"txz", "Tar + xz", ".tar.xz", true},
    {"tbz2", "Tar + bzip2", ".tar.bz2", true},
    {"tlz", "Tar + LZMA", ".tar.lzma", true},
    {"tzst", "Tar + Zstandard", ".tar.zst", true},
    {"tlz4", "Tar + LZ4", ".tar.lz4", true},
    {"tlzip", "Tar + lzip", ".tar.lz", true},
    {"taz", "Tar + Unix compress", ".tar.Z", false},
    {"tar", "Tar (uncompressed)", ".tar", false},
    {"cpio", "CPIO (newc)", ".cpio", false},
    {"xar", "XAR", ".xar", true},
    {"ar", "Unix ar", ".ar", false},
    {"rar5", "RAR5 (Store)", ".rar", false},
    {"arj", "ARJ (Store)", ".arj", false},
    {"cab", "Microsoft Cabinet", ".cab", false},
    {"lha", "LHA", ".lha", true},
    {"iso", "ISO 9660 (data disc)", ".iso", false}};
la_ssize_t BudgetArchiveWrite(archive *archive, void *opaque,
                              const void *data, size_t size) {
  auto &writer = *static_cast<Writer *>(opaque);
  size_t at = 0;
  while (at < size) {
    const auto count = WriteStaging(writer.fd, static_cast<const char *>(data) + at,
                                     size - at);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      const auto code = count ? errno : EIO;
      LIBCALL(writer.lib, archive_set_error)(archive, code, "Archive staging: %s",
                                      strerror(code));
      return -1;
    }
    at += count;
  }
  return size;
}
// Read the original logical headers, not the ownership of extracted scratch files.
void ReadTarMetadata(DynamicLibrary &lib, archive *reader, archive_entry *entry,
                   int status, const Hooks &hooks, TarMemberMap &metadata) {
  ArchiveMetadataBudget budget;
  for (; status != ARCHIVE_EOF;
       status = LIBCALL(lib, archive_read_next_header)(reader, &entry)) {
    Require(status == ARCHIVE_OK, "Cannot read TAR ownership. Original archive retained.");
    if (hooks.proceed && !hooks.proceed())
      throw Cancelled();
    Require(metadata.size() < 200000, "Archive has too many entries.");
    auto name = SafeArchivePath(LIBCALL(lib, archive_entry_pathname)(entry));
    Require(!name.empty(), "Cannot preserve an explicit TAR root entry. Original archive retained.");
    const char *user = LIBCALL(lib, archive_entry_uname)(entry);
    const char *group = LIBCALL(lib, archive_entry_gname)(entry);
    budget.Charge(sizeof(TarMemberMap::value_type) + ArchiveMetadataBudget::NodeOverhead +
                  name.size() + (user ? strlen(user) : 0) + (group ? strlen(group) : 0));
    unsigned long flags = 0, cleared = 0;
    LIBCALL(lib, archive_entry_fflags)(entry, &flags, &cleared);
    Require(!flags && !cleared && !LIBCALL(lib, archive_entry_xattr_count)(entry) &&
                !LIBCALL(lib, archive_entry_acl_types)(entry) &&
                !LIBCALL(lib, archive_entry_sparse_count)(entry),
            "Cannot preserve extended TAR metadata for " + name + ". Original archive retained.");
    TarMemberMetadata member{LIBCALL(lib, archive_entry_uid)(entry),
                             LIBCALL(lib, archive_entry_gid)(entry),
                             user ? user : "", group ? group : "",
                             LIBCALL(lib, archive_entry_filetype)(entry) == AE_IFDIR,
                             unsigned(LIBCALL(lib, archive_entry_perm)(entry)) & 07777,
                             LIBCALL(lib, archive_entry_mtime)(entry),
                             LIBCALL(lib, archive_entry_mtime_nsec)(entry)};
    if (LIBCALL(lib, archive_entry_atime_is_set)(entry))
      member.accessed.emplace(LIBCALL(lib, archive_entry_atime)(entry),
                              LIBCALL(lib, archive_entry_atime_nsec)(entry));
    if (LIBCALL(lib, archive_entry_ctime_is_set)(entry))
      member.changed.emplace(LIBCALL(lib, archive_entry_ctime)(entry),
                             LIBCALL(lib, archive_entry_ctime_nsec)(entry));
    if (LIBCALL(lib, archive_entry_birthtime_is_set)(entry))
      member.created.emplace(LIBCALL(lib, archive_entry_birthtime)(entry),
                             LIBCALL(lib, archive_entry_birthtime_nsec)(entry));
    Require(metadata.emplace(std::move(name), std::move(member)).second,
            "Ambiguous TAR member ownership. Original archive retained.");
  }
}
struct Pack {
  Writer &writer;
  const Hooks &hooks;
  bool zip, tar;
  const TarMemberMap &metadata;
  size_t count = 0;
  std::vector<std::pair<std::string, struct stat>> snapshots;
  std::map<std::string, std::string> *isoMembers = nullptr;
  void Verify() {
    for (const auto &item : snapshots) {
      Check();
      struct stat now;
      Require(!lstat(item.first.c_str(), &now) && Unchanged(item.second, now),
              "Source changed before archive completion: " + item.first);
    }
  }
  std::vector<char> buffer = std::vector<char>(131072);
  void Check() {
    if (hooks.proceed && !hooks.proceed())
      throw Cancelled();
  }
  void Add(const std::string &path, const std::string &name,
           unsigned depth = 0) {
    Check();
    Require(depth <= 64 && ++count <= 200000,
            "Archive exceeds the folder or entry limit.");
    Require(!name.empty() &&
                name.find_first_of("\r\n\\") == std::string::npos &&
                !(depth == 0 && name.size() > 1 && name[1] == ':'),
            "Unsupported archive entry name.");
    struct stat before, after;
    Require(!lstat(path.c_str(), &before), "Cannot read source: " + path);
    Require(S_ISREG(before.st_mode) || S_ISDIR(before.st_mode),
            "Links and special files are not packed in this version: " + path);
    snapshots.push_back({path, before});
    if (isoMembers)
      isoMembers->emplace(name, path);
    FD fd(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK));
    Require(fd.n >= 0 && !fstat(fd.n, &after) && Unchanged(before, after),
            "Source changed before packing: " + path);
    auto entry = LIBCALL(writer.lib, archive_entry_new)();
    Require(entry, "Cannot allocate archive entry.");
    try {
      LIBCALL(writer.lib, archive_entry_copy_stat)(entry, &before);
      // PAX headers must not acquire extra timestamps from our private staging tree.
      if (tar) {
        LIBCALL(writer.lib, archive_entry_unset_ctime)(entry);
        LIBCALL(writer.lib, archive_entry_unset_atime)(entry);
        LIBCALL(writer.lib, archive_entry_unset_birthtime)(entry);
      }
      // Haiku supplies UTF-8 independently of the worker's C locale.
      if (zip)
        LIBCALL(writer.lib, archive_entry_set_pathname_utf8)(entry, name.c_str());
      else
        LIBCALL(writer.lib, archive_entry_set_pathname)(entry, name.c_str());
      LIBCALL(writer.lib, archive_entry_set_perm)(entry, before.st_mode & 0777);
      const auto member = metadata.find(name);
      LIBCALL(writer.lib, archive_entry_set_uid)(entry, member == metadata.end() ? 0 : member->second.uid);
      LIBCALL(writer.lib, archive_entry_set_gid)(entry, member == metadata.end() ? 0 : member->second.gid);
      if (member != metadata.end()) {
        const auto &saved = member->second;
        Require(bool(S_ISDIR(before.st_mode)) == saved.directory, "TAR member type changed: " + name);
        LIBCALL(writer.lib, archive_entry_set_perm)(entry, saved.mode);
        LIBCALL(writer.lib, archive_entry_set_mtime)(entry, saved.modified, saved.nanoseconds);
        if (saved.accessed)
          LIBCALL(writer.lib, archive_entry_set_atime)(entry, saved.accessed->first, saved.accessed->second);
        if (saved.changed)
          LIBCALL(writer.lib, archive_entry_set_ctime)(entry, saved.changed->first, saved.changed->second);
        if (saved.created)
          LIBCALL(writer.lib, archive_entry_set_birthtime)(entry, saved.created->first, saved.created->second);
        LIBCALL(writer.lib, archive_entry_set_uname)(entry, member->second.user.c_str());
        LIBCALL(writer.lib, archive_entry_set_gname)(entry, member->second.group.c_str());
      }
      if (S_ISDIR(before.st_mode))
        LIBCALL(writer.lib, archive_entry_set_size)(entry, 0);
      writer.Check(LIBCALL(writer.lib, archive_write_header)(writer.a, entry));
      LIBCALL(writer.lib, archive_entry_free)(entry);
      entry = nullptr;
      if (S_ISREG(before.st_mode)) {
        off_t total = 0;
        for (;;) {
          Check();
          ssize_t n = read(fd.n, buffer.data(), buffer.size());
          if (n < 0 && errno == EINTR)
            continue;
          Require(n >= 0, "Cannot read source: " + path);
          if (!n)
            break;
          total += n;
          Require(total <= before.st_size,
                  "Source grew during packing: " + path);
          size_t offset = 0;
          while (offset < size_t(n)) {
            auto written = LIBCALL(writer.lib, archive_write_data)(
                writer.a, buffer.data() + offset, n - offset);
            Require(written > 0, "Cannot write archive data.");
            offset += written;
          }
          if (hooks.progress)
            hooks.progress(path, n);
        }
        Require(total == before.st_size,
                "Source size changed during packing: " + path);
      }
      writer.Check(LIBCALL(writer.lib, archive_write_finish_entry)(writer.a));
      if (S_ISDIR(before.st_mode)) {
        DIR *raw = fdopendir(dup(fd.n));
        Require(raw, "Cannot list source folder: " + path);
        std::unique_ptr<DIR, int (*)(DIR *)> dir(raw, closedir);
        for (;;) {
          errno = 0;
          auto child = readdir(raw);
          if (!child) {
            Require(errno == 0, "Cannot finish reading folder: " + path);
            break;
          }
          if (!strcmp(child->d_name, ".") || !strcmp(child->d_name, ".."))
            continue;
          Add(Join(path, child->d_name), Join(name, child->d_name), depth + 1);
        }
      }
      Require(!fstat(fd.n, &after) && Unchanged(before, after),
              "Source changed during packing: " + path);
      Require(!lstat(path.c_str(), &after) && Unchanged(before, after),
              "Source was replaced during packing: " + path);
    } catch (...) {
      if (entry)
        LIBCALL(writer.lib, archive_entry_free)(entry);
      throw;
    }
  }
};
}

bool IsTarFormat(const std::string &format) {
  return format == "tar" || format == "tgz" || format == "txz" ||
         format == "tbz2" || format == "tlz" || format == "tzst" ||
         format == "tlz4" || format == "tlzip" || format == "taz";
}

void CreateArchiveOutput(int output, const std::string &format,
                         const std::vector<std::string> &sources, int level,
                         bool encryptNames, const ArchiveSecretPtr &secret,
                         const Hooks &hooks, const TarMemberMap &metadata) {
  if (format == "ar" || format == "ar-bsd")
    ArCreate(output, sources, format == "ar-bsd", hooks);
  else if (format == "rar5")
    RarStoreWrite(output, sources, hooks);
  else if (format == "arj")
    ArjWrite(output, sources, hooks, level);
  else if (format == "cab")
    CabWrite(output, sources, level, hooks);
  else if (format == "lha")
    LhaCreate(output, sources, level, hooks);
  else if (secret && format == "7z")
    SevenZipCreate(output, sources, level, encryptNames, secret, hooks);
  else {
    // PAX and other libarchive formats may transcode filesystem names even
    // when the caller's worker is running in the C locale.
    ArchiveTextLocale locale;
    Writer writer;
    writer.Configure(format, level, secret, !metadata.empty());
    writer.fd = output;
    writer.Check(LIBCALL(writer.lib, archive_write_set_bytes_in_last_block)(writer.a, 1));
    writer.Check(LIBCALL(writer.lib, archive_write_open)(
        writer.a, &writer, nullptr, BudgetArchiveWrite, nullptr));
    Pack pack{writer, hooks, format == "zip", IsTarFormat(format), metadata};
    std::map<std::string, std::string> isoMembers;
    if (format == "iso")
      pack.isoMembers = &isoMembers;
    for (const auto &source : sources)
      pack.Add(source, Leaf(source));
    writer.Check(LIBCALL(writer.lib, archive_write_close)(writer.a));
    if (format == "iso")
      VerifyDataIsoOutput(writer.lib, output, std::move(isoMembers), hooks);
    pack.Verify();
  }
}
std::string DetectArchiveEditFormat(const std::string &path,
                                   const Hooks &hooks, TarMemberMap *metadata) {
  ArchiveTextLocale locale;
  const auto parts = FindArchiveVolumes(path);
  Require(
      parts.kind == ArchiveVolumes::Single,
      "Split archives cannot be edited. Extract them, change the files, and create a new archive.");
  const auto sevenZipFormat = SevenZipFormat(path);
  if (sevenZipFormat == "7z")
    return "7z";
  FD fd(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK));
  Require(fd.n >= 0, "Cannot open archive for editing.");
  if (sevenZipFormat == "rar5") {
    Require(RarStoreCanCreate(), "Install p7zip from HaikuDepot to edit RAR5 archives.");
    RarStoreValidateEdit(fd.n, hooks);
    return "rar5";
  }
  if (sevenZipFormat == "arj") {
    Require(ArjCanCreate(), "Install p7zip from HaikuDepot to edit ARJ archives.");
    ArjValidateEdit(fd.n);
    return "arj";
  }
  auto arFormat = ArEditFormat(fd.n, path, hooks);
  if (!arFormat.empty())
    return arFormat;
  unsigned char magic[4]{};
  auto got = read(fd.n, magic, sizeof(magic));
  if (got >= 4 && magic[0] == 'P' && magic[1] == 'K' &&
      ((magic[2] == 3 && magic[3] == 4) || (magic[2] == 5 && magic[3] == 6) ||
       (magic[2] == 7 && magic[3] == 8)))
    return "zip";
  Require(
      sevenZipFormat.empty(),
      "This archive format has no installed writer. It can be browsed and extracted, but cannot be changed.");
  DynamicLibrary lib{"libarchive.so", "libarchive.so.13"};
  Require(lib.Available(), "Install libarchive to edit this archive format.");
  struct Reader {
    DynamicLibrary &lib;
    archive *value;
    ~Reader() {
      if (value)
        LIBCALL(lib, archive_read_free)(value);
    }
  } reader{lib, LIBCALL(lib, archive_read_new)()};
  Require(reader.value, "Cannot inspect archive format.");
  LIBCALL(lib, archive_read_support_format_all)(reader.value);
  LIBCALL(lib, archive_read_support_filter_all)(reader.value);
  Require(lseek(fd.n, 0, SEEK_SET) == 0 &&
              LIBCALL(lib, archive_read_open_fd)(reader.value, fd.n, 65536) ==
                  ARCHIVE_OK,
          "Cannot inspect this archive for editing.");
  archive_entry *entry = nullptr;
  const int status =
      LIBCALL(lib, archive_read_next_header)(reader.value, &entry);
  Require(status == ARCHIVE_OK || status == ARCHIVE_EOF,
          "Archive is damaged or cannot be inspected for editing.");
  const int detected =
      LIBCALL(lib, archive_format)(reader.value) & ARCHIVE_FORMAT_BASE_MASK;
  if (metadata && detected == ARCHIVE_FORMAT_TAR)
    ReadTarMetadata(lib, reader.value, entry, status, hooks, *metadata);
  if (detected == ARCHIVE_FORMAT_CAB)
    CabValidateEdit(fd.n);
  if (detected == ARCHIVE_FORMAT_LHA)
    Require(LhaCanCreate(), "Install lha from HaikuDepot to edit this archive.");
  if (detected == ARCHIVE_FORMAT_ISO9660)
    RequireVerifiedDataIso(fd.n, hooks);
  std::string format = detected == ARCHIVE_FORMAT_TAR       ? "tar"
                       : detected == ARCHIVE_FORMAT_CPIO    ? "cpio"
                       : detected == ARCHIVE_FORMAT_CAB     ? "cab"
                       : detected == ARCHIVE_FORMAT_LHA     ? "lha"
                       : detected == ARCHIVE_FORMAT_XAR     ? "xar"
                       : detected == ARCHIVE_FORMAT_ISO9660 ? "iso"
                                                            : "";
  Require(
      !format.empty(),
      "This archive type has no qualified writer in Listi. It can be browsed and extracted; the original is unchanged.");
  for (int i = 0; i < LIBCALL(lib, archive_filter_count)(reader.value); ++i) {
    const int filter = LIBCALL(lib, archive_filter_code)(reader.value, i);
    if (filter == ARCHIVE_FILTER_NONE)
      continue;
    Require(
        format == "tar",
        "This archive compression wrapper cannot be preserved when editing.");
    switch (filter) {
    case ARCHIVE_FILTER_GZIP:
      format = "tgz";
      break;
    case ARCHIVE_FILTER_BZIP2:
      format = "tbz2";
      break;
    case ARCHIVE_FILTER_XZ:
      format = "txz";
      break;
    case ARCHIVE_FILTER_LZMA:
      format = "tlz";
      break;
    case ARCHIVE_FILTER_ZSTD:
      format = "tzst";
      break;
    case ARCHIVE_FILTER_LZ4:
      format = "tlz4";
      break;
    case ARCHIVE_FILTER_COMPRESS:
      format = "taz";
      break;
    case ARCHIVE_FILTER_LZIP:
      format = "tlzip";
      break;
    default:
      throw std::runtime_error(
          "This archive compression wrapper has no qualified writer. The original is unchanged.");
    }
  }
  if (format != "cab" && format != "lha") {
    Writer probe;
    probe.Configure(format, 6);
  }
  return format;
}

std::vector<ArchiveFormat> WritableArchiveFormats() {
  std::vector<ArchiveFormat> result;
  for (const auto &f : formats)
    try {
      if (f.id == "rar5") {
        if (RarStoreCanCreate())
          result.push_back(f);
        continue;
      }
      if (f.id == "arj") {
        if (ArjCanCreate()) {
          auto available = f;
          if (ArjCanCompress()) {
            available.label = "ARJ";
            available.compression = true;
          }
          result.push_back(available);
        }
        continue;
      }
      if (f.id == "cab") {
        auto available = f;
        available.compression = CabCanCompress();
        result.push_back(available);
        continue;
      }
      if (f.id == "ar" || f.id == "lha") {
        if (f.id == "ar" || LhaCanCreate())
          result.push_back(f);
        continue;
      }
      Writer w;
      w.Configure(f.id, 6);
      FD fd(open("/dev/null", O_WRONLY));
      Require(fd.n >= 0, "Cannot probe writer.");
      w.Check(LIBCALL(w.lib, archive_write_open_fd)(w.a, fd.n));
      w.Check(LIBCALL(w.lib, archive_write_close)(w.a));
      auto available = f;
      if (f.id == "zip") {
        // AES support is optional even when the ordinary ZIP writer works.
        try {
          auto probe = std::make_shared<ArchiveSecret>("Listi format probe");
          Writer encrypted;
          encrypted.Configure(f.id, 6, probe);
          available.encryption = true;
        } catch (const std::exception &) {
          available.encryption = false;
        }
      } else if (f.id == "7z" && SevenZipCanCreate()) {
        available.encryption = true;
        available.encryptedNames = true;
      }
      result.push_back(available);
    } catch (const std::exception &) {
    }
  return result;
}
}
