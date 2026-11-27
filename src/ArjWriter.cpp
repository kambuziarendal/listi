// SPDX-License-Identifier: MIT
#include "ArjWriter.h"
#include "StagingSpace.h"
#include "PrivateScratch.h"
#include "ArchiveBackend.h"
#include "ArchiveCompressor.h"
#include "LocalMoveTree.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace Listi {
namespace {
namespace fs = std::filesystem;
constexpr char kTool[] = "/boot/system/bin/arj";
constexpr uint16_t kSignature = 0xea60;
constexpr size_t kFixed = 30;
constexpr size_t kMaxName = 255;
constexpr uint8_t kArchiveHeader = 2;
constexpr uint8_t kDirectory = 3;
constexpr uint8_t kFile = 0;
constexpr char kArchiveName[] = "LISTI.ARJ";

void Require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}

void Put(unsigned char *bytes, uint32_t value, size_t count) {
  for (size_t i = 0; i < count; ++i)
    bytes[i] = value >> (i * 8);
}

uint32_t Get(const unsigned char *bytes, size_t count) {
  uint32_t value = 0;
  for (size_t i = 0; i < count; ++i)
    value |= uint32_t(bytes[i]) << (i * 8);
  return value;
}

constexpr auto CrcTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t index = 0; index < table.size(); ++index) {
    auto value = index;
    for (unsigned bit = 0; bit < 8; ++bit)
      value = (value >> 1) ^ ((value & 1) ? 0xedb88320 : 0);
    table[index] = value;
  }
  return table;
}

uint32_t Crc(uint32_t crc, const unsigned char *bytes, size_t count) {
  static constexpr auto table = CrcTable();
  crc = ~crc;
  for (size_t i = 0; i < count; ++i)
    crc = table[(crc ^ bytes[i]) & 255] ^ (crc >> 8);
  return ~crc;
}

void Write(int fd, const unsigned char *bytes, size_t count) {
  while (count) {
    const auto wrote = WriteStaging(fd, bytes, count);
    if (wrote < 0 && errno == EINTR)
      continue;
    Require(wrote > 0, "Cannot write ARJ output.");
    bytes += wrote;
    count -= wrote;
  }
}

void ReadAt(int fd, unsigned char *bytes, size_t count, off_t offset) {
  while (count) {
    const auto got = pread(fd, bytes, count, offset);
    if (got < 0 && errno == EINTR)
      continue;
    Require(got > 0, "ARJ header is damaged or truncated.");
    bytes += got;
    count -= got;
    offset += got;
  }
}

bool ValidName(const std::string &name) {
  if (name.empty() || name.size() > kMaxName ||
      name.find_first_of(":*?\"<>|\\") != std::string::npos ||
      std::any_of(name.begin(), name.end(),
                  [](unsigned char c) { return c < 32 || c >= 127; }))
    return false;
  return SafeArchivePath(name.c_str()) == name;
}

uint32_t DosTime(time_t stamp) {
  // The p7zip ARJ handler interprets the member time as local DOS time.
  std::tm date{};
  Require(localtime_r(&stamp, &date) && date.tm_year >= 80 &&
              date.tm_year <= 207,
          "ARJ can only store dates from 1980 to 2107.");
  return (uint32_t(date.tm_year - 80) << 25) | ((date.tm_mon + 1) << 21) |
         (date.tm_mday << 16) | (date.tm_hour << 11) | (date.tm_min << 5) |
         (date.tm_sec / 2);
}

using Header = std::vector<unsigned char>;
Header Block(uint8_t type, const std::string &name, uint32_t stamp,
             uint32_t size = 0, uint32_t crc = 0, uint8_t method = 0,
             uint32_t compressed = 0) {
  // A basic 30-byte header followed by an ASCIIZ name and empty comment.
  // Version 3, DOS host, no flags, and no extension headers.
  const size_t basic = kFixed + name.size() + 2;
  Header block(4 + basic + 4 + 2);
  Put(block.data(), kSignature, 2);
  Put(block.data() + 2, basic, 2);
  auto *fields = block.data() + 4;
  fields[0] = kFixed;
  fields[1] = 3;
  fields[2] = 1;
  fields[5] = method;
  fields[6] = type;
  Put(fields + 8, stamp, 4);
  if (type == kArchiveHeader) {
    Put(fields + 12, stamp, 4);
  } else {
    const auto slash = name.rfind('/');
    Put(fields + 12, method ? compressed : size, 4);
    Put(fields + 16, size, 4);
    Put(fields + 20, crc, 4);
    Put(fields + 24, slash == std::string::npos ? 0 : slash + 1, 2);
    Put(fields + 26, type == kDirectory ? 0x10 : 0x20, 2);
  }
  memcpy(fields + kFixed, name.data(), name.size());
  Put(fields + basic, Crc(0, fields, basic), 4);
  return block;
}

struct FD {
  int value;
  ~FD() {
    if (value >= 0)
      close(value);
  }
};

// Give the external codec synthetic names. It never opens user paths or
// interprets member names as switches, wildcards or list-file arguments.
struct Compression {
  struct Member {
    std::string name;
    uint32_t stamp, size, crc;
  };
  std::map<std::string, Member> members;
  std::unique_ptr<PrivateScratch> scratch;
  std::string path;
  explicit Compression(const std::vector<std::string> &sources) {
    const auto parent = fs::canonical("/tmp");
    for (const auto &source : sources) {
      const auto ancestor = parent.lexically_relative(fs::canonical(source));
      Require(ancestor.empty() || *ancestor.begin() == "..",
              "Cannot archive a folder that contains the ARJ staging folder.");
    }
    scratch = std::make_unique<PrivateScratch>("listi-arj-");
    path = scratch->path;
  }
  void Finish(int output, int level, const Hooks &hooks) {
    if (members.empty())
      return;
    const TransferControl control{hooks.proceed, {}};
    const std::string method = level < 4 ? "-m4" : level < 7 ? "-m2" : "-m1";
    RunArchiveCompressor(
        "ARJ", path,
        {kTool, "a", "-y", "-i", "-2d", "-e", method, "data.arj", "*.bin"},
        output, control);
    // Decode and check the compressed data before importing the payloads.
    RunArchiveCompressor("ARJ", path, {kTool, "t", "-y", "-i", "data.arj"},
                         output, control);
    FD file{open(Join(path, "data.arj").c_str(), O_RDONLY | O_NOFOLLOW)};
    struct stat st;
    Require(file.value >= 0 && !fstat(file.value, &st) && S_ISREG(st.st_mode),
            "Cannot inspect compressed ARJ data.");
    off_t at = 0;
    for (;;) {
      control.Check();
      unsigned char prefix[4];
      ReadAt(file.value, prefix, 4, at);
      const auto basic = Get(prefix + 2, 2);
      Require(Get(prefix, 2) == kSignature, "Invalid ARJ codec output.");
      if (!basic) {
        Require(members.empty() && st.st_size - at == 4,
                "ARJ codec output omitted a member.");
        return;
      }
      const bool main = !at;
      Require(basic >= kFixed + 2 && basic <= 4096,
              "Unexpected ARJ codec header.");
      Header header(basic + 6);
      ReadAt(file.value, header.data(), header.size(), at + 4);
      const auto *h = header.data();
      Require(h[0] >= kFixed && h[0] < basic &&
                  Get(h + basic, 4) == Crc(0, h, basic) &&
                  Get(h + basic + 4, 2) == 0,
              "Unexpected ARJ codec header extensions.");
      at += 4 + header.size();
      if (main) {
        Require(h[6] == kArchiveHeader, "Missing ARJ codec archive header.");
        continue;
      }
      const auto *name = reinterpret_cast<const char *>(h + h[0]);
      const auto found =
          members.find(std::string(name, strnlen(name, basic - h[0])));
      Require(found != members.end(), "Unexpected ARJ codec member.");
      const auto &m = found->second;
      const auto packed = Get(h + 12, 4);
      // PATHSYM marks the separator convention. Synthetic names have no path.
      Require(!(h[4] & ~0x10) && h[6] == kFile && h[5] <= 4 &&
                  (h[5] || m.size == packed) && Get(h + 16, 4) == m.size &&
                  Get(h + 20, 4) == m.crc && at <= st.st_size &&
                  packed <= uint64_t(st.st_size - at),
              "ARJ codec did not preserve the source data.");
      const auto block =
          Block(kFile, m.name, m.stamp, m.size, m.crc, h[5], packed);
      Write(output, block.data(), block.size());
      std::array<unsigned char, 65536> bytes;
      for (auto left = packed; left;) {
        control.Check();
        const auto count = std::min<size_t>(left, bytes.size());
        ReadAt(file.value, bytes.data(), count, at);
        Write(output, bytes.data(), count);
        left -= count;
        at += count;
      }
      members.erase(found);
    }
  }
};

struct Encoder {
  int output;
  const Hooks &hooks;
  std::set<std::string> names;
  Compression *compression;
  // Share one transfer buffer across recursive directory frames.
  std::vector<unsigned char> bytes = std::vector<unsigned char>(65536);
  void Check() const {
    if (hooks.proceed && !hooks.proceed())
      throw Cancelled();
  }

  void Add(const fs::path &path, const std::string &name) {
    Check();
    auto folded = name;
    for (auto &c : folded) {
      if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
    }
    Require(ValidName(name) && names.insert(folded).second,
            "ARJ requires unique, portable ASCII member names: " + name);
    FD input{open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK)};
    struct stat st;
    Require(input.value >= 0 && fstat(input.value, &st) == 0 &&
                (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)),
            "ARJ can only store ordinary files and folders: " + name);

    if (S_ISDIR(st.st_mode)) {
      const auto block = Block(kDirectory, name, DosTime(st.st_mtime));
      Write(output, block.data(), block.size());
      for (const auto &entry : fs::directory_iterator(path))
        Add(entry.path(), Join(name, entry.path().filename().string()));
      return;
    }

    Require(st.st_size >= 0 && uint64_t(st.st_size) <= UINT32_MAX,
            "ARJ entries must be smaller than 4 GiB: " + name);
    const auto at = lseek(output, 0, SEEK_CUR);
    Require(at >= 0, "Cannot seek to the ARJ member header.");
    const auto stamp = DosTime(st.st_mtime);
    auto block = Block(kFile, name, stamp, st.st_size);
    FD staged{-1};
    std::string id;
    if (compression) {
      id = std::to_string(compression->members.size()) + ".bin";
      staged.value = open(Join(compression->path, id).c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
      Require(staged.value >= 0, "Cannot stage ARJ source bytes.");
    } else {
      Write(output, block.data(), block.size());
    }
    uint32_t crc = 0;
    off_t left = st.st_size;
    while (left) {
      Check();
      const auto got =
          read(input.value, bytes.data(), std::min<off_t>(left, bytes.size()));
      if (got < 0 && errno == EINTR)
        continue;
      Require(got > 0,
              "ARJ source was truncated or became unreadable: " + name);
      crc = Crc(crc, bytes.data(), got);
      Write(compression ? staged.value : output, bytes.data(), got);
      left -= got;
      if (hooks.progress)
        hooks.progress(path.string(), got);
    }
    if (compression) {
      compression->members.emplace(
          id, Compression::Member{name, stamp, uint32_t(st.st_size), crc});
      return;
    }
    const auto end = lseek(output, 0, SEEK_CUR);
    Require(end > at && lseek(output, at, SEEK_SET) == at,
            "Cannot update the ARJ member CRC.");
    block = Block(kFile, name, stamp, st.st_size, crc);
    Write(output, block.data(), block.size());
    Require(lseek(output, end, SEEK_SET) == end, "Cannot seek in ARJ output.");
  }
};
} // namespace

bool ArjCanCompress() { return access(kTool, X_OK) == 0; }

void ArjWrite(int output, const std::vector<std::string> &sources,
              const Hooks &hooks, int level) {
  Require(level >= 0 && level <= 9, "Invalid ARJ compression level.");
  std::unique_ptr<Compression> compression;
  if (level && ArjCanCompress())
    compression = std::make_unique<Compression>(sources);
  LocalMoveTree frozen(sources, hooks, false);
  Encoder encoder{output, hooks, {}, compression.get()};
  const auto header =
      Block(kArchiveHeader, kArchiveName, DosTime(time(nullptr)));
  Write(output, header.data(), header.size());
  for (const auto &source : sources)
    encoder.Add(source, Leaf(source));
  if (compression)
    compression->Finish(output, level, hooks);
  unsigned char end[4]{};
  Put(end, kSignature, 2);
  Write(output, end, sizeof(end));
  encoder.Check();
  frozen.Verify();
}

void ArjValidateEdit(int input, const Hooks &hooks) {
  struct stat st;
  Require(fstat(input, &st) == 0 && S_ISREG(st.st_mode),
          "Cannot inspect the ARJ archive for editing.");
  const char *unsupported =
      "Only single-volume ARJ archives with the portable headers written by Listi can be edited.";
  off_t at = 0;
  while (at < st.st_size) {
    if (hooks.proceed && !hooks.proceed())
      throw Cancelled();
    unsigned char prefix[4];
    ReadAt(input, prefix, sizeof(prefix), at);
    const auto basic = Get(prefix + 2, 2);
    Require(Get(prefix, 2) == kSignature, unsupported);
    if (!basic) {
      Require(at != 0 && st.st_size - at == 4, unsupported);
      return;
    }
    Require(basic >= kFixed + 2 && basic <= kFixed + kMaxName + 2 &&
                st.st_size - at >= off_t(4 + basic + 6),
            unsupported);
    Header actual(4 + basic + 6);
    ReadAt(input, actual.data(), actual.size(), at);
    const auto *fields = actual.data() + 4;
    const auto type = fields[6];
    const auto nameBytes = reinterpret_cast<const char *>(fields + kFixed);
    const std::string name(nameBytes, strnlen(nameBytes, basic - kFixed));
    const auto packed = Get(fields + 12, 4);
    const auto size = Get(fields + 16, 4);
    const auto method = fields[5];
    const auto crc = Get(fields + 20, 4);
    const auto stamp = Get(fields + 8, 4);
    if (!at) {
      Require(type == kArchiveHeader && name == kArchiveName &&
                  actual == Block(kArchiveHeader, name, stamp),
              unsupported);
    } else {
      Require((type == kFile || type == kDirectory) && ValidName(name) &&
                  (type != kDirectory || (!size && !crc && !method)) &&
                  method <= 4 && (method || packed == size) &&
                  actual == Block(type, name, stamp, size, crc, method, packed),
              unsupported);
    }
    at += actual.size();
    if (type == kFile) {
      Require(packed <= uint64_t(st.st_size - at), unsupported);
      at += packed;
    }
  }
  throw std::runtime_error("ARJ archive has no complete end marker.");
}
} // namespace Listi
