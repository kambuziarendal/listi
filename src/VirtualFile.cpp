// SPDX-License-Identifier: MIT
#include "VirtualFile.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <sys/stat.h>
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
std::map<std::string, Connection> connections;
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
  e.modified = st.st_mtime;
  e.modifiedKnown = true;
  e.permissions = st.st_mode & 0777;
  return e;
}
} // namespace
void SetConnection(const std::string &url, const Connection &c) {
  auto l = ParseLocation(url);
  std::lock_guard<std::mutex> guard(connectionsMutex);
  connections[l.Authority()] = c;
}
Connection GetConnection(const std::string &url) {
  auto l = ParseLocation(url);
  std::lock_guard<std::mutex> guard(connectionsMutex);
  auto it = connections.find(l.Authority());
  return it == connections.end() ? Connection{} : it->second;
}
std::vector<FileEntry> ListFiles(const std::string &p,
                                 const TransferControl &c) {
  c.Check();
  auto l = ParseLocation(p);
  if (l.Archive())
    return ListArchive(l, c);
  if (!l.Local())
    return ListNetwork(l, c);
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
  // Opening a special file must not block a worker before cancellation can run.
  int fd = open(p.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    Fail(p);
  try {
    struct stat before;
    if (fstat(fd, &before))
      Fail(p);
    if (!S_ISREG(before.st_mode))
      throw std::runtime_error("Only regular files can be read: " + p);
    uint64_t bytes = 0;
    std::vector<char> buf(131072);
    for (;;) {
      c.Check();
      ssize_t n = read(fd, buf.data(), buf.size());
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
    if (before.st_dev != named.st_dev || before.st_ino != named.st_ino ||
        before.st_size != after.st_size || bytes != uint64_t(before.st_size) ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
      throw std::runtime_error("Source changed while being read: " + p);
  } catch (...) {
    close(fd);
    throw;
  }
  close(fd);
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
  if (l.Authority() != to.Authority())
    throw std::runtime_error("Rename requires the same connection");
  if (!l.Local()) {
    ChangeNetwork(l, "rename", b, c);
    return;
  }
  if (rename(a.c_str(), b.c_str()))
    Fail(b);
}
} // namespace Listi
