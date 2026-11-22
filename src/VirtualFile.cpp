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
std::string Normalize(const std::string &p) {
  std::vector<std::string> parts;
  size_t at = 0;
  while (at < p.size()) {
    size_t end = p.find('/', at);
    if (end == std::string::npos)
      end = p.size();
    auto s = p.substr(at, end - at);
    if (s == "..") {
      if (!parts.empty())
        parts.pop_back();
    } else if (!s.empty() && s != ".")
      parts.push_back(s);
    at = end + 1;
  }
  std::string result;
  for (auto &s : parts)
    result += "/" + s;
  return result.empty() ? "/" : result;
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
bool IsVirtual(const std::string &p) {
  return p.rfind("smb://", 0) == 0 || p.rfind("sftp://", 0) == 0 ||
         p.rfind("ftp://", 0) == 0 || p.rfind("ftps://", 0) == 0 ||
         p.rfind("archive://", 0) == 0;
}
std::string EncodePath(const std::string &s) {
  const char *hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9')) ||
        c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
      out += c;
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}
std::string DecodePath(const std::string &s) {
  std::string out;
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = s[i];
    if (c == '%') {
      if (i + 2 >= s.size() || digit(s[i + 1]) < 0 || digit(s[i + 2]) < 0)
        throw std::runtime_error("Invalid URL escape");
      c = (digit(s[i + 1]) << 4) | digit(s[i + 2]);
      i += 2;
    }
    if (c == 0 || c == '\r' || c == '\n')
      throw std::runtime_error(
          "Control characters are not supported in remote paths");
    out += c;
  }
  return out;
}
Location ParseLocation(const std::string &s) {
  Location l;
  auto split = s.find("://");
  if (s.empty() || s[0] == '/' || split == std::string::npos) {
    l.path = s;
    return l;
  }
  l.scheme = s.substr(0, split);
  if (!IsVirtual(s))
    throw std::runtime_error("Unsupported location protocol");
  auto rest = s.substr(split + 3);
  if (l.Archive()) {
    auto boundary = rest.find("!/");
    if (boundary == std::string::npos)
      throw std::runtime_error("Invalid archive location");
    l.archive = DecodePath(rest.substr(0, boundary));
    if (l.archive.empty() || l.archive[0] != '/')
      throw std::runtime_error("Archive path must be absolute");
    l.path = Normalize(DecodePath(rest.substr(boundary + 1)));
    return l;
  }
  auto slash = rest.find('/');
  l.host = rest.substr(0, slash);
  if (l.host.empty() || l.host.find_first_of("@?# \\\r\n") != std::string::npos)
    throw std::runtime_error("Use a server name without credentials; connect "
                             "using the Network dialog");
  auto colon = l.host.rfind(':');
  if (colon != std::string::npos &&
      (l.host[0] != '[' || colon > l.host.find(']'))) {
    std::string port = l.host.substr(colon + 1);
    if (port.empty() || port.size() > 5 ||
        port.find_first_not_of("0123456789") != std::string::npos)
      throw std::runtime_error("Invalid server port");
    l.port = std::stoul(port);
    if (!l.port || l.port > 65535)
      throw std::runtime_error("Invalid server port");
    l.host.resize(colon);
  }
  std::transform(l.host.begin(), l.host.end(), l.host.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  l.path = Normalize(
      slash == std::string::npos ? "/" : DecodePath(rest.substr(slash)));
  return l;
}
std::string Location::Authority() const {
  return scheme + "://" + host + (port ? ":" + std::to_string(port) : "");
}
std::string Location::URL() const {
  if (Local())
    return path;
  if (Archive())
    return "archive://" + EncodePath(archive) + "!" + EncodePath(path);
  return Authority() + EncodePath(path);
}
std::string ChildLocation(const std::string &p, const std::string &name) {
  if (name.empty() || name == "." || name == ".." ||
      name.find('/') != std::string::npos ||
      name.find('\0') != std::string::npos)
    throw std::runtime_error("Invalid file name");
  auto l = ParseLocation(p);
  l.path += (l.path.empty() || l.path.back() == '/' ? "" : "/") + name;
  return l.URL();
}
std::string ParentLocation(const std::string &p) {
  auto l = ParseLocation(p);
  if (l.Archive() && l.path == "/")
    return ParentLocation(l.archive);
  if (l.path.size() > 1 && l.path.back() == '/')
    l.path.pop_back();
  auto at = l.path.find_last_of('/');
  l.path = at == 0 || at == std::string::npos ? "/" : l.path.substr(0, at);
  return l.URL();
}
std::string LocationLeaf(const std::string &p) {
  auto l = ParseLocation(p);
  if (l.path.size() > 1 && l.path.back() == '/')
    l.path.pop_back();
  auto at = l.path.find_last_of('/');
  return at == std::string::npos ? l.path : l.path.substr(at + 1);
}
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
bool StatFile(const std::string &p, FileEntry &e, const TransferControl &c) {
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
    return StatNetwork(l, e, c);
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
