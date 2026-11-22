// SPDX-License-Identifier: MIT
#include "CabWriter.h"
#include "StagingSpace.h"
#include "ArchivePasswords.h"
#include "DynamicLibrary.h"
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <ctime>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace Listi {
namespace {
constexpr uint32_t kHeaderSize = 36;
constexpr uint32_t kFolderSize = 8;
constexpr uint32_t kFileSize = 16;
constexpr uint32_t kFilesOffset = kHeaderSize + kFolderSize;
constexpr uint32_t kBlockSize = 32768;
constexpr uint32_t kMaxBlocks = UINT16_MAX;
constexpr uint32_t kMaxFiles = UINT16_MAX;
constexpr size_t kMaxName = 255;
constexpr uint16_t kReadOnly = 0x01;
constexpr uint16_t kArchive = 0x20;
constexpr uint16_t kUtf8 = 0x80;

void Require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}

uint32_t U16(const unsigned char *field) {
  return field[0] | (uint32_t(field[1]) << 8);
}
uint32_t U32(const unsigned char *field) {
  return U16(field) | (U16(field + 2) << 16);
}
void Put16(unsigned char *field, uint32_t value) {
  field[0] = value & 255;
  field[1] = (value >> 8) & 255;
}
void Put32(unsigned char *field, uint32_t value) {
  Put16(field, value);
  Put16(field + 2, value >> 16);
}

void ReadAt(int fd, void *buffer, size_t length, off_t offset) {
  auto *bytes = static_cast<unsigned char *>(buffer);
  while (length) {
    auto got = pread(fd, bytes, length, offset);
    if (got < 0 && errno == EINTR)
      continue;
    Require(got > 0, "Cabinet data is truncated or unreadable.");
    bytes += got;
    offset += got;
    length -= got;
  }
}
void Write(int fd, const void *buffer, size_t length) {
  const auto *bytes = static_cast<const unsigned char *>(buffer);
  while (length) {
    auto wrote = WriteStaging(fd, bytes, length);
    if (wrote < 0 && errno == EINTR)
      continue;
    Require(wrote > 0, "Cannot write Cabinet data.");
    bytes += wrote;
    length -= wrote;
  }
}

bool ValidUtf8(const std::string &name) {
  for (size_t i = 0; i < name.size();) {
    const auto first = static_cast<unsigned char>(name[i++]);
    if (first < 0x80)
      continue;
    const unsigned count = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
    if (first < 0xc2 || first > 0xf4 || i + count > name.size())
      return false;
    const auto next = static_cast<unsigned char>(name[i]);
    if ((first == 0xe0 && next < 0xa0) || (first == 0xed && next > 0x9f) ||
        (first == 0xf0 && next < 0x90) || (first == 0xf4 && next > 0x8f))
      return false;
    for (unsigned j = 0; j < count; ++j)
      if ((static_cast<unsigned char>(name[i++]) & 0xc0) != 0x80)
        return false;
  }
  return true;
}
bool HasNonAscii(const std::string &name) {
  return std::any_of(name.begin(), name.end(),
                     [](unsigned char c) { return c >= 128; });
}

std::string AsciiFold(std::string name) {
  for (auto &character : name) {
    if (character >= 'A' && character <= 'Z')
      character += 'a' - 'A';
  }
  return name;
}
bool ValidName(const std::string &name) {
  if (name.empty() || name.size() > kMaxName ||
      name.find_first_of("\\:*?\"<>|") != std::string::npos || !ValidUtf8(name))
    return false;
  size_t begin = 0;
  for (;;) {
    const auto end = name.find('/', begin);
    const auto limit = end == std::string::npos ? name.size() : end;
    if (begin == limit || name[limit - 1] == ' ' || name[limit - 1] == '.')
      return false;
    const auto dot = std::min(name.find('.', begin), limit);
    const auto base = AsciiFold(name.substr(begin, dot - begin));
    if (base == "con" || base == "prn" || base == "aux" || base == "nul" ||
        (base.size() == 4 && base[3] >= '1' && base[3] <= '9' &&
         (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0)))
      return false;
    for (size_t i = begin; i < limit; ++i) {
      if (static_cast<unsigned char>(name[i]) < 32)
        return false;
    }
    if (end == std::string::npos)
      return true;
    begin = end + 1;
  }
}
struct FD {
  int n;
  explicit FD(int value) : n(value) {}
  ~FD() {
    if (n >= 0)
      close(n);
  }
};
struct Node {
  std::string path, name, identity;
  uint32_t size = 0;
  uint16_t date = 0, time = 0;
  uint16_t attributes = kArchive;
};
struct Packed {
  const Hooks &hooks;
  std::map<std::string, Node> nodes;
  std::vector<const Node *> files;
  uint32_t fileTableSize = 0, totalBytes = 0;
  void Check() const {
    if (hooks.proceed && !hooks.proceed())
      throw Cancelled();
  }
  void Capture(const std::string &path, const std::string &name,
               unsigned depth = 0) {
    Check();
    Require(depth <= 64 && nodes.size() < kMaxFiles && ValidName(name),
            "Cabinet entry name or folder structure is unsupported: " + name);
    struct stat st;
    FD fd(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW));
    Require(fd.n >= 0 && !fstat(fd.n, &st) &&
                (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)),
            "Cabinet can only pack regular files and folders: " + path);
    auto inserted = nodes.emplace(
        AsciiFold(name), Node{path, name, ArchivePasswordIdentity(st)});
    Require(inserted.second,
            "Cabinet paths differ only in ASCII case: " + name);
    auto &node = inserted.first->second;
    if (S_ISREG(st.st_mode)) {
      Require(st.st_size >= 0 &&
                  uint64_t(st.st_size) <= kBlockSize * kMaxBlocks - totalBytes,
              "Cabinet exceeds the single-volume size limit.");
      std::tm time{};
      Require(localtime_r(&st.st_mtime, &time) && time.tm_year >= 80 &&
                  time.tm_year <= 207,
              "Cabinet cannot store this file's modification date: " + path);
      node.date =
          ((time.tm_year - 80) << 9) | ((time.tm_mon + 1) << 5) | time.tm_mday;
      node.time = (time.tm_hour << 11) | (time.tm_min << 5) | (time.tm_sec / 2);
      if (!(st.st_mode & 0222))
        node.attributes |= kReadOnly;
      if (HasNonAscii(name))
        node.attributes |= kUtf8;
      node.size = st.st_size;
      totalBytes += node.size;
      fileTableSize += kFileSize + name.size() + 1;
      files.push_back(&node);
      return;
    }

    const auto start = files.size();
    FD directoryFd(dup(fd.n));
    DIR *raw = directoryFd.n < 0 ? nullptr : fdopendir(directoryFd.n);
    Require(raw, "Cannot list Cabinet source folder: " + path);
    directoryFd.n = -1; // fdopendir owns the descriptor on success.
    std::unique_ptr<DIR, decltype(&closedir)> directory(raw, closedir);
    for (;;) {
      errno = 0;
      const auto *child = readdir(directory.get());
      if (!child) {
        Require(errno == 0, "Cannot finish reading Cabinet source folder.");
        break;
      }
      const std::string leaf = child->d_name;
      if (leaf != "." && leaf != "..")
        Capture(Join(path, leaf), Join(name, leaf), depth + 1);
    }
    Require(start < files.size(),
            "Cabinet cannot store an empty folder: " + path);
  }
  void Verify() const {
    for (const auto &item : nodes) {
      Check();
      const auto &node = item.second;
      struct stat st;
      Require(!lstat(node.path.c_str(), &st) &&
                  ArchivePasswordIdentity(st) == node.identity,
              "Cabinet source changed while packing: " + node.path);
    }
  }
};

uint32_t Checksum(const unsigned char *data, uint32_t size,
                  uint32_t originalSize) {
  uint32_t checksum = size | (originalSize << 16);
  while (size >= 4) {
    checksum ^= U32(data);
    size -= 4;
    data += 4;
  }
  uint32_t tail = 0;
  for (uint32_t i = 0; i < size; ++i)
    tail = (tail << 8) | data[i];
  return checksum ^ tail;
}
// Each MSZIP block contains "CK" and an independent raw Deflate stream.
// zlib's compress2 writes that same stream between a two-byte zlib header
// (no preset dictionary) and a four-byte Adler-32 trailer (RFC 1950).
struct Zlib {
  DynamicLibrary library{"libz.so.1", "libz.so"};
  decltype(&compress2) compress = nullptr;
  decltype(&compressBound) bound = nullptr;
  Zlib() {
    if (library.Available()) {
      bound = LIBCALL(library, compressBound);
      compress = LIBCALL(library, compress2);
    }
  }
};
const Zlib &Codec() {
  static const Zlib codec;
  return codec;
}
struct BlockEncoder {
  decltype(&compress2) compress = nullptr;
  std::vector<unsigned char> buffer;

  explicit BlockEncoder(int level) {
    if (level && CabCanCompress()) {
      compress = Codec().compress;
      buffer.resize(Codec().bound(kBlockSize));
    }
  }
  size_t WriteBlock(int output, const unsigned char *bytes, size_t originalSize,
                    int level) {
    size_t size = originalSize;
    if (compress) {
      uLongf length = buffer.size();
      Require(compress(buffer.data(), &length, bytes, originalSize, level) ==
                      Z_OK &&
                  length >= 6 && length - 4 <= UINT16_MAX,
              "Cannot compress Cabinet data.");
      buffer[0] = 'C';
      buffer[1] = 'K';
      bytes = buffer.data();
      size = length - 4;
    }
    unsigned char header[8]{};
    Put32(header, Checksum(bytes, size, originalSize));
    Put16(header + 4, size);
    Put16(header + 6, originalSize);
    Write(output, header, sizeof(header));
    Write(output, bytes, size);
    return sizeof(header) + size;
  }
};
} // namespace

bool CabCanCompress() {
  try {
    return Codec().compress != nullptr;
  } catch (const std::runtime_error &) {
    return false;
  }
}

void CabWrite(int output, const std::vector<std::string> &sources, int level,
              const Hooks &hooks) {
  Require(level >= 0 && level <= 9, "Invalid Cabinet compression level.");
  BlockEncoder encoder(level);
  Packed packed{hooks};
  for (const auto &source : sources)
    packed.Capture(source, Leaf(source));
  Require(!packed.files.empty(), "Cabinet requires at least one file.");

  const uint32_t blocks = (packed.totalBytes + kBlockSize - 1) / kBlockSize;
  const uint32_t dataOffset = kFilesOffset + packed.fileTableSize;
  uint32_t cabinetSize = dataOffset;
  unsigned char header[kFilesOffset]{};
  memcpy(header, "MSCF", 4);
  Put32(header + 16, kFilesOffset);
  header[24] = 3;
  header[25] = 1;
  Put16(header + 26, 1);
  Put16(header + 28, packed.files.size());
  Put32(header + 36, dataOffset);
  Put16(header + 40, blocks);
  Put16(header + 42, encoder.compress ? 1 : 0);
  Write(output, header, sizeof(header));

  uint32_t fileOffset = 0;
  for (const auto *node : packed.files) {
    packed.Check();
    unsigned char file[kFileSize]{};
    Put32(file, node->size);
    Put32(file + 4, fileOffset);
    Put16(file + 10, node->date);
    Put16(file + 12, node->time);
    Put16(file + 14, node->attributes);
    Write(output, file, sizeof(file));
    auto name = node->name;
    std::replace(name.begin(), name.end(), '/', '\\');
    Write(output, name.c_str(), name.size() + 1);
    fileOffset += node->size;
  }

  auto buffer = std::make_unique<unsigned char[]>(kBlockSize);
  uint32_t used = 0;
  for (const auto *node : packed.files) {
    packed.Check();
    FD fd(open(node->path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW));
    struct stat st;
    Require(fd.n >= 0 && !fstat(fd.n, &st) && S_ISREG(st.st_mode) &&
                ArchivePasswordIdentity(st) == node->identity,
            "Cabinet source changed before reading: " + node->path);
    for (uint32_t offset = 0; offset < node->size;) {
      packed.Check();
      auto size = std::min(kBlockSize - used, node->size - offset);
      ReadAt(fd.n, buffer.get() + used, size, offset);
      used += size;
      offset += size;
      if (used == kBlockSize) {
        cabinetSize += encoder.WriteBlock(output, buffer.get(), used, level);
        used = 0;
      }
      if (hooks.progress)
        hooks.progress(node->path, size);
    }
  }
  if (used)
    cabinetSize += encoder.WriteBlock(output, buffer.get(), used, level);
  packed.Verify();
  unsigned char sizeField[4];
  Put32(sizeField, cabinetSize);
  ssize_t wrote;
  do {
    wrote = pwrite(output, sizeField, sizeof(sizeField), 8);
  } while (wrote < 0 && errno == EINTR);
  Require(wrote == sizeof(sizeField), "Cannot finalize Cabinet size.");
}

void CabValidateEdit(int input) {
  struct stat st;
  unsigned char header[kHeaderSize]{};
  Require(!fstat(input, &st) && S_ISREG(st.st_mode),
          "Cannot inspect Cabinet input.");
  ReadAt(input, header, sizeof(header), 0);
  const uint32_t size = U32(header + 8);
  const uint32_t folders = U16(header + 26);
  const uint32_t files = U16(header + 28);
  const uint32_t fileOffset = U32(header + 16);
  Require(!memcmp(header, "MSCF", 4) && size == uint64_t(st.st_size) &&
              !U32(header + 4) && !U32(header + 12) && !U32(header + 20) &&
              header[24] == 3 && header[25] == 1 && !U16(header + 30) &&
              !U16(header + 34) && folders &&
              fileOffset == kHeaderSize + kFolderSize * folders &&
              fileOffset < size,
          "Signed, spanned, reserved or damaged Cabinets cannot be changed.");
  uint32_t firstData = size;
  for (uint32_t i = 0; i < folders; ++i) {
    unsigned char folder[kFolderSize]{};
    ReadAt(input, folder, sizeof(folder), kHeaderSize + kFolderSize * i);
    const auto offset = U32(folder);
    Require(offset > fileOffset && offset <= size,
            "Cabinet folder data offset is invalid.");
    firstData = std::min(firstData, offset);
  }

  uint32_t at = fileOffset;
  for (uint32_t i = 0; i < files; ++i) {
    unsigned char entry[kFileSize]{};
    Require(at <= firstData && firstData - at > kFileSize,
            "Cabinet file table is truncated.");
    ReadAt(input, entry, sizeof(entry), at);
    at += sizeof(entry);
    const auto attributes = U16(entry + 14);
    Require(
        U16(entry + 8) < folders &&
            !(attributes & ~(kReadOnly | kArchive | kUtf8)),
        "Cabinet has file attributes or spanning data that cannot be preserved.");
    std::array<char, kMaxName + 1> name{};
    const auto limit = std::min<size_t>(name.size(), firstData - at);
    ReadAt(input, name.data(), limit, at);
    const auto *end = static_cast<const char *>(memchr(name.data(), 0, limit));
    Require(end != nullptr, "Cabinet entry name is invalid.");
    std::string path(name.data(), end - name.data());
    std::replace(path.begin(), path.end(), '\\', '/');
    Require(ValidName(path) && (!HasNonAscii(path) || (attributes & kUtf8)),
            "Cabinet entry name cannot be preserved as UTF-8.");
    at += path.size() + 1;
  }
  Require(at == firstData,
          "Cabinet file table contains unsupported extra metadata.");
}
} // namespace Listi
