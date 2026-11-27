// SPDX-License-Identifier: MIT
#pragma once
#include "ArchivePasswords.h"
#include "FileOperation.h"
#include "VirtualFile.h"
#include <string_view>
namespace Listi {
// Bounded, worker-local metadata reuse during one archive job. Contents are
// still decoded from disk; fingerprints and password generations guard hits.
class ArchiveListingScope {
public:
  ArchiveListingScope();
  ~ArchiveListingScope();
  ArchiveListingScope(const ArchiveListingScope &) = delete;
  ArchiveListingScope &operator=(const ArchiveListingScope &) = delete;
};
bool ArchiveListingScopeActive();
class ArchiveMetadataBudget;
void ClearLibarchiveReadCache();
struct ArchiveBackend {
  const char *package;
  bool (*available)();
  std::string (*open)(const std::string &, const TransferControl &);
  std::vector<FileEntry> (*list)(const Location &, const TransferControl &);
  void (*read)(const Location &, const DataSink &, const TransferControl &);
};
bool HasArchiveDrivePrefix(std::string_view name);
std::string SafeArchivePath(const char *);
bool SevenZipAvailable();
std::string SevenZipFormat(const std::string &);
std::string SevenZipOpen(const std::string &, const TransferControl &);
std::vector<FileEntry> SevenZipList(const Location &, const TransferControl &);
std::vector<FileEntry> SevenZipList(const Location &, const TransferControl &,
                                    ArchiveMetadataBudget &);
void SevenZipRead(const Location &, const DataSink &, const TransferControl &);
bool SevenZipCanCreate();
void SevenZipCreate(int outputFd, const std::vector<std::string> &sources,
                    int level, bool encryptNames, ArchiveSecretPtr,
                    const Hooks &);
bool ArchiveExtension(const std::string &);
bool LibarchiveAvailable();
std::string LibarchiveOpen(const std::string &, const TransferControl &);
std::vector<FileEntry> LibarchiveList(const Location &,
                                      const TransferControl &);
std::vector<FileEntry> LibarchiveList(const Location &, const TransferControl &,
                                    ArchiveMetadataBudget &);
void LibarchiveRead(const Location &, const DataSink &,
                    const TransferControl &);
} // namespace Listi
