// SPDX-License-Identifier: MIT
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <vector>

namespace Listi {
struct Cancelled : std::exception {};
// Shared by all remote calls made to reconcile one failed upload. Never used
// to limit the original transfer or to authorize an unverified mutation.
struct NetworkCleanupBudget {
  using Clock = std::chrono::steady_clock;
  Clock::time_point deadline = Clock::now() + std::chrono::seconds(2);
  uint64_t remaining = 1024 * 1024;

  long TimeoutMs() const {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (ms <= 0 || !remaining)
      throw std::runtime_error("Remote cleanup budget exhausted.");
    return static_cast<long>(ms);
  }
  void Consume(size_t n) {
    if (n > remaining) {
      remaining = 0;
      throw std::runtime_error("Remote cleanup byte budget exceeded.");
    }
    remaining -= n;
  }
};
struct TransferControl {
  std::function<bool()> proceed;
  std::function<void(uint64_t)> progress;
  NetworkCleanupBudget *cleanup = nullptr;
  void Check() const {
    if (proceed && !proceed())
      throw Cancelled();
    if (cleanup)
      cleanup->TimeoutMs();
  }
  void Tick(uint64_t n) const {
    Check();
    if (progress)
      progress(n);
  }
};
struct Location {
  std::string scheme, host, path, archive;
  unsigned port = 0;
  bool Local() const { return scheme.empty(); }
  bool Archive() const { return scheme == "archive"; }
  std::string URL() const;
  std::string Authority() const;
};
struct FileEntry {
  std::string name, path;
  bool directory = false, link = false;
  bool sizeKnown = true;
  int64_t size = 0, modified = 0;
  unsigned permissions = 0644;
  bool modifiedKnown = false; // Distinguishes an explicit epoch from an absent timestamp.
};
struct Connection {
  std::string username, password, domain, privateKey, knownHosts, keyPassphrase;
};
Location ParseLocation(const std::string &);
bool IsVirtual(const std::string &);
void RegisterArchiveStage(const std::string &path, dev_t device, ino_t inode);
void ForgetArchiveStage(const std::string &path);
bool IsListiPrivateEntry(const std::string &path);
std::string EncodePath(const std::string &);
std::string DecodePath(const std::string &);
std::string ChildLocation(const std::string &, const std::string &);
std::string ParentLocation(const std::string &);
std::string LocationLeaf(const std::string &);
void SetConnection(const std::string &url, const Connection &);
Connection GetConnection(const std::string &url);
std::vector<FileEntry> ListFiles(const std::string &,
                                 const TransferControl & = {});
bool StatFile(const std::string &, FileEntry &, const TransferControl & = {});
using DataSink = std::function<void(const char *, size_t)>;
void ReadFile(const std::string &, const DataSink &,
              const TransferControl & = {});
void UploadFile(const std::string &local, const std::string &remote,
                const TransferControl &);
void MakeFolder(const std::string &, const TransferControl & = {});
void RemoveFile(const std::string &, bool directory,
                const TransferControl & = {});
void RenameFile(const std::string &, const std::string &,
                const TransferControl & = {});
// Returns empty for ordinary files. Inspection is performed only by workers.
std::string OpenArchive(const std::string &local, const TransferControl & = {});
std::vector<FileEntry> ListArchive(const Location &, const TransferControl &);
bool StatArchive(const Location &, FileEntry &, const TransferControl &);
void ReadArchive(const Location &, const DataSink &, const TransferControl &);
bool ArchiveAvailable();
bool NetworkAvailable(const std::string &scheme);
std::vector<FileEntry> ListNetwork(const Location &, const TransferControl &);
bool StatNetwork(const Location &, FileEntry &, const TransferControl &);
void ReadNetwork(const Location &, const DataSink &, const TransferControl &);
void UploadNetwork(const std::string &, const Location &,
                   const TransferControl &);
void ChangeNetwork(const Location &, const char *action, const std::string &,
                   const TransferControl &);
} // namespace Listi
