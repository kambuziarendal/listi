// SPDX-License-Identifier: MIT
#include "SftpFile.h"
#include "SftpSession.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <unistd.h>

namespace Listi {
namespace {
template<typename F>
auto WithSession(const Location &location, const TransferControl &control, F action) {
  control.Check();
  // A browser lookup gets a private session. For a job, keep a connection per
  // bound authority, never a cache of names or attributes.
  std::shared_ptr<SftpSession> temporary;
  const auto route = location.CanonicalAuthority();
  auto &slot = control.sftpSessions ? control.sftpSessions->connections[route] : temporary;
  try {
    if (!slot) {
      slot = std::make_shared<SftpSession>();
      slot->Connect(location, control);
      if (control.sftpSessions) {
        const auto bound = control.sftpSessions->boundPeers.find(route);
        if (bound != control.sftpSessions->boundPeers.end()) {
          const auto peer = slot->Peer();
          if (peer.endpoint != bound->second.endpoint || peer.hostKey != bound->second.hostKey)
            throw std::runtime_error("SFTP server changed during transfer. Refresh and try again.");
        }
      }
    }
    return action(*slot);
  } catch (...) {
    // A cancelled/incomplete protocol exchange must not be used for cleanup,
    // another root, or a fresh attempt. Never retry a mutation here.
    slot.reset();
    throw;
  }
}

bool Lookup(SftpSession &session, const std::string &path, LIBSSH2_SFTP_ATTRIBUTES &a,
            const TransferControl &control) {
  const auto r = session.Request(control, [&] {
    return libssh2_sftp_lstat(session.Channel(), path.c_str(), &a);
  });
  if (r == LIBSSH2_ERROR_SFTP_PROTOCOL) {
    const auto status = libssh2_sftp_last_error(session.Channel());
    if (status == LIBSSH2_FX_NO_SUCH_FILE || status == LIBSSH2_FX_NO_SUCH_PATH)
      return false;
  }
  session.Require(r, "Inspect SFTP entry");
  return true;
}

bool Describe(const LIBSSH2_SFTP_ATTRIBUTES &a, FileEntry &entry) {
  if (!(a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS))
    return false; // v3 has no separate type field; do not assume a regular file.
  entry.directory = LIBSSH2_SFTP_S_ISDIR(a.permissions);
  entry.link = LIBSSH2_SFTP_S_ISLNK(a.permissions);
  if (!entry.directory && !entry.link && !LIBSSH2_SFTP_S_ISREG(a.permissions))
    return false;
  entry.permissions = a.permissions & 0777;
  entry.sizeKnown = a.flags & LIBSSH2_SFTP_ATTR_SIZE;
  if (entry.sizeKnown) {
    if (a.filesize > uint64_t(std::numeric_limits<int64_t>::max()))
      throw std::runtime_error("SFTP entry size exceeds the supported range.");
    entry.size = static_cast<int64_t>(a.filesize);
  }
  if (a.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
    entry.SetModification(a.mtime, 0);
  return true;
}

// LSTAT can resolve a case alias. Operations authorizing a mutation need an
// exact name witness; ordinary metadata reads need no directory round trip.
template<typename F>
void VisitNames(SftpSession &session, const std::string &path,
                const TransferControl &control, F visit) {
  auto *dir = session.Open(path, 0, true, control);
  size_t listingBytes = 0;
  std::array<char, 32768> buffer;
  for (;;) {
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    const auto n = session.Request(control, [&] {
      return libssh2_sftp_readdir(dir, buffer.data(), buffer.size(), &attributes);
    });
    session.Require(n, "List SFTP folder");
    if (!n)
      break;
    listingBytes += size_t(n) + sizeof(FileEntry);
    if (listingBytes > 64 * 1024 * 1024)
      throw std::runtime_error("Server listing is too large");
    if (control.cleanup)
      control.cleanup->Consume(size_t(n) + sizeof(attributes));
    std::string name(buffer.data(), n);
    if (name != "." && name != ".." && !visit(name, attributes))
      break;
  }
  session.Close(control);
}

void RequireSpelling(SftpSession &session, const Location &location, const TransferControl &control) {
  if (location.path == "/")
    return;
  const auto url = location.URL();
  const auto leaf = LocationLeaf(url);
  bool found = false;
  VisitNames(session, ParseLocation(ParentLocation(url)).path, control,
      [&](const std::string &name, const LIBSSH2_SFTP_ATTRIBUTES &) {
        found = name == leaf;
        return !found;
      });
  if (!found)
    throw std::runtime_error("SFTP entry no longer has its selected name: " + url);
}
}

std::vector<FileEntry> ListSftp(const Location &l, const TransferControl &c) {
  return WithSession(l, c, [&](SftpSession &session) {
    std::vector<FileEntry> result;
    VisitNames(session, l.path, c, [&](const std::string &name, LIBSSH2_SFTP_ATTRIBUTES attributes) {
      FileEntry item;
      item.name = name;
      item.path = ChildLocation(l.URL(), name);
      if (!(attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
          !Lookup(session, ParseLocation(item.path).path, attributes, c))
        throw std::runtime_error("SFTP folder changed during listing.");
      if (!Describe(attributes, item)) {
        if (!c.listingWarning)
          throw std::runtime_error("Unsupported or unidentified SFTP entry: " + item.path);
        *c.listingWarning = "Incomplete listing: special or unidentified SFTP entries are not shown.";
      } else {
        result.push_back(std::move(item));
      }
      return true;
    });
    return result;
  });
}

// Claims need the literal names, including names of entries that cannot be
// transferred. Do not fetch or interpret unrelated siblings' attributes.
std::set<std::string> ListSftpNames(const Location &l, const TransferControl &c) {
  return WithSession(l, c, [&](SftpSession &session) {
    std::set<std::string> names;
    VisitNames(session, l.path, c, [&](const std::string &name, const LIBSSH2_SFTP_ATTRIBUTES &) {
      names.insert(name);
      return true;
    });
    return names;
  });
}

SftpPeer BindSftpPeer(const Location &location, const TransferControl &control) {
  if (!control.sftpSessions)
    throw std::logic_error("Cannot bind an SFTP peer outside a transfer session.");
  return WithSession(location, control, [&](SftpSession &session) {
    const auto peer = session.Peer();
    control.sftpSessions->boundPeers.emplace(location.CanonicalAuthority(), peer);
    return peer;
  });
}

std::string CanonicalSftpPath(const Location &l, const TransferControl &c) {
  return WithSession(l, c, [&](SftpSession &session) {
    std::array<char, 32768> path;
    const auto n = session.Request(c, [&] {
      return libssh2_sftp_realpath(session.Channel(), l.path.c_str(),
                                  path.data(), path.size());
    });
    session.Require(n, "Resolve SFTP entry");
    if (n <= 0 || size_t(n) >= path.size())
      throw std::runtime_error("Cannot establish canonical SFTP path: " + l.URL());
    return std::string(path.data(), n);
  });
}

bool StatSftp(const Location &l, FileEntry &entry, const TransferControl &c) {
  return WithSession(l, c, [&](SftpSession &session) {
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    if (!Lookup(session, l.path, attributes, c))
      return false;
    if (c.requireExactName)
      RequireSpelling(session, l, c);
    FileEntry current;
    current.path = l.URL();
    current.name = LocationLeaf(current.path);
    if (!Describe(attributes, current))
      throw std::runtime_error("Unsupported or unidentified SFTP entry: " + current.path);
    entry = std::move(current);
    return true;
  });
}

void ReadSftp(const Location &l, const DataSink &sink, const TransferControl &c) {
  WithSession(l, c, [&](SftpSession &session) {
    auto *input = session.Open(l.path, LIBSSH2_FXF_READ, false, c);
    std::array<char, 32768> buffer;
    for (;;) {
      const auto n = session.Request(c, [&] {
        return libssh2_sftp_read(input, buffer.data(), buffer.size());
      });
      session.Require(n, "Read SFTP file");
      if (!n)
        break;
      sink(buffer.data(), n);
      c.Tick(n);
    }
    session.Close(c);
  });
}

void UploadSftp(const std::string &local, const Location &l, const TransferControl &c) {
  const int fd = open(local.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0)
    throw std::runtime_error(std::string("Open upload source: ") + strerror(errno));
  struct Source { int fd; ~Source() { close(fd); } } source{fd};
  WithSession(l, c, [&](SftpSession &session) {
    auto *output = session.Open(l.path, LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL,
                                false, c);
    std::array<char, 32768> buffer;
    for (;;) {
      c.Check();
      const auto n = read(fd, buffer.data(), buffer.size());
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0)
        throw std::runtime_error(std::string("Read upload source: ") + strerror(errno));
      if (!n)
        break;
      for (ssize_t done = 0; done < n;) {
        const auto sent = session.Request(c, [&] {
          return libssh2_sftp_write(output, buffer.data() + done, n - done);
        });
        session.Require(sent, "Write SFTP file");
        if (!sent)
          throw std::runtime_error("SFTP write made no progress.");
        done += sent;
        c.Tick(sent);
      }
    }
    session.Close(c);
  });
}

void ChangeSftp(const Location &l, const std::string &action, const std::string &destination,
                const TransferControl &c) {
  WithSession(l, c, [&](SftpSession &session) {
    const auto target = action == "rename" ? ParseLocation(destination).path : std::string();
    if (action != "mkdir")
      RequireSpelling(session, l, c);
    auto *sftp = session.Channel();
    const char *path = l.path.c_str();
    const auto status = session.Request(c, [&] {
      if (c.mutationSubmitted)
        *c.mutationSubmitted = true;
      if (action == "rename")
        // Standard v3 rename, not the clobbering posix-rename extension.
        return libssh2_sftp_rename_ex(sftp, path, l.path.size(), target.c_str(), target.size(), 0);
      if (action == "mkdir")
        return libssh2_sftp_mkdir(sftp, path, 0755);
      if (action == "rmdir")
        return libssh2_sftp_rmdir(sftp, path);
      if (action == "remove")
        return libssh2_sftp_unlink(sftp, path);
      throw std::runtime_error("Unknown SFTP operation");
    });
    session.Require(status, action.c_str());
  });
}
}
