// SPDX-License-Identifier: MIT
#pragma once
#include "FileEntry.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
class SftpSession;
// Only the worker for this job can use its authenticated sessions. The account
// bound to a canonical authority cannot change during the process.
using SftpSessions = std::map<std::string, std::shared_ptr<SftpSession>>;
struct TransferControl {
  std::function<bool()> proceed;
  std::function<void(uint64_t)> progress;
  NetworkCleanupBudget *cleanup = nullptr;
  // Bound by an archive member transfer; listings and reads must not adopt a new revision.
  const std::string *archiveVersion = nullptr;
  // Set immediately before handing a mutation to the transport library.
  bool *mutationSubmitted = nullptr;
  SftpSessions *sftpSessions = nullptr;
  // Browsers may omit unsupported entries with a visible warning. Workers cannot.
  std::string *listingWarning = nullptr;
  bool requireExactName = false; // SFTP mutation guards also need exact spelling.
  void RequireArchiveVersion(const std::string &actual) const {
    if (archiveVersion && *archiveVersion != actual)
      throw std::runtime_error("Archive changed after selection. Reload and select again.");
  }
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
  std::string CanonicalAuthority() const;
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
// Navigation treats components as literal names and stays inside the URL root.
std::string ResolveVirtualRelativePath(const std::string &base,
                                       const std::string &relative);
std::string ParentLocation(const std::string &);
std::string LocationLeaf(const std::string &);
// Interim single-account mode: a normalized route can be registered only
// once per process. Existing panes and queued work never adopt new credentials.
bool SetConnection(const std::string &url, const Connection &);
void RequireConnection(const std::string &url);
Connection GetConnection(const std::string &url);
// FTP servers may omit names from both MLSD and LIST.
enum class ListingMethod { Provider, FtpMlsd, FtpList };
inline bool VisibleEntriesOnly(ListingMethod method) {
  return method == ListingMethod::FtpMlsd || method == ListingMethod::FtpList;
}
// Browsers hide interrupted volumes. Integrity-sensitive callers also collect
// the exact paths that were omitted without opening them.
std::vector<FileEntry> ListFiles(const std::string &,
                                 const TransferControl & = {},
                                 std::vector<std::string> *omitted = nullptr,
                                 ListingMethod *method = nullptr);
// False means the provider reports absence; a directory listing alone does
// not establish absence on FTP. Ambiguous replies throw.
// Only a copy's initial FTP lookup may reuse an entry from its traversal.
// Leave discovered unset when checking the current revision.
bool StatFile(const std::string &, FileEntry &, const TransferControl & = {},
              const FileEntry *discovered = nullptr);
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
// Returns the fingerprint checked by the very listing which supplied these rows.
std::vector<FileEntry> ListArchiveView(const Location &, const TransferControl &, std::string &fingerprint);
bool StatArchive(const Location &, FileEntry &, const TransferControl &);
void ReadArchive(const Location &, const DataSink &, const TransferControl &);
bool ArchiveAvailable();
bool NetworkAvailable(const std::string &scheme);
std::vector<FileEntry> ListNetwork(const Location &, const TransferControl &,
                                   ListingMethod *method = nullptr);
bool StatNetwork(const Location &, FileEntry &, const TransferControl &,
                 const FileEntry *discovered = nullptr);
bool RecheckFtpListingEntry(const Location &, FileEntry &, const TransferControl &);
void ReadNetwork(const Location &, const DataSink &, const TransferControl &);
void UploadNetwork(const std::string &, const Location &,
                   const TransferControl &);
void ChangeNetwork(const Location &, const char *action, const std::string &,
                   const TransferControl &);
} // namespace Listi
