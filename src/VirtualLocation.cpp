// SPDX-License-Identifier: MIT
#include "VirtualFile.h"
#include <algorithm>
#include <cctype>

namespace Listi {
namespace {
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
namespace {
void RequirePathByte(unsigned char c, bool localArchivePath) {
  if (c == 0)
    throw std::runtime_error("NUL is not supported in paths");
  if (!localArchivePath && (c == '\r' || c == '\n'))
    throw std::runtime_error("Line breaks are not supported in virtual paths");
}

std::string DecodeURLPath(const std::string &s, bool localArchivePath) {
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
    RequirePathByte(c, localArchivePath);
    out += c;
  }
  return out;
}
} // namespace

std::string DecodePath(const std::string &s) { return DecodeURLPath(s, false); }

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
    l.archive = DecodeURLPath(rest.substr(0, boundary), true);
    if (l.archive.empty() || l.archive[0] != '/')
      throw std::runtime_error("Archive path must be absolute");
    l.path = Normalize(DecodePath(rest.substr(boundary + 1)));
    return l;
  }
  auto slash = rest.find('/');
  l.host = rest.substr(0, slash);
  if (l.host.empty() || l.host.find('\0') != std::string::npos ||
      l.host.find_first_of("@?# \\\r\n") != std::string::npos)
    throw std::runtime_error("Use a server name without credentials; connect "
                             "using the Network dialog");
  auto colon = l.host.rfind(':');
  if (colon != std::string::npos &&
      (l.host[0] != '[' || colon > l.host.find(']'))) {
    if (colon == 0)
      throw std::runtime_error("Missing server name");
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
std::string Location::CanonicalAuthority() const {
  const unsigned defaultPort = scheme == "sftp" ? 22 : scheme == "ftp" ? 21 :
                               scheme == "ftps" ? 990 : scheme == "smb" ? 445 : 0;
  return port == defaultPort ? scheme + "://" + host : Authority();
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
std::string ResolveVirtualRelativePath(const std::string &base,
                                       const std::string &relative) {
  auto location = ParseLocation(base);
  if (location.Local() || relative.empty() || relative.front() == '/')
    throw std::runtime_error("Expected a relative virtual path");
  for (unsigned char c : relative)
    RequirePathByte(c, false);
  location.path = Normalize(location.path + "/" + relative);
  return location.URL();
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
} // namespace Listi
