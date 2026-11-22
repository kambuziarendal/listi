// SPDX-License-Identifier: MIT
#include "RarStore.h"
#include "StagingSpace.h"
#include "ArchiveBackend.h"
#include "LocalMoveTree.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

namespace Listi {
namespace {
// The on-disk fields below follow https://www.rarlab.com/technote.htm.
// No compression or decompression implementation is used by this writer.
using Bytes = std::vector<unsigned char>;
constexpr std::array<unsigned char, 8> kSignature{0x52, 0x61, 0x72, 0x21,
                                                  0x1a, 0x07, 0x01, 0x00};
constexpr unsigned kMain = 1, kFile = 2, kEnd = 5;
constexpr unsigned kExtra = 1, kData = 2, kSkipUnknown = 4;
constexpr unsigned kDirectory = 1, kMTime = 2, kCrc = 4;
constexpr unsigned kUnixHost = 1;
constexpr size_t kNameLimit = 32768;
constexpr size_t kEntryLimit = 200000;
constexpr const char *kCorrupt = "RAR5 headers are damaged or incomplete.";
constexpr const char *kUnsupported =
    "Only ordinary single-volume RAR5 archives without unsupported metadata "
    "can be edited. The original was not changed.";

void Require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
void Check(const Hooks &hooks) {
  if (hooks.proceed && !hooks.proceed())
    throw Cancelled();
}
bool Same(const struct stat &a, const struct stat &b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
         a.st_mode == b.st_mode && a.st_size == b.st_size &&
         a.st_nlink == b.st_nlink && a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
         a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
         a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
         a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
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
uint32_t Crc(uint32_t crc, const unsigned char *data, size_t size) {
  static constexpr auto table = CrcTable();
  crc = ~crc;
  while (size--)
    crc = table[(crc ^ *data++) & 255] ^ (crc >> 8);
  return ~crc;
}
void Write(int fd, const unsigned char *data, size_t size) {
  while (size) {
    const auto count = WriteStaging(fd, data, size);
    if (count < 0 && errno == EINTR)
      continue;
    Require(count > 0, "Cannot write RAR5 output.");
    data += count;
    size -= count;
  }
}
void ReadAt(int fd, off_t offset, unsigned char *data, size_t size) {
  while (size) {
    const auto count = pread(fd, data, size, offset);
    if (count < 0 && errno == EINTR)
      continue;
    Require(count > 0, kCorrupt);
    data += count;
    offset += count;
    size -= count;
  }
}
void VarInt(Bytes &out, uint64_t value) {
  while (value >= 128) {
    out.push_back((value & 127) | 128);
    value >>= 7;
  }
  out.push_back(value);
}
void UInt32(Bytes &out, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    out.push_back(value >> shift);
}
struct Fields {
  const unsigned char *at;
  const unsigned char *end;
  size_t Left() const { return end - at; }
  Fields Take(size_t size) {
    Require(size <= Left(), kCorrupt);
    const auto start = at;
    at += size;
    return {start, at};
  }
  uint64_t Var(unsigned max = 10) {
    uint64_t value = 0;
    for (unsigned index = 0; index < max; ++index) {
      const auto digit = *Take(1).at;
      Require(index != 9 || digit <= 1, kCorrupt);
      value |= uint64_t(digit & 127) << (index * 7);
      if (!(digit & 128))
        return value;
    }
    throw std::runtime_error(kCorrupt);
  }
  uint64_t Fixed(unsigned size) {
    const auto *bytes = Take(size).at;
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i)
      value |= uint64_t(bytes[i]) << (i * 8);
    return value;
  }
};

bool ValidName(const std::string &name) {
  if (name.empty() || name.size() > kNameLimit ||
      name.find_first_of("\\:*?\"<>|") != std::string::npos ||
      std::count(name.begin(), name.end(), '/') > 64)
    return false;
  // Disallow invalid UTF-8 and the RAR raw-Unix-name sentinel (U+FFFE).
  for (size_t i = 0; i < name.size();) {
    const auto first = static_cast<unsigned char>(name[i++]);
    if (first < 32 || first == 127)
      return false;
    if (first < 128)
      continue;
    const unsigned count = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
    if (first < 0xc2 || first > 0xf4 || name.size() - i < count)
      return false;
    uint32_t code = first & (127 >> (count + 1));
    for (unsigned j = 0; j < count; ++j) {
      const auto byte = static_cast<unsigned char>(name[i++]);
      if ((byte & 0xc0) != 0x80)
        return false;
      code = (code << 6) | (byte & 63);
    }
    constexpr uint32_t minimum[]{0, 0x80, 0x800, 0x10000};
    if (code < minimum[count] || code > 0x10ffff || code == 0xfffe ||
        (code >= 0xd800 && code <= 0xdfff))
      return false;
  }
  return SafeArchivePath(name.c_str()) == name;
}
void WriteBlock(int fd, const Bytes &body) {
  Bytes framed;
  VarInt(framed, body.size());
  framed.insert(framed.end(), body.begin(), body.end());
  Bytes crc;
  UInt32(crc, Crc(0, framed.data(), framed.size()));
  Write(fd, crc.data(), crc.size());
  Write(fd, framed.data(), framed.size());
}
Bytes Member(const struct stat &st, const std::string &name, uint32_t crc) {
  const bool directory = S_ISDIR(st.st_mode);
  const uint64_t size = directory ? 0 : st.st_size;
  const auto stamp = st.st_mtime;
  Require(stamp >= 0 && uint64_t(stamp) <= UINT32_MAX,
          "RAR5 Store requires modification times from 1970 to 2106: " + name);
  Bytes body;
  VarInt(body, kFile);
  VarInt(body, size ? kData : 0);
  if (size)
    VarInt(body, size);
  VarInt(body, kMTime | (directory ? kDirectory : kCrc));
  VarInt(body, size);
  // Portable Unix permission bits, plus the ordinary object type.
  VarInt(body, (directory ? 0040000 : 0100000) | (st.st_mode & 0777));
  UInt32(body, stamp);
  if (!directory)
    UInt32(body, crc);
  VarInt(body, 0); // Compression version 0, method 0 (Store).
  VarInt(body, kUnixHost);
  VarInt(body, name.size());
  body.insert(body.end(), name.begin(), name.end());
  return body;
}
struct FD {
  int value;
  ~FD() {
    if (value >= 0)
      close(value);
  }
};
struct Encoder {
  int output;
  const Hooks &hooks;
  std::map<std::string, bool> names;
  void Add(const std::filesystem::path &path, const std::string &name) {
    Check(hooks);
    Require(ValidName(name) && names.size() < kEntryLimit,
            "Unsupported RAR5 name or too many entries: " + name);
    FD source{open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK)};
    struct stat before, after;
    Require(source.value >= 0 && !fstat(source.value, &before) &&
                (S_ISDIR(before.st_mode) ||
                 (S_ISREG(before.st_mode) && before.st_nlink == 1 &&
                  before.st_size >= 0)) &&
                !(before.st_mode & 07000),
            "RAR5 Store requires ordinary files and folders: " + name);
    const bool directory = S_ISDIR(before.st_mode);
    Require(names.emplace(name, directory).second,
            "Duplicate RAR5 member name: " + name);
    if (directory) {
      WriteBlock(output, Member(before, name, 0));
      for (const auto &entry : std::filesystem::directory_iterator(path))
        Add(entry.path(), Join(name, entry.path().filename().string()));
    } else {
      WriteFile(source.value, before, path.string(), name);
    }
    Require(!fstat(source.value, &after) && Same(before, after) &&
                !lstat(path.c_str(), &after) && Same(before, after),
            "Source changed during RAR5 packing: " + name);
  }
  void WriteFile(int input, const struct stat &st, const std::string &path,
                 const std::string &name) {
    const auto start = lseek(output, 0, SEEK_CUR);
    Require(start >= 0, "Cannot seek to the RAR5 member header.");
    WriteBlock(output, Member(st, name, 0));
    std::array<unsigned char, 65536> bytes;
    uint32_t crc = 0;
    off_t left = st.st_size;
    while (left) {
      Check(hooks);
      const auto got =
          read(input, bytes.data(), std::min<off_t>(left, bytes.size()));
      if (got < 0 && errno == EINTR)
        continue;
      Require(got > 0, "RAR5 source changed or became unreadable: " + name);
      crc = Crc(crc, bytes.data(), got);
      Write(output, bytes.data(), got);
      left -= got;
      if (hooks.progress)
        hooks.progress(path, got);
    }
    const auto end = lseek(output, 0, SEEK_CUR);
    Require(end > start && lseek(output, start, SEEK_SET) == start,
            "Cannot update the RAR5 member CRC.");
    WriteBlock(output, Member(st, name, crc));
    Require(lseek(output, end, SEEK_SET) == end, "Cannot seek in RAR5 output.");
  }
};

Fields ExtraRecord(Fields &extra) {
  const auto size = extra.Var();
  Require(size && size <= extra.Left(), kCorrupt);
  return extra.Take(size);
}
void CheckMainExtra(Fields extra) {
  if (!extra.Left())
    return;
  // A reserved locator with zero offsets does not reference service data.
  auto record = ExtraRecord(extra);
  Require(!extra.Left() && record.Var() == 1, kUnsupported);
  const auto flags = record.Var();
  Require(!(flags & ~3ULL), kUnsupported);
  for (unsigned bit : {1u, 2u}) {
    if (flags & bit)
      Require(!record.Var(), kUnsupported);
  }
  Require(!record.Left(), kUnsupported);
}
void CheckFileExtra(Fields extra) {
  if (!extra.Left())
    return;
  auto record = ExtraRecord(extra);
  Require(!extra.Left() && record.Var() == 3, kUnsupported);
  // Only an mtime exactly representable by the second-precision reader.
  const auto flags = record.Var();
  if (flags == 2) {
    constexpr uint64_t epoch = 116444736000000000ULL, perSecond = 10000000;
    const auto ticks = record.Fixed(8);
    Require(ticks >= epoch && (ticks - epoch) % perSecond == 0 &&
                (ticks - epoch) / perSecond <= UINT32_MAX,
            kUnsupported);
  } else {
    Require(flags == 3 || flags == 0x13, kUnsupported);
    record.Fixed(4);
    if (flags == 0x13)
      Require(!record.Fixed(4), kUnsupported);
  }
  Require(!record.Left(), kUnsupported);
}
void CheckFile(Fields fields, Fields extra, uint64_t packed,
               std::map<std::string, bool> &names) {
  const auto flags = fields.Var();
  Require(!(flags & ~(uint64_t(kDirectory) | kMTime | kCrc)), kUnsupported);
  const bool directory = flags & kDirectory;
  const auto size = fields.Var();
  const auto attributes = fields.Var();
  if (flags & kMTime)
    fields.Fixed(4);
  if (flags & kCrc)
    fields.Fixed(4);
  const auto compression = fields.Var();
  const auto host = fields.Var();
  const auto length = fields.Var();
  Require(size <= INT64_MAX && length <= kNameLimit, kUnsupported);
  const auto *nameBytes = fields.Take(length).at;
  const std::string name(reinterpret_cast<const char *>(nameBytes), length);
  Require(!fields.Left() && ValidName(name) && names.size() < kEntryLimit &&
              names.emplace(name, directory).second,
          kUnsupported);
  // Version 0 is the version read by the optional Haiku p7zip handler.
  const auto method = (compression >> 7) & 7;
  Require(!(compression & ~0x3fc0ULL) && method <= 5 &&
              (method != 0 || packed == size) &&
              (!directory || (!packed && !size && !method)) &&
              (!size || packed),
          kUnsupported);
  if (host == kUnixHost) {
    const auto mode = attributes & ~0777ULL;
    Require(!mode || mode == (directory ? 0040000u : 0100000u), kUnsupported);
  } else {
    // The archive and directory bits carry no permissions or alternate data.
    Require(host == 0 && !(attributes & ~(directory ? 0x30ULL : 0x20ULL)),
            kUnsupported);
  }
  CheckFileExtra(extra);
}
void CheckTree(const std::map<std::string, bool> &names, const Hooks &hooks) {
  for (const auto &entry : names) {
    Check(hooks);
    auto parent = entry.first;
    for (auto slash = parent.rfind('/'); slash != std::string::npos;
         slash = parent.rfind('/')) {
      parent.resize(slash);
      const auto it = names.find(parent);
      Require(it == names.end() || it->second, kUnsupported);
    }
  }
}
} // namespace

void RarStoreWrite(int output, const std::vector<std::string> &sources,
                   const Hooks &hooks) {
  LocalMoveTree frozen(sources, hooks, false);
  Encoder encoder{output, hooks, {}};
  Write(output, kSignature.data(), kSignature.size());
  WriteBlock(output, {kMain, 0, 0});
  for (const auto &source : sources)
    encoder.Add(source, Leaf(source));
  WriteBlock(output, {kEnd, 0, 0});
  Check(hooks);
  frozen.Verify();
}

void RarStoreValidateEdit(int input, const Hooks &hooks) {
  struct stat before, after;
  Require(!fstat(input, &before) && S_ISREG(before.st_mode),
          "Cannot inspect RAR5 archive for editing.");
  std::array<unsigned char, kSignature.size()> signature;
  ReadAt(input, 0, signature.data(), signature.size());
  Require(signature == kSignature,
          "Only RAR5 archives without a self-extracting stub can be edited.");
  off_t offset = kSignature.size();
  bool main = false;
  std::map<std::string, bool> names;
  while (offset < before.st_size) {
    Check(hooks);
    std::array<unsigned char, 7> prefix;
    Require(before.st_size - offset >= off_t(prefix.size()), kCorrupt);
    ReadAt(input, offset, prefix.data(), prefix.size());
    Fields lead{prefix.data(), prefix.data() + prefix.size()};
    const auto crc = lead.Fixed(4);
    const auto size = lead.Var(3);
    const auto sizeBytes = prefix.size() - 4 - lead.Left();
    offset += 4;
    Require(size >= 3 && size + sizeBytes <= uint64_t(before.st_size - offset),
            kCorrupt);
    Bytes block(size + sizeBytes);
    ReadAt(input, offset, block.data(), block.size());
    Require(Crc(0, block.data(), block.size()) == crc, kCorrupt);
    offset += block.size();

    Fields fields{block.data() + sizeBytes, block.data() + block.size()};
    const auto type = fields.Var();
    const auto flags = fields.Var();
    Require(!(flags & ~(uint64_t(kExtra) | kData | kSkipUnknown)),
            kUnsupported);
    const auto extraSize = flags & kExtra ? fields.Var() : 0;
    const auto packed = flags & kData ? fields.Var() : 0;
    Require(extraSize <= fields.Left() &&
                packed <= uint64_t(before.st_size - offset),
            kCorrupt);
    Fields extra{fields.end - extraSize, fields.end};
    fields.end = extra.at;
    if (!main) {
      Require(type == kMain && !(flags & kData) && !(fields.Var() & ~4ULL) &&
                  !fields.Left(),
              kUnsupported);
      CheckMainExtra(extra);
      main = true;
    } else if (type == kEnd) {
      Require(!(flags & (kExtra | kData)) && !fields.Var() && !fields.Left() &&
                  offset == before.st_size,
              kUnsupported);
      CheckTree(names, hooks);
      Require(!fstat(input, &after) && Same(before, after),
              "RAR5 archive changed during validation.");
      return;
    } else {
      Require(type == kFile, kUnsupported);
      CheckFile(fields, extra, packed, names);
    }
    offset += packed;
  }
  throw std::runtime_error(kCorrupt);
}
} // namespace Listi
