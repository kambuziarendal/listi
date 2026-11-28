// SPDX-License-Identifier: MIT
#include "VirtualFile.h"
#include "UnavailableVolume.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>

namespace Listi {
namespace {
std::mutex stagesMutex;
std::map<std::string, std::pair<dev_t, ino_t>> activeStages;
}
void RegisterArchiveStage(const std::string &path, dev_t device, ino_t inode) {
  std::lock_guard<std::mutex> guard(stagesMutex);
  activeStages.emplace(path, std::make_pair(device, inode));
}
void ForgetArchiveStage(const std::string &path) {
  std::lock_guard<std::mutex> guard(stagesMutex);
  activeStages.erase(path);
}
bool IsListiPrivateEntry(const std::string &path) {
  if (path.find("/.Listi-archive-stage-") == std::string::npos)
    return false;
  std::lock_guard<std::mutex> guard(stagesMutex);
  for (const auto &[name, identity] : activeStages) {
    if (path != name && path.compare(0, name.size() + 1, name + "/") != 0)
      continue;
    struct stat st;
    if (!lstat(name.c_str(), &st) && st.st_dev == identity.first &&
        st.st_ino == identity.second)
      return true;
  }
  return false;
}
namespace {
std::mutex connectionsMutex;
std::map<std::string, const Connection> connections;
struct ConnectionAttempt {
  int64_t id;
  Connection auth;
};
std::map<std::string, ConnectionAttempt> pendingConnections;
int64_t nextConnectionAttempt = 0;
auto Credentials(const Connection &auth) {
  return std::tie(auth.username, auth.password, auth.domain, auth.privateKey,
                  auth.knownHosts, auth.keyPassphrase);
}
const Connection &BoundConnection(const Location &location) {
  std::lock_guard<std::mutex> guard(connectionsMutex);
  const auto it = connections.find(location.CanonicalAuthority());
  if (it == connections.end())
    throw std::runtime_error("No account is bound to this server. Use Network / Connect first.");
  // Entries are immutable and never erased during this process.
  return it->second;
}
void Fail(const std::string &p) {
  throw std::runtime_error(p + ": " + strerror(errno));
}
FileEntry Entry(const std::string &p, const struct stat &st) {
  FileEntry e;
  e.path = p;
  e.name = LocationLeaf(p);
  e.directory = S_ISDIR(st.st_mode);
  e.link = S_ISLNK(st.st_mode);
  e.size = st.st_size;
  e.SetModification(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
  e.permissions = st.st_mode & 0777;
  return e;
}
} // namespace
bool SetConnection(const std::string &url, const Connection &auth) {
  const auto location = ParseLocation(url);
  if (location.Local() || location.Archive())
    return false;
  std::lock_guard<std::mutex> guard(connectionsMutex);
  const auto [it, inserted] = connections.try_emplace(location.CanonicalAuthority(), auth);
  return inserted || Credentials(it->second) == Credentials(auth);
}
void RequireConnection(const std::string &url) {
  const auto location = ParseLocation(url);
  if (!location.Local() && !location.Archive())
    BoundConnection(location);
}
int64_t BeginConnectionAttempt(const std::string &url, const Connection &auth) {
  const auto location = ParseLocation(url);
  if (location.Local() || location.Archive())
    throw std::runtime_error("Not a network address.");
  const auto route = location.CanonicalAuthority();
  std::lock_guard<std::mutex> guard(connectionsMutex);
  const auto bound = connections.find(route);
  if (bound != connections.end() && Credentials(bound->second) != Credentials(auth))
    throw std::runtime_error("This server already has a different connection. Restart Listi to change accounts or credentials.");
  const auto id = ++nextConnectionAttempt;
  pendingConnections.insert_or_assign(route, ConnectionAttempt{id, auth});
  return id;
}
Connection GetConnection(const std::string &url, int64_t attempt) {
  const auto location = ParseLocation(url);
  if (!attempt)
    return BoundConnection(location);
  std::lock_guard<std::mutex> guard(connectionsMutex);
  const auto it = pendingConnections.find(location.CanonicalAuthority());
  if (it == pendingConnections.end() || it->second.id != attempt)
    throw std::runtime_error("Connection attempt was superseded.");
  return it->second.auth;
}
void FinishConnectionAttempt(const std::string &url, int64_t attempt, bool accepted) {
  const auto route = ParseLocation(url).CanonicalAuthority();
  std::lock_guard<std::mutex> guard(connectionsMutex);
  const auto it = pendingConnections.find(route);
  if (it == pendingConnections.end() || it->second.id != attempt) {
    if (accepted)
      throw std::runtime_error("Connection attempt was superseded.");
    return;
  }
  auto auth = std::move(it->second.auth);
  pendingConnections.erase(it);
  if (accepted) {
    const auto bound = connections.try_emplace(route, auth).first;
    if (Credentials(auth) != Credentials(bound->second))
      throw std::runtime_error("This server already has a different connection.");
  }
}
std::vector<FileEntry> ListFiles(const std::string &p,
                                 const TransferControl &c,
                                 std::vector<std::string> *omitted,
                                 ListingMethod *method) {
  if (method)
    *method = ListingMethod::Provider;
  c.Check();
  if (InterruptedVolumes().ContainsResolved(p))
    throw std::runtime_error(p + ": " + kUnavailableVolumeText);
  auto l = ParseLocation(p);
  if (l.Archive())
    return ListArchive(l, c);
  if (!l.Local())
    return ListNetwork(l, c, method);
  DIR *dir = opendir(p.c_str());
  if (!dir)
    Fail(p);
  std::vector<FileEntry> out;
  try {
    for (;;) {
      errno = 0;
      auto e = readdir(dir);
      if (!e) {
        if (errno)
          Fail(p);
        break;
      }
      c.Check();
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
        continue;
      auto path = ChildLocation(p, e->d_name);
      if (InterruptedVolumes().Contains(path)) {
        if (omitted)
          omitted->push_back(path);
        continue;
      }
      if (IsListiPrivateEntry(path))
        continue;
      struct stat st;
      if (lstat(path.c_str(), &st))
        Fail(path);
      out.push_back(Entry(path, st));
    }
  } catch (...) {
    closedir(dir);
    throw;
  }
  closedir(dir);
  return out;
}
bool StatFile(const std::string &p, FileEntry &e, const TransferControl &c,
              const FileEntry *discovered) {
  c.Check();
  auto l = ParseLocation(p);
  if (l.Archive()) {
    if (l.path == "/") {
      e = {};
      e.path = p;
      e.directory = true;
      return true;
    }
    return StatArchive(l, e, c);
  }
  if (!l.Local())
    return StatNetwork(l, e, c, discovered);
  struct stat st;
  if (lstat(p.c_str(), &st)) {
    if (errno == ENOENT)
      return false;
    Fail(p);
  }
  e = Entry(p, st);
  return true;
}
bool SameLocalRevision(const struct stat &a, const struct stat &b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
         a.st_mode == b.st_mode && a.st_size == b.st_size &&
         a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
         a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
         a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
         a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
namespace {
void ReadRegularLocalFile(const std::string &p, const DataSink &sink,
                          const TransferControl &c, const struct stat *expected,
                          size_t prefixBytes) {
  c.Check();
  // Opening a special file must not block a worker before cancellation can run.
  int fd = open(p.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0 && expected && (errno == EACCES || errno == EPERM))
    throw WitnessAccessDenied(p + ": permission denied");
  if (fd < 0)
    Fail(p);
  try {
    struct stat before;
    if (fstat(fd, &before))
      Fail(p);
    if (!S_ISREG(before.st_mode))
      throw std::runtime_error("Only regular files can be read: " + p);
    if (expected && !SameLocalRevision(*expected, before))
      throw std::runtime_error("Source changed before being read: " + p);
    uint64_t bytes = 0;
    std::vector<char> buf(131072);
    while (!expected || bytes < prefixBytes) {
      c.Check();
      const auto count = expected ? std::min<uint64_t>(buf.size(), prefixBytes - bytes) : buf.size();
      ssize_t n = read(fd, buf.data(), count);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        Fail(p);
      }
      if (!n)
        break;
      sink(buf.data(), n);
      bytes += n;
      c.Tick(n);
    }
    struct stat after, named;
    if (fstat(fd, &after) || lstat(p.c_str(), &named))
      Fail(p);
    if (!SameLocalRevision(before, after) || !SameLocalRevision(before, named) ||
        bytes != (expected ? prefixBytes : uint64_t(before.st_size)))
      throw std::runtime_error("Source changed while being read: " + p);
  } catch (...) {
    close(fd);
    throw;
  }
  close(fd);
}
}
void ReadLocalPrefix(const std::string &p, const struct stat &expected, size_t bytes,
                     const DataSink &sink, const TransferControl &c) {
  ReadRegularLocalFile(p, sink, c, &expected, bytes);
}
void ReadFile(const std::string &p, const DataSink &sink,
              const TransferControl &c) {
  c.Check();
  auto l = ParseLocation(p);
  if (l.Archive()) {
    ReadArchive(l, sink, c);
    return;
  }
  if (!l.Local()) {
    ReadNetwork(l, sink, c);
    return;
  }
  ReadRegularLocalFile(p, sink, c, nullptr, 0);
}
void UploadFile(const std::string &src, const std::string &dst,
                const TransferControl &c) {
  auto l = ParseLocation(dst);
  if (l.Local() || l.Archive())
    throw std::runtime_error("Upload target must be a network location");
  UploadNetwork(src, l, c);
}
void MakeFolder(const std::string &p, const TransferControl &c) {
  c.Check();
  auto l = ParseLocation(p);
  if (l.Archive())
    throw std::runtime_error(
        "Use the archive editing actions to change a writable archive");
  if (!l.Local()) {
    ChangeNetwork(l, "mkdir", "", c);
    return;
  }
  if (mkdir(p.c_str(), 0755))
    Fail(p);
}
void RemoveFile(const std::string &p, bool dir, const TransferControl &c) {
  c.Check();
  auto l = ParseLocation(p);
  if (l.Archive())
    throw std::runtime_error(
        "Use the archive editing actions to change a writable archive");
  if (!l.Local()) {
    ChangeNetwork(l, dir ? "rmdir" : "remove", "", c);
    return;
  }
  if (dir ? rmdir(p.c_str()) : unlink(p.c_str()))
    Fail(p);
}
void RenameFile(const std::string &a, const std::string &b,
                const TransferControl &c) {
  c.Check();
  auto l = ParseLocation(a), to = ParseLocation(b);
  if (l.Archive() || to.Archive())
    throw std::runtime_error(
        "Use the archive editing actions to change a writable archive");
  if (l.CanonicalAuthority() != to.CanonicalAuthority())
    throw std::runtime_error("Rename requires the same connection");
  if (!l.Local()) {
    ChangeNetwork(l, "rename", b, c);
    return;
  }
  if (rename(a.c_str(), b.c_str()))
    Fail(b);
}
} // namespace Listi
