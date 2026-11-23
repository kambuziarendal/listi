// SPDX-License-Identifier: MIT
#include "DynamicLibrary.h"
#include "SshPaths.h"
#include "VirtualFile.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <curl/curl.h>
#include <exception>
#include <fcntl.h>
#include <libsmbclient.h>
#include <memory>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace Listi {
namespace {
DynamicLibrary &SMB() {
  static DynamicLibrary lib({"libsmbclient.so", "libsmbclient.so.0"});
  return lib;
}
DynamicLibrary &Curl() {
  static DynamicLibrary lib({"libcurl.so", "libcurl.so.4"});
  return lib;
}
std::mutex smbMutex;
[[noreturn]] void Error(const std::string &s) {
  throw std::runtime_error(s + ": " + strerror(errno));
}
void Fill(FileEntry &e, const Location &l, const struct stat &st) {
  e.path = l.URL();
  e.name = LocationLeaf(e.path);
  e.directory = S_ISDIR(st.st_mode);
  e.link = S_ISLNK(st.st_mode);
  e.size = st.st_size;
  e.modified = st.st_mtime;
  e.modifiedKnown = true;
  e.permissions = st.st_mode & 0777;
}
struct SMBSession {
  std::unique_lock<std::mutex> lock{smbMutex};
  DynamicLibrary &lib = SMB();
  SMBCCTX *ctx = nullptr;
  Connection auth;
  explicit SMBSession(const Location &l) : auth(GetConnection(l.URL())) {
    if (!lib.Available())
      throw std::runtime_error(
          "Install samba4 from HaikuDepot to connect to SMB shares");
    ctx = LIBCALL(lib, smbc_new_context)();
    if (!ctx)
      Error("SMB context");
    try {
      LIBCALL(lib, smbc_setOptionUserData)(ctx, &auth);
      LIBCALL(lib, smbc_setFunctionAuthDataWithContext)(ctx, &Authenticate);
      LIBCALL(lib, smbc_setTimeout)(ctx, 10000);
      if (!LIBCALL(lib, smbc_setOptionProtocols)(ctx, "SMB2_02", "SMB3_11"))
        throw std::runtime_error("SMB2/3 protocol configuration failed");
      LIBCALL(lib, smbc_setOptionNoAutoAnonymousLogin)
      (ctx, auth.username.empty() ? 0 : 1);
      LIBCALL(lib, smbc_setDebug)(ctx, 0);
      if (l.port)
        LIBCALL(lib, smbc_setPort)(ctx, l.port);
      if (!LIBCALL(lib, smbc_init_context)(ctx))
        Error("SMB connection");
      LIBCALL(lib, smbc_setDebug)(ctx, 0);
    } catch (...) {
      LIBCALL(lib, smbc_free_context)(ctx, 1);
      ctx = nullptr;
      throw;
    }
  }
  ~SMBSession() {
    if (ctx)
      LIBCALL(lib, smbc_free_context)(ctx, 1);
  }
  static void Authenticate(SMBCCTX *c, const char *, const char *, char *domain,
                           int nd, char *user, int nu, char *pass, int np) {
    auto *a =
        static_cast<Connection *>(LIBCALL(SMB(), smbc_getOptionUserData)(c));
    if (nd > 0)
      snprintf(domain, nd, "%s", a->domain.c_str());
    if (nu > 0)
      snprintf(user, nu, "%s", a->username.c_str());
    if (np > 0)
      snprintf(pass, np, "%s", a->password.c_str());
  }
  std::string URL(Location l) {
    l.port = 0;
    return l.URL();
  }
  bool Stat(const Location &l, FileEntry &e) {
    struct stat st;
    if (LIBCALL(lib, smbc_getFunctionStat)(ctx)(ctx, URL(l).c_str(), &st)) {
      if (errno == ENOENT)
        return false;
      Error("SMB stat");
    }
    Fill(e, l, st);
    return true;
  }
};
struct SMBHandle {
  SMBSession &session;
  SMBCFILE *file;
  bool directory;
  ~SMBHandle() {
    if (file) {
      auto &s = session;
      if (directory)
        LIBCALL(s.lib, smbc_getFunctionClosedir)(s.ctx)(s.ctx, file);
      else
        LIBCALL(s.lib, smbc_getFunctionClose)(s.ctx)(s.ctx, file);
    }
  }
  void Close() {
    auto &s = session;
    int r = directory
                ? LIBCALL(s.lib, smbc_getFunctionClosedir)(s.ctx)(s.ctx, file)
                : LIBCALL(s.lib, smbc_getFunctionClose)(s.ctx)(s.ctx, file);
    file = nullptr;
    if (r)
      Error("SMB close");
  }
};
struct CurlSession {
  DynamicLibrary &lib = Curl();
  CURL *curl = nullptr;
  Connection auth;
  Location location;
  const TransferControl &control;
  DataSink sink;
  FILE *input = nullptr;
  std::exception_ptr failure;
  char error[CURL_ERROR_SIZE] = {};
  curl_slist *commands = nullptr;
  explicit CurlSession(const Location &l, const TransferControl &c)
      : auth(GetConnection(l.URL())), location(l), control(c) {
    if (!lib.Available())
      throw std::runtime_error("Install curl from HaikuDepot for FTP and SFTP");
    static std::once_flag init;
    std::call_once(init, [&] {
      if (LIBCALL(lib, curl_global_init)(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        throw std::runtime_error("curl initialization failed");
    });
    curl = LIBCALL(lib, curl_easy_init)();
    if (!curl)
      throw std::runtime_error("Could not allocate network connection");
    try {
      Set(CURLOPT_URL, l.URL().c_str());
      Set(CURLOPT_USERNAME, auth.username.c_str());
      Set(CURLOPT_PASSWORD, auth.password.c_str());
      Set(CURLOPT_ERRORBUFFER, error);
      Set(CURLOPT_NOSIGNAL, 1L);
      Set(CURLOPT_CONNECTTIMEOUT, 10L);
      Set(CURLOPT_LOW_SPEED_LIMIT, 1L);
      Set(CURLOPT_LOW_SPEED_TIME, 20L);
      Set(CURLOPT_FOLLOWLOCATION, 0L);
      Set(CURLOPT_PROTOCOLS_STR, "ftp,ftps,sftp");
      Set(CURLOPT_WRITEFUNCTION, &Write);
      Set(CURLOPT_WRITEDATA, this);
      Set(CURLOPT_XFERINFOFUNCTION, &Progress);
      Set(CURLOPT_XFERINFODATA, this);
      Set(CURLOPT_NOPROGRESS, 0L);
      if (l.scheme == "sftp") {
        if (auth.knownHosts.empty())
          auth.knownHosts = DefaultSshPath(SshFile::KnownHosts);
        Set(CURLOPT_SSH_KNOWNHOSTS, auth.knownHosts.c_str());
        if (!auth.privateKey.empty()) {
          Set(CURLOPT_SSH_PRIVATE_KEYFILE, auth.privateKey.c_str());
          if (!auth.keyPassphrase.empty())
            Set(CURLOPT_KEYPASSWD, auth.keyPassphrase.c_str());
        }
      }
    } catch (...) {
      LIBCALL(lib, curl_easy_cleanup)(curl);
      curl = nullptr;
      throw;
    }
  }
  ~CurlSession() {
    if (curl)
      LIBCALL(lib, curl_easy_cleanup)(curl);
    if (commands)
      LIBCALL(lib, curl_slist_free_all)(commands);
    if (input)
      fclose(input);
  }
  template <class T> void Set(CURLoption option, T value) {
    auto r = LIBCALL(lib, curl_easy_setopt)(curl, option, value);
    if (r != CURLE_OK)
      throw std::runtime_error(LIBCALL(lib, curl_easy_strerror)(r));
  }
  void Command(const std::string &s) {
    auto next = LIBCALL(lib, curl_slist_append)(commands, s.c_str());
    if (!next)
      throw std::bad_alloc();
    commands = next;
  }
  static size_t Write(char *p, size_t size, size_t count, void *data) {
    auto &s = *static_cast<CurlSession *>(data);
    size_t n = size * count;
    try {
      s.control.Check();
      if (s.sink)
        s.sink(p, n);
      return n;
    } catch (...) {
      s.failure = std::current_exception();
      return 0;
    }
  }
  static size_t Read(char *p, size_t size, size_t count, void *data) {
    auto &self = *static_cast<CurlSession *>(data);
    try {
      self.control.Check();
      size_t n = fread(p, size, count, self.input) * size;
      if (ferror(self.input))
        throw std::runtime_error("Could not read upload source");
      self.control.Tick(n);
      return n;
    } catch (...) {
      self.failure = std::current_exception();
      return CURL_READFUNC_ABORT;
    }
  }
  static int Progress(void *data, curl_off_t, curl_off_t, curl_off_t,
                      curl_off_t) {
    auto &s = *static_cast<CurlSession *>(data);
    try {
      s.control.Check();
      return 0;
    } catch (...) {
      s.failure = std::current_exception();
      return 1;
    }
  }
  CURLcode Perform(bool missingOK = false, bool accessDeniedOK = false) {
    control.Check();
    if (control.cleanup) {
      // NOSIGNAL with a synchronous DNS resolver cannot bound a lookup.
      const auto *info = LIBCALL(lib, curl_version_info)(CURLVERSION_NOW);
      if (!info || !(info->features & CURL_VERSION_ASYNCHDNS))
        throw std::runtime_error("Remote cleanup deferred: resolver has no deadline.");
      Set(CURLOPT_TIMEOUT_MS, control.cleanup->TimeoutMs());
    }
    auto r = LIBCALL(lib, curl_easy_perform)(curl);
    if (failure)
      std::rethrow_exception(failure);
    if (location.scheme == "sftp" && r == CURLE_PEER_FAILED_VERIFICATION)
      throw std::runtime_error(
          "SSH host key is unknown or changed. Verify this server with ssh in "
          "Terminal, or select a trusted known_hosts file in Connect / "
          "Advanced.");
    if (r != CURLE_OK && !(missingOK && r == CURLE_REMOTE_FILE_NOT_FOUND) &&
        !(accessDeniedOK && r == CURLE_REMOTE_ACCESS_DENIED))
      throw std::runtime_error(error[0] ? error
                                        : LIBCALL(lib, curl_easy_strerror)(r));
    return r;
  }
};
std::string QuoteSFTP(const std::string &s) {
  if (s.find_first_of("\r\n") != std::string::npos)
    throw std::runtime_error("Invalid remote path");
  std::string out = "\"";
  for (char c : s) {
    if (c == '\\' || c == '\"')
      out += '\\';
    out += c;
  }
  return out + '"';
}
// RFC 3659 uses UTC Gregorian dates, regardless of the client's time zone.
void ParseModify(const std::string &value, FileEntry &entry) {
  constexpr size_t dateLength = 14;
  if (value.size() < dateLength ||
      value.find_first_not_of("0123456789") < dateLength)
    return;

  std::string fraction;
  if (value.size() > dateLength) {
    if (value[dateLength] != '.' || value.size() == dateLength + 1 ||
        value.find_first_not_of("0123456789", dateLength + 1) !=
            std::string::npos)
      return;
    auto last = value.find_last_not_of('0');
    fraction = value.substr(dateLength + 1, last - dateLength);
  }

  auto number = [&](size_t start, size_t width) {
    return std::stoi(value.substr(start, width));
  };
  int year = number(0, 4);
  int month = number(4, 2);
  int day = number(6, 2);
  int hour = number(8, 2);
  int minute = number(10, 2);
  int second = number(12, 2);
  // POSIX time cannot represent leap seconds distinctly. Leave such a fact
  // unknown rather than silently assigning it to a different instant.
  if (year < 1000 || month < 1 || month > 12 || day < 1 || hour > 23 ||
      minute > 59 || second > 59)
    return;
  bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  int monthDays[] = {31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30,
                     31};
  if (day > monthDays[month - 1])
    return;
  auto daysBeforeYear = [](int year) -> int64_t {
    int previous = year - 1;
    return 365LL * previous + previous / 4 - previous / 100 + previous / 400;
  };
  int64_t days = daysBeforeYear(year) - daysBeforeYear(1970) + day - 1;
  for (int i = 0; i < month - 1; ++i)
    days += monthDays[i];
  entry.modified = ((days * 24 + hour) * 60 + minute) * 60 + second;
  entry.modifiedFraction = std::move(fraction);
  entry.modifiedKnown = true;
}
// MLSD preserves spaces in names. Unix LIST is a fallback for SFTP and older
// FTP.
FileEntry ParseList(const std::string &line, const Location &l, bool mlsd) {
  FileEntry e;
  bool hasPermissions = false;
  if (mlsd) {
    e.sizeKnown = false;
    auto at = line.find(' ');
    if (at == std::string::npos)
      throw std::runtime_error("Invalid FTP MLSD response");
    e.name = line.substr(at + 1);
    std::string facts = line.substr(0, at);
    size_t i = 0;
    bool sawModify = false;
    while (i < facts.size()) {
      auto end = facts.find(';', i);
      if (end == std::string::npos)
        end = facts.size();
      auto fact = facts.substr(i, end - i);
      auto eq = fact.find('=');
      auto key = fact.substr(0, eq),
           v = eq == std::string::npos ? "" : fact.substr(eq + 1);
      std::transform(key.begin(), key.end(), key.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (key == "type") {
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (v == "cdir" || v == "pdir")
          return {};
        e.directory = v == "dir";
        e.link = v.find("slink") != std::string::npos;
      } else if (key == "size") {
        if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos)
          throw std::runtime_error("Invalid FTP file size");
        e.size = std::stoll(v);
        e.sizeKnown = true;
      } else if (key == "modify") {
        // Duplicate facts are ambiguous, not a reason to trust the last one.
        if (!sawModify)
          ParseModify(v, e);
        else {
          e.modifiedKnown = false;
          e.modified = 0;
          e.modifiedFraction.clear();
        }
        sawModify = true;
      } else if (key == "unix.mode") {
        e.permissions = std::stoul(v, nullptr, 8) & 0777;
        hasPermissions = true;
      }
      i = end + 1;
    }
  } else {
    if (line.rfind("total ", 0) == 0 || line.empty())
      return {};
    std::istringstream in(line);
    std::string mode, links, owner, group, size, month, day, time;
    if (!(in >> mode >> links >> owner >> group >> size >> month >> day >>
          time) ||
        mode.size() < 10)
      throw std::runtime_error("Server listing format is not supported (MLSD "
                               "or Unix LIST required)");
    std::getline(in, e.name);
    if (!e.name.empty() && e.name[0] == ' ')
      e.name.erase(0, 1);
    e.directory = mode[0] == 'd';
    e.link = mode[0] == 'l';
    e.permissions = 0;
    for (int i = 0; i < 9; ++i)
      if (mode[i + 1] == "rwx"[i % 3] ||
          (i % 3 == 2 && (mode[i + 1] == 's' || mode[i + 1] == 't')))
        e.permissions |= 1u << (8 - i);
    e.size = std::stoll(size);
    if (e.link) {
      auto arrow = e.name.find(" -> ");
      if (arrow != std::string::npos)
        e.name.resize(arrow);
    } else if (mode[0] != '-' && mode[0] != 'd')
      throw std::runtime_error("Unsupported remote special file");
  }
  if (e.name.empty() || e.name == "." || e.name == "..")
    return {};
  // MLSD servers need not provide POSIX permissions. Keep unknown directories
  // traversable instead of applying the default regular-file mode (0644).
  if (mlsd && e.directory && !hasPermissions)
    e.permissions = 0755;
  e.path = ChildLocation(l.URL(), e.name);
  return e;
}
} // namespace
bool NetworkAvailable(const std::string &s) {
  return s == "smb" ? SMB().Available() : Curl().Available();
}
std::vector<FileEntry> ListNetwork(const Location &l,
                                   const TransferControl &c) {
  c.Check();
  std::vector<FileEntry> out;
  if (l.scheme == "smb") {
    SMBSession s(l);
    SMBHandle dir{
        s,
        LIBCALL(s.lib, smbc_getFunctionOpendir)(s.ctx)(s.ctx, s.URL(l).c_str()),
        true};
    if (!dir.file)
      Error("SMB open folder");
    for (;;) {
      c.Check();
      errno = 0;
      auto *de =
          LIBCALL(s.lib, smbc_getFunctionReaddir)(s.ctx)(s.ctx, dir.file);
      if (!de) {
        if (errno)
          Error("SMB list folder");
        break;
      }
      std::string name = de->name;
      if (name == "." || name == "..")
        continue;
      if (de->smbc_type != SMBC_FILE_SHARE && de->smbc_type != SMBC_DIR &&
          de->smbc_type != SMBC_FILE && de->smbc_type != SMBC_LINK)
        continue;
      FileEntry e;
      e.name = name;
      e.path = ChildLocation(l.URL(), name);
      e.directory =
          de->smbc_type == SMBC_DIR || de->smbc_type == SMBC_FILE_SHARE;
      e.link = de->smbc_type == SMBC_LINK;
      if (de->smbc_type != SMBC_FILE_SHARE && !s.Stat(ParseLocation(e.path), e))
        continue;
      out.push_back(e);
    }
    return out;
  }
  std::string listing;
  bool mlsd = l.scheme != "sftp";
  auto fetch = [&] {
    CurlSession s(l, c);
    auto url = l.URL();
    if (url.back() != '/')
      url += '/';
    s.Set(CURLOPT_URL, url.c_str());
    if (mlsd)
      s.Set(CURLOPT_CUSTOMREQUEST, "MLSD");
    s.sink = [&](const char *p, size_t n) {
      if (c.cleanup)
        c.cleanup->Consume(n);
      if (listing.size() + n > 64 * 1024 * 1024)
        throw std::runtime_error("Server listing is too large");
      listing.append(p, n);
    };
    s.Perform();
  };
  try {
    fetch();
  } catch (const Cancelled &) {
    throw;
  } catch (const std::exception &) {
    if (!mlsd)
      throw;
    mlsd = false;
    listing.clear();
    fetch();
  }
  std::istringstream lines(listing);
  std::string line;
  while (std::getline(lines, line)) {
    c.Check();
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;
    auto e = ParseList(line, l, mlsd);
    if (!e.name.empty())
      out.push_back(e);
  }
  return out;
}
bool StatNetwork(const Location &l, FileEntry &e, const TransferControl &c) {
  c.Check();
  if (l.scheme == "smb") {
    SMBSession s(l);
    return s.Stat(l, e);
  }
  if (l.path == "/") {
    e = {};
    e.path = l.URL();
    e.directory = true;
    return true;
  }
  for (auto &item : ListNetwork(ParseLocation(ParentLocation(l.URL())), c))
    if (item.name == LocationLeaf(l.URL())) {
      if (l.scheme == "sftp" && !item.directory && !item.link) {
        // Curl derives this from SFTP STAT; NOBODY avoids transferring the
        // file. It may still open it for reading, so a denied probe can
        // leave metadata unknown without invalidating the listing.
        CurlSession s(l, c);
        s.Set(CURLOPT_NOBODY, 1L);
        s.Set(CURLOPT_FILETIME, 1L);
        auto result = s.Perform(true, true);
        if (result == CURLE_REMOTE_FILE_NOT_FOUND)
          return false;
        curl_off_t modified = -1;
        // Some curl SSH backends expose an absent SFTP time as zero, like
        // an explicit epoch. Do not guess which applies.
        if (result == CURLE_OK &&
            LIBCALL(s.lib, curl_easy_getinfo)(s.curl, CURLINFO_FILETIME_T,
                                              &modified) == CURLE_OK &&
            modified > 0) {
          item.modified = modified;
          item.modifiedKnown = true;
        }
      }
      e = std::move(item);
      return true;
    }
  return false;
}
void ReadNetwork(const Location &l, const DataSink &sink,
                 const TransferControl &c) {
  if (l.scheme == "smb") {
    SMBSession s(l);
    SMBHandle f{s,
                LIBCALL(s.lib, smbc_getFunctionOpen)(s.ctx)(
                    s.ctx, s.URL(l).c_str(), O_RDONLY, 0),
                false};
    if (!f.file)
      Error("SMB open file");
    char buf[32768];
    for (;;) {
      c.Check();
      auto n = LIBCALL(s.lib, smbc_getFunctionRead)(s.ctx)(s.ctx, f.file, buf,
                                                           sizeof(buf));
      if (n < 0)
        Error("SMB read");
      if (!n)
        break;
      sink(buf, n);
      c.Tick(n);
    }
    f.Close();
    return;
  }
  CurlSession s(l, c);
  s.sink = [&](const char *p, size_t n) {
    sink(p, n);
    c.Tick(n);
  };
  s.Perform();
}
void UploadNetwork(const std::string &local, const Location &l,
                   const TransferControl &c) {
  int fd = open(local.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0)
    Error("Open upload source");
  FILE *input = fdopen(fd, "rb");
  if (!input) {
    close(fd);
    Error("Upload source");
  }
  std::unique_ptr<FILE, decltype(&fclose)> holder(input, &fclose);
  if (l.scheme == "smb") {
    SMBSession s(l);
    SMBHandle f{s,
                LIBCALL(s.lib, smbc_getFunctionOpen)(s.ctx)(
                    s.ctx, s.URL(l).c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600),
                false};
    if (!f.file)
      Error("SMB create staged file");
    char buf[32768];
    for (;;) {
      c.Check();
      size_t n = fread(buf, 1, sizeof(buf), input);
      if (!n) {
        if (ferror(input))
          Error("Read upload source");
        break;
      }
      size_t at = 0;
      while (at < n) {
        c.Check();
        auto wrote = LIBCALL(s.lib, smbc_getFunctionWrite)(s.ctx)(
            s.ctx, f.file, buf + at, n - at);
        if (wrote <= 0)
          Error("SMB write");
        at += wrote;
        c.Tick(wrote);
      }
    }
    f.Close();
    return;
  }
  CurlSession s(l, c);
  s.input = holder.release();
  struct stat st;
  if (fstat(fd, &st))
    Error("Upload source size");
  s.Set(CURLOPT_UPLOAD, 1L);
  s.Set(CURLOPT_READFUNCTION, &CurlSession::Read);
  s.Set(CURLOPT_READDATA, &s);
  s.Set(CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(st.st_size));
  s.Perform();
}
void ChangeNetwork(const Location &l, const char *action,
                   const std::string &destination, const TransferControl &c) {
  c.Check();
  std::string a = action;
  if (l.scheme == "smb") {
    SMBSession s(l);
    int r;
    if (a == "mkdir")
      r = LIBCALL(s.lib, smbc_getFunctionMkdir)(s.ctx)(s.ctx, s.URL(l).c_str(),
                                                       0755);
    else if (a == "rmdir")
      r = LIBCALL(s.lib, smbc_getFunctionRmdir)(s.ctx)(s.ctx, s.URL(l).c_str());
    else if (a == "remove")
      r = LIBCALL(s.lib, smbc_getFunctionUnlink)(s.ctx)(s.ctx,
                                                        s.URL(l).c_str());
    else if (a == "rename")
      r = LIBCALL(s.lib, smbc_getFunctionRename)(s.ctx)(
          s.ctx, s.URL(l).c_str(), s.ctx,
          s.URL(ParseLocation(destination)).c_str());
    else
      throw std::runtime_error("Unknown network operation");
    if (r)
      Error("SMB " + a);
    return;
  }
  CurlSession s(l, c);
  auto root = l;
  root.path = "/";
  s.Set(CURLOPT_URL, root.URL().c_str());
  s.Set(CURLOPT_NOBODY, 1L);
  if (l.scheme == "sftp") {
    std::string verb = a == "mkdir"    ? "mkdir"
                       : a == "rmdir"  ? "rmdir"
                       : a == "remove" ? "rm"
                                       : "rename";
    auto command = verb + " " + QuoteSFTP(l.path);
    if (a == "rename")
      command += " " + QuoteSFTP(ParseLocation(destination).path);
    s.Command(command);
  } else {
    // A single slash after the FTP authority in a curl URL denotes the
    // login directory, not the server's filesystem root. QUOTE commands
    // must address the same entry that listing and transfer showed.
    auto relativePath = [](const Location &location) {
      if (location.path.size() < 2 || location.path.front() != '/' ||
          location.path.find_first_of("\r\n") != std::string::npos)
        throw std::runtime_error("Invalid FTP entry path");
      return location.path.substr(1);
    };
    const auto path = relativePath(l);
    if (a == "rename") {
      s.Command("RNFR " + path);
      s.Command("RNTO " + relativePath(ParseLocation(destination)));
    } else
      s.Command((a == "mkdir"   ? "MKD "
                 : a == "rmdir" ? "RMD "
                                : "DELE ") +
                path);
  }
  s.Set(CURLOPT_QUOTE, s.commands);
  s.Perform();
}
} // namespace Listi
