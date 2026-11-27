// SPDX-License-Identifier: MIT
#include "ArchiveBackend.h"
#include "ArWriter.h"
#include "ArchiveVolumes.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <regex>
#include <string_view>
#include <map>
#include <memory>

namespace Listi {
std::string SafeArchivePath(const char *raw) {
  if (!raw)
    throw std::runtime_error("Archive entry has no name");
  const size_t length = strnlen(raw, 32769);
  if (length > 32768)
    throw std::runtime_error("Archive path is too long");
  const std::string_view name(raw, length);
  if (!name.empty() && (name[0] == '/' || name[0] == '\\'))
    throw std::runtime_error("Archive contains an unsafe absolute path");
  if (name.find('\\') != std::string::npos ||
      name.find_first_of("\r\n") != std::string::npos)
    throw std::runtime_error("Archive contains an unsupported path");
  std::string out;
  size_t at = 0;
  while (at < name.size()) {
    auto end = name.find('/', at);
    if (end == std::string::npos)
      end = name.size();
    auto part = name.substr(at, end - at);
    if (part == ".." || (out.empty() && part.size() > 1 && part[1] == ':'))
      throw std::runtime_error(
          "Archive contains an unsafe parent or drive path");
    if (!part.empty() && part != ".") {
      if (!out.empty())
        out += '/';
      out += part;
    }
    at = end + 1;
  }
  return out;
}

// Listi adapters are small routing code, not bundled compression engines.
// Additional system-backed adapters can be added here without changing panes
// or transactional extraction. Never fall back to launching an archive GUI.
namespace {
const ArchiveBackend backends[] = {{"libarchive", LibarchiveAvailable,
                                    LibarchiveOpen, LibarchiveList,
                                    LibarchiveRead}};
void RejectSplitArchive(const std::string &path) {
  static const std::regex split(R"((\.(zip|tar)\.[0-9]{3}|\.z[0-9]{2})$)",
                                std::regex::icase);
  if (std::regex_search(path, split))
    throw std::runtime_error("This split ZIP/tar format is not supported yet. "
                             "Listi has not extracted an incomplete volume.");
}
const ArchiveBackend &Backend() {
  for (const auto &backend : backends)
    if (backend.available())
      return backend;
  throw std::runtime_error(
      "Install libarchive from HaikuDepot, then try again. "
      "Archive browsing and extraction stay inside Listi.");
}
} // namespace
bool ArchiveExtension(const std::string &path) {
  auto dot = path.find_last_of('.');
  if (dot == std::string::npos)
    return false;
  std::string ext = path.substr(dot);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  for (const char *known :
       {".zip",  ".zipx", ".7z",   ".rar", ".tar", ".tgz",  ".tbz", ".tbz2",
        ".txz",  ".tlz",  ".tzst", ".gz",  ".bz2", ".xz",   ".zst", ".lz",
        ".lzma", ".lz4",  ".z",    ".lzh", ".lha", ".cpio", ".iso", ".cab",
        ".ar",   ".a",    ".deb",  ".rpm", ".xar", ".war",  ".jar", ".epub",
        ".cbz",  ".cbr",  ".arj",  ".ace", ".sit", ".sitx"})
    if (ext == known)
      return true;
  return false;
}
bool ArchiveAvailable() {
  if (SevenZipAvailable())
    return true;
  for (const auto &backend : backends)
    if (backend.available())
      return true;
  return false;
}
std::string OpenArchive(const std::string &path,
                        const TransferControl &control) {
  if (IsVirtual(path))
    return "";
  RejectSplitArchive(path);
  const auto parts = FindArchiveVolumes(path);
  if (parts.kind == ArchiveVolumes::Single) {
    auto root = ArOpen(path, control);
    if (!root.empty())
      return root;
  }
  if (parts.kind != ArchiveVolumes::Single && !SevenZipAvailable())
    throw std::runtime_error("Install the optional p7zip package from "
                             "HaikuDepot to read split archives inside Listi.");
  auto format = SevenZipFormat(parts.files.front().path);
  if (parts.kind != ArchiveVolumes::Single &&
      (format.empty() ||
       (parts.kind == ArchiveVolumes::Joined7z && format != "7z") ||
       (parts.kind == ArchiveVolumes::Rar && format != "rar" &&
        format != "rar5")))
    throw std::runtime_error(
        "The first archive part is damaged or does not match this set.");
  if (!format.empty() && SevenZipAvailable())
    return SevenZipOpen(path, control);
  if (!LibarchiveAvailable() && !ArchiveExtension(path))
    return "";
  return Backend().open(path, control);
}
namespace {
struct CachedListing {
  std::string fingerprint;
  uint64_t passwordGeneration;
  std::shared_ptr<const std::vector<FileEntry>> entries;
  size_t bytes;
};
thread_local unsigned listingScopes = 0;
thread_local std::map<std::string, CachedListing> listings;
thread_local size_t listingBytes = 0;
constexpr size_t ListingBudget = 8 * 1024 * 1024;
std::vector<FileEntry> ReadListing(const Location &location,
                                   const TransferControl &control) {
  RejectSplitArchive(location.archive);
  const auto parts = FindArchiveVolumes(location.archive);
  if (parts.kind != ArchiveVolumes::Single && !SevenZipAvailable())
    throw std::runtime_error("Install the optional p7zip package from "
                             "HaikuDepot to read split archives inside Listi.");
  auto format = SevenZipFormat(parts.files.front().path);
  if (parts.kind != ArchiveVolumes::Single &&
      (format.empty() ||
       (parts.kind == ArchiveVolumes::Joined7z && format != "7z") ||
       (parts.kind == ArchiveVolumes::Rar && format != "rar" &&
        format != "rar5")))
    throw std::runtime_error(
        "The first archive part is damaged or does not match this set.");
  if (!format.empty() && SevenZipAvailable())
    return SevenZipList(location, control);
  if (parts.kind == ArchiveVolumes::Single && ArRecognized(location.archive))
    return ArList(location, control);
  return Backend().list(location, control);
}
std::shared_ptr<const std::vector<FileEntry>>
Listing(const Location &location, const TransferControl &control,
        std::string *shown = nullptr) {
  control.Check();
  const auto fingerprint = FindArchiveVolumes(location.archive).Fingerprint();
  control.RequireArchiveVersion(fingerprint);
  if (shown) *shown = fingerprint;
  const auto passwordGeneration = ArchivePasswordGeneration();
  const auto key = location.URL();
  if (listingScopes) {
    auto found = listings.find(key);
    if (found != listings.end()) {
      if (found->second.fingerprint == fingerprint &&
          found->second.passwordGeneration == passwordGeneration)
        return found->second.entries;
      listingBytes -= found->second.bytes;
      listings.erase(found);
    }
  }
  auto entries = std::make_shared<const std::vector<FileEntry>>(
      ReadListing(location, control));
  control.Check();
  CheckArchivePasswordGeneration(passwordGeneration);
  if (FindArchiveVolumes(location.archive).Fingerprint() != fingerprint)
    throw std::runtime_error("Archive changed while listing. Open it again.");
  if (listingScopes) {
    size_t bytes = 0;
    for (const auto &entry : *entries)
      bytes += sizeof(FileEntry) + entry.name.size() + entry.path.size();
    if (bytes <= ListingBudget) {
      if (listings.size() >= 4 || bytes > ListingBudget - listingBytes) {
        listings.clear();
        listingBytes = 0;
      }
      listings.emplace(
          key, CachedListing{fingerprint, passwordGeneration, entries, bytes});
      listingBytes += bytes;
    }
  }
  return entries;
}
} // namespace
ArchiveListingScope::ArchiveListingScope() { ++listingScopes; }
bool ArchiveListingScopeActive() { return listingScopes != 0; }
ArchiveListingScope::~ArchiveListingScope() {
  if (!--listingScopes) {
    listings.clear();
    listingBytes = 0;
    ClearLibarchiveReadCache();
    ClearArReadCache();
  }
}
std::vector<FileEntry> ListArchive(const Location &location,
                                   const TransferControl &control) {
  return *Listing(location, control);
}
std::vector<FileEntry> ListArchiveView(const Location &location, const TransferControl &control,
                                       std::string &fingerprint) {
  return *Listing(location, control, &fingerprint);
}
bool StatArchive(const Location &location, FileEntry &entry,
                 const TransferControl &control) {
  auto parent = ParseLocation(ParentLocation(location.URL()));
  const auto entries = Listing(parent, control);
  const auto name = LocationLeaf(location.URL());
  const auto found =
      std::lower_bound(entries->begin(), entries->end(), name,
                       [](const FileEntry &entry, const std::string &name) {
                         return entry.name < name;
                       });
  if (found == entries->end() || found->name != name)
    return false;
  entry = *found;
  return true;
}
void ReadArchive(const Location &location, const DataSink &sink,
                 const TransferControl &control) {
  RejectSplitArchive(location.archive);
  const auto parts = FindArchiveVolumes(location.archive);
  if (control.archiveVersion)
    control.RequireArchiveVersion(parts.Fingerprint());
  if (parts.kind != ArchiveVolumes::Single && !SevenZipAvailable())
    throw std::runtime_error("Install the optional p7zip package from "
                             "HaikuDepot to read split archives inside Listi.");
  auto format = SevenZipFormat(parts.files.front().path);
  if (parts.kind != ArchiveVolumes::Single &&
      (format.empty() ||
       (parts.kind == ArchiveVolumes::Joined7z && format != "7z") ||
       (parts.kind == ArchiveVolumes::Rar && format != "rar" &&
        format != "rar5")))
    throw std::runtime_error(
        "The first archive part is damaged or does not match this set.");
  if (!format.empty() && SevenZipAvailable())
    SevenZipRead(location, sink, control);
  else if (parts.kind == ArchiveVolumes::Single && ArRecognized(location.archive))
    ArRead(location, sink, control);
  else
    Backend().read(location, sink, control);
}
} // namespace Listi
