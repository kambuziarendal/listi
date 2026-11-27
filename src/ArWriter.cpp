// SPDX-License-Identifier: MIT
#include "ArWriter.h"
#include "StagingSpace.h"
#include "ArchiveBackend.h"
#include "ArchiveVolumes.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <set>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace Listi {
namespace {
constexpr std::string_view kMagic = "!<arch>\n";
constexpr std::string_view kThinMagic = "!<thin>\n";
constexpr size_t kHeaderSize = 60;
constexpr size_t kMaxMembers = 200000;
constexpr size_t kMaxTableSize = 64 * 1024 * 1024;
constexpr uint64_t kMaxSize = 9999999999ULL;

void Require(bool condition, const std::string &message) {
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

size_t ReadAt(int fd, void *data, size_t length, off_t offset) {
  auto *bytes = static_cast<char *>(data);
  size_t have = 0;
  while (have < length) {
    const auto count = pread(fd, bytes + have, length - have, offset + have);
    if (count < 0 && errno == EINTR)
      continue;
    Require(count >= 0, "Cannot read Unix ar data.");
    if (!count)
      break;
    have += count;
  }
  return have;
}

void ReadExact(int fd, void *data, size_t length, off_t offset) {
  Require(ReadAt(fd, data, length, offset) == length,
          "Unix ar archive is truncated.");
}

void Write(int fd, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto count = WriteStaging(fd, bytes.data(), bytes.size());
    if (count < 0 && errno == EINTR)
      continue;
    Require(count > 0, "Cannot write Unix ar data.");
    bytes.remove_prefix(count);
  }
}

enum class Signature { Other, Normal, Thin };

Signature Detect(int fd) {
  std::array<char, kMagic.size()> bytes;
  if (ReadAt(fd, bytes.data(), bytes.size(), 0) != bytes.size())
    return Signature::Other;
  const std::string_view magic(bytes.data(), bytes.size());
  if (magic == kMagic)
    return Signature::Normal;
  if (magic == kThinMagic)
    return Signature::Thin;
  return Signature::Other;
}

void CheckRegularAr(int fd) {
  struct stat state;
  Require(fd >= 0 && !fstat(fd, &state) && S_ISREG(state.st_mode),
          "Cannot open Unix ar archive.");
  const auto signature = Detect(fd);
  Require(
      signature != Signature::Thin,
      "Thin ar libraries reference external files. Listi does not open or change them.");
  Require(signature == Signature::Normal, "Not a Unix ar archive.");
}

bool SameState(const struct stat &a, const struct stat &b) {
  return a.st_mode == b.st_mode && a.st_dev == b.st_dev &&
         a.st_ino == b.st_ino && a.st_size == b.st_size &&
         a.st_uid == b.st_uid && a.st_gid == b.st_gid &&
         a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
         a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
         a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
         a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}

void VerifySame(int fd, const std::string &path, const struct stat &before) {
  struct stat opened, named;
  Require(!fstat(fd, &opened) && !lstat(path.c_str(), &named) &&
              SameState(before, opened) && SameState(before, named),
          "File changed while processing Unix ar: " + path);
}

std::string_view TrimRight(std::string_view field) {
  while (!field.empty() && field.back() == ' ')
    field.remove_suffix(1);
  return field;
}

uint64_t Number(std::string_view field, unsigned base, bool optional = false) {
  field = TrimRight(field);
  while (!field.empty() && field.front() == ' ')
    field.remove_prefix(1);
  Require(optional || !field.empty(), "Unix ar header has an empty number.");
  uint64_t value = 0;
  for (unsigned char c : field) {
    Require(c >= '0' && c < '0' + base &&
                value <=
                    (std::numeric_limits<uint64_t>::max() - (c - '0')) / base,
            "Unix ar header has an invalid number.");
    value = value * base + (c - '0');
  }
  return value;
}

bool SymbolIndex(std::string_view name) {
  return name == "/" || name == "/SYM64/" || name == "__.SYMDEF" ||
         name == "__.SYMDEF SORTED" || name == "__.SYMDEF_64" ||
         name == "__.SYMDEF_64 SORTED" || name == "__.GHS_SYMDEF" ||
         name == "__.LLVM_SYM_TAB";
}

bool EncodedName(const std::string &name) {
  return name.find_first_of(" \t/") != std::string::npos || name.size() > 15 ||
         name.rfind("#1/", 0) == 0;
}

struct Member {
  std::string name;
  off_t offset = 0;
  uint64_t size = 0;
  int64_t modified = 0;
  uint64_t uid = 0, gid = 0;
  unsigned mode = 0;
  bool normalized = false;
};

struct Index {
  struct stat state {};
  std::vector<Member> members;
  bool bsd = false, gnu = false, symbolIndex = false;
};

std::string GnuName(const std::string &table, size_t start) {
  Require(start < table.size() &&
              (!start || table[start - 1] == '\n' || table[start - 1] == '\0'),
          "Unix ar name table has an invalid reference.");
  const auto end = table.find_first_of(std::string_view("\n\0", 2), start);
  Require(end != std::string::npos && end - start <= 32769,
          "Unix ar name table is invalid.");
  std::string name = table.substr(start, end - start);
  if (!name.empty() && table[end] == '\n' && name.back() == '/')
    name.pop_back();
  return name;
}

Index ReadIndex(int fd, const TransferControl &control) {
  control.Check();
  CheckRegularAr(fd);
  Index result;
  Require(!fstat(fd, &result.state) &&
              result.state.st_size >= off_t(kMagic.size()),
          "Cannot inspect Unix ar file size.");
  std::string table;
  bool haveTable = false;
  std::set<std::string> names;
  size_t nameBytes = 0;
  size_t headers = 0;
  for (off_t offset = kMagic.size(); offset < result.state.st_size;) {
    control.Check();
    Require(++headers <= kMaxMembers, "Unix ar has too many entries.");
    std::array<char, kHeaderSize> bytes;
    Require(result.state.st_size - offset >= off_t(bytes.size()),
            "Unix ar member header is truncated.");
    ReadExact(fd, bytes.data(), bytes.size(), offset);
    const std::string_view header(bytes.data(), bytes.size());
    Require(header.substr(58) == "`\n", "Unix ar member header is invalid.");
    const auto raw = TrimRight(header.substr(0, 16));
    const auto size = Number(header.substr(48, 10), 10);
    const auto dataOffset = offset + bytes.size();
    Require(size + (size % 2) <= uint64_t(result.state.st_size - dataOffset),
            "Unix ar member body or padding is truncated.");
    offset = dataOffset + size + (size % 2);
    if (raw == "//") {
      Require(!haveTable && size <= kMaxTableSize,
              "Unix ar name table is repeated or too large.");
      haveTable = true;
      result.gnu = true;
      table.resize(size);
      ReadExact(fd, table.data(), table.size(), dataOffset);
      continue;
    }
    if (SymbolIndex(raw)) {
      result.symbolIndex = true;
      continue;
    }
    Member member;
    member.offset = dataOffset;
    member.size = size;
    if (raw.substr(0, 3) == "#1/") {
      result.bsd = true;
      const auto length = Number(raw.substr(3), 10);
      Require(length && length <= size && length <= 32768,
              "BSD ar extended name has an invalid length.");
      member.name.resize(length);
      ReadExact(fd, member.name.data(), length, dataOffset);
      while (!member.name.empty() && member.name.back() == '\0')
        member.name.pop_back();
      member.offset += length;
      member.size -= length;
    } else if (!raw.empty() && raw.front() == '/') {
      result.gnu = true;
      member.name = GnuName(table, Number(raw.substr(1), 10));
    } else {
      if (!raw.empty() && raw.back() == '/') {
        result.gnu = true;
        member.name = raw.substr(0, raw.size() - 1);
      } else
        member.name = raw;
    }
    if (SymbolIndex(member.name)) {
      result.symbolIndex = true;
      continue;
    }
    Require(member.name.find('\0') == std::string::npos,
            "Unix ar entry has an invalid name.");
    const auto safe = SafeArchivePath(member.name.c_str());
    member.normalized = safe != member.name;
    member.name = safe;
    nameBytes += safe.size();
    Require(nameBytes <= kMaxTableSize,
            "Unix ar has too much member name data.");
    Require(
        !safe.empty() && names.insert(safe).second,
        "Unix ar has empty or duplicate member paths; nothing was extracted.");
    const auto mode = Number(header.substr(40, 8), 8, true);
    Require(mode <= 0177777 &&
                ((mode & S_IFMT) == 0 || (mode & S_IFMT) == S_IFREG),
            "Unix ar contains an unsupported member type.");
    member.mode = mode & 07777;
    member.modified = Number(header.substr(16, 12), 10, true);
    member.uid = Number(header.substr(28, 6), 10, true);
    member.gid = Number(header.substr(34, 6), 10, true);
    result.members.push_back(std::move(member));
  }
  for (const auto &member : result.members) {
    size_t slash = 0, depth = 0;
    while ((slash = member.name.find('/', slash)) != std::string::npos) {
      Require(++depth <= 64, "Unix ar exceeds the safe folder depth.");
      Require(!names.count(member.name.substr(0, slash++)),
              "Unix ar has conflicting file and folder paths.");
    }
  }
  std::sort(result.members.begin(), result.members.end(),
            [](const Member &a, const Member &b) { return a.name < b.name; });
  return result;
}

struct CachedIndex {
  std::string path;
  std::shared_ptr<const Index> index;
};
thread_local CachedIndex cached;

std::shared_ptr<const Index> OpenIndex(int fd, const std::string &path,
                                       const TransferControl &control) {
  control.Check();
  const bool scoped = ArchiveListingScopeActive();
  struct stat state;
  Require(!fstat(fd, &state), "Cannot inspect the opened Unix ar archive.");
  if (control.archiveVersion)
    CheckOpenedArchiveVersion(path, ArchivePasswordIdentity(state), control);
  if (scoped && cached.index && cached.path == path && SameState(state, cached.index->state)) {
    VerifySame(fd, path, state);
    return cached.index;
  }
  auto index = std::make_shared<const Index>(ReadIndex(fd, control));
  VerifySame(fd, path, index->state);
  if (scoped)
    cached = {path, index};
  return index;
}

bool Extension(const std::string &path, std::string_view ext) {
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos)
    return false;
  const auto suffix = std::string_view(path).substr(dot);
  return std::equal(
      suffix.begin(), suffix.end(), ext.begin(), ext.end(),
      [](unsigned char a, unsigned char b) { return std::tolower(a) == b; });
}

bool ObjectMember(int fd, const Member &member) {
  if (Extension(member.name, ".o") || Extension(member.name, ".obj"))
    return true;
  if (member.size < 4)
    return false;
  std::array<unsigned char, 18> bytes{};
  const auto count = std::min<uint64_t>(member.size, bytes.size());
  ReadExact(fd, bytes.data(), count, member.offset);
  const auto magic = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
                     (uint32_t(bytes[2]) << 8) | bytes[3];
  switch (magic) {
  case 0x7f454c46: // ELF ET_REL, not executables or shared libraries.
    return count >= 18 &&
           ((bytes[5] == 1 && bytes[16] == 1 && bytes[17] == 0) ||
            (bytes[5] == 2 && bytes[16] == 0 && bytes[17] == 1));
  case 0x4243c0de: // LLVM bitcode used for link-time optimization.
  case 0xdec0170b:
    return true;
  case 0xfeedface: // Mach-O MH_OBJECT.
  case 0xfeedfacf:
    return count >= 16 && bytes[12] == 0 && bytes[13] == 0 && bytes[14] == 0 &&
           bytes[15] == 1;
  case 0xcefaedfe:
  case 0xcffaedfe:
    return count >= 16 && bytes[12] == 1 && bytes[13] == 0 && bytes[14] == 0 &&
           bytes[15] == 0;
  default:
    return false;
  }
}

std::string Field(std::string value, size_t width) {
  Require(value.size() <= width, "A Unix ar header field does not fit.");
  value.resize(width, ' ');
  return value;
}

std::string Octal(unsigned value) {
  std::string result;
  do {
    result.insert(result.begin(), '0' + value % 8);
    value /= 8;
  } while (value);
  return result;
}

void Header(int fd, const std::string &name, uint64_t size, time_t modified,
            unsigned mode, bool table = false) {
  Write(fd, Field(name, 16) + Field(table ? "" : std::to_string(modified), 12) +
                Field(table ? "" : "0", 6) + Field(table ? "" : "0", 6) +
                Field(table ? "" : Octal(mode), 8) +
                Field(std::to_string(size), 10) + "`\n");
}

struct Source {
  std::string path, name, headerName, extraName;
  struct stat state {};
};
} // namespace

void ClearArReadCache() { cached = {}; }

bool ArRecognized(const std::string &path) {
  FD fd{open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
  struct stat state;
  return fd.value >= 0 && !fstat(fd.value, &state) && S_ISREG(state.st_mode) &&
         Detect(fd.value) != Signature::Other;
}

std::string ArOpen(const std::string &path, const TransferControl &control) {
  control.Check();
  FD fd{open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
  struct stat state;
  if (fd.value < 0 || fstat(fd.value, &state) || !S_ISREG(state.st_mode) ||
      Detect(fd.value) == Signature::Other)
    return "";
  CheckRegularAr(fd.value);
  return Location{"archive", "", "/", path}.URL();
}

std::vector<FileEntry> ArList(const Location &location,
                              const TransferControl &control) {
  FD fd{open(location.archive.c_str(),
             O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
  const auto index = OpenIndex(fd.value, location.archive, control);
  const auto prefix = location.path == "/" ? "" : location.path.substr(1) + "/";
  std::map<std::string, FileEntry> children;
  auto first =
      std::lower_bound(index->members.begin(), index->members.end(), prefix,
                       [](const Member &member, const std::string &name) {
                         return member.name < name;
                       });
  for (; first != index->members.end() &&
         !first->name.compare(0, prefix.size(), prefix);
       ++first) {
    control.Check();
    const auto &member = *first;
    const auto relative = member.name.substr(prefix.size());
    const auto slash = relative.find('/');
    const auto leaf = relative.substr(0, slash);
    if (children.count(leaf))
      continue;
    const bool directory = slash != std::string::npos;
    FileEntry entry;
    entry.name = leaf;
    entry.path = ChildLocation(location.URL(), leaf);
    entry.directory = directory;
    entry.permissions = directory ? 0755 : member.mode & 0777;
    entry.size = directory ? 0 : member.size;
    entry.modifiedKnown = !directory;
    entry.modified = directory ? 0 : member.modified;
    children.emplace(leaf, std::move(entry));
  }
  VerifySame(fd.value, location.archive, index->state);
  std::vector<FileEntry> entries;
  for (auto &entry : children)
    entries.push_back(std::move(entry.second));
  return entries;
}

void ArRead(const Location &location, const DataSink &sink,
            const TransferControl &control) {
  FD fd{open(location.archive.c_str(),
             O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
  const auto index = OpenIndex(fd.value, location.archive, control);
  const auto wanted = location.path.substr(1);
  const auto found =
      std::lower_bound(index->members.begin(), index->members.end(), wanted,
                       [](const Member &member, const std::string &name) {
                         return member.name < name;
                       });
  Require(found != index->members.end() && found->name == wanted,
          "Unix ar member no longer exists.");
  std::vector<char> buffer(65536);
  for (uint64_t at = 0; at < found->size;) {
    control.Check();
    const auto count = std::min<uint64_t>(buffer.size(), found->size - at);
    ReadExact(fd.value, buffer.data(), count, found->offset + at);
    sink(buffer.data(), count);
    control.Tick(count);
    at += count;
  }
  VerifySame(fd.value, location.archive, index->state);
}

std::string ArEditFormat(int fd, const std::string &path, const Hooks &hooks) {
  if (Detect(fd) == Signature::Other)
    return "";
  Require(
      !Extension(path, ".deb"),
      "Debian packages can be browsed and extracted, but not rebuilt as ordinary ar archives.");
  Require(
      !Extension(path, ".a") && !Extension(path, ".lib"),
      "Static libraries can be browsed and extracted, but Listi cannot rebuild their linker metadata.");
  const TransferControl control{hooks.proceed, {}};
  const auto index = ReadIndex(fd, control);
  Require(
      !index.symbolIndex,
      "Indexed ar libraries can be browsed and extracted, but Listi cannot rebuild their linker metadata.");
  Require(!(index.gnu && index.bsd),
          "Listi cannot preserve the layout of this mixed GNU/BSD ar archive.");
  for (const auto &member : index.members) {
    control.Check();
    Require(
        member.name != "debian-binary",
        "Debian packages can be browsed and extracted, but not rebuilt as ordinary ar archives.");
    Require(
        !ObjectMember(fd, member),
        "Ar archives with object files can be browsed and extracted, but Listi cannot rebuild library metadata.");
    Require(!member.normalized && member.name.find('/') == std::string::npos,
            "Listi can edit ordinary flat ar archives only.");
    Require(
        !member.uid && !member.gid && !(member.mode & 07000),
        "Listi cannot preserve this ar archive's member ownership or special permissions.");
  }
  VerifySame(fd, path, index.state);
  return index.bsd || (!index.gnu && !index.members.empty()) ? "ar-bsd" : "ar";
}

void ArCreate(int outputFd, const std::vector<std::string> &paths, bool bsd,
              const Hooks &hooks) {
  const TransferControl control{hooks.proceed, {}};
  control.Check();
  Require(paths.size() < kMaxMembers, "Unix ar has too many members.");
  std::vector<Source> sources;
  std::set<std::string> names;
  std::string table;
  for (const auto &path : paths) {
    control.Check();
    Source source;
    source.path = path;
    source.name = Leaf(path);
    const auto &name = source.name;
    Require(!name.empty() && SafeArchivePath(name.c_str()) == name &&
                name.size() <= 255 && name.find('/') == std::string::npos &&
                !SymbolIndex(name) && names.insert(name).second,
            "Unix ar requires valid, unique member names.");
    auto &state = source.state;
    Require(!lstat(path.c_str(), &state) && S_ISREG(state.st_mode) &&
                state.st_size >= 0,
            "Unix ar supports only regular files, not folders or links: " +
                path);
    Require(state.st_mtime >= 0 && uint64_t(state.st_mtime) <= 999999999999ULL,
            "Unix ar cannot store this file's modification time: " + path);
    Require(!(state.st_mode & 07000),
            "Unix ar cannot safely store special permissions: " + path);
    if (!EncodedName(name))
      source.headerName = bsd ? name : name + "/";
    else if (bsd) {
      source.extraName = name;
      // BSD extended names use NUL padding so the member starts aligned.
      source.extraName.resize((name.size() + 3) & ~size_t(3), '\0');
      source.headerName = "#1/" + std::to_string(source.extraName.size());
    } else {
      source.headerName = "/" + std::to_string(table.size());
      table += name + "/\n";
      Require(table.size() <= kMaxTableSize,
              "Unix ar name table is too large.");
    }
    Require(uint64_t(state.st_size) + source.extraName.size() <= kMaxSize,
            "Unix ar cannot store this file's size: " + path);
    sources.push_back(std::move(source));
  }

  Write(outputFd, kMagic);
  if (!table.empty()) {
    if (table.size() % 2)
      table += '\n';
    Header(outputFd, "//", table.size(), 0, 0, true);
    Write(outputFd, table);
  }
  std::vector<char> buffer(65536);
  for (const auto &source : sources) {
    control.Check();
    FD input{open(source.path.c_str(),
                  O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
    Require(input.value >= 0, "Cannot read Unix ar source: " + source.path);
    VerifySame(input.value, source.path, source.state);
    const auto size = uint64_t(source.state.st_size) + source.extraName.size();
    Header(outputFd, source.headerName, size, source.state.st_mtime,
           source.state.st_mode);
    Write(outputFd, source.extraName);
    for (off_t at = 0; at < source.state.st_size;) {
      control.Check();
      const auto count =
          std::min<uint64_t>(buffer.size(), source.state.st_size - at);
      ReadExact(input.value, buffer.data(), count, at);
      Write(outputFd, std::string_view(buffer.data(), count));
      if (hooks.progress)
        hooks.progress(source.path, count);
      at += count;
    }
    if (size % 2)
      Write(outputFd, "\n");
    VerifySame(input.value, source.path, source.state);
  }
  control.Check();
  for (const auto &source : sources) {
    struct stat named;
    Require(!lstat(source.path.c_str(), &named) &&
                SameState(source.state, named),
            "Source changed while creating Unix ar: " + source.path);
  }
}
} // namespace Listi
