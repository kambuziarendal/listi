// SPDX-License-Identifier: MIT
#include "DynamicLibrary.h"
#include "CurlLibrary.h"
#include "SftpFile.h"
#include "VirtualFile.h"
#include <algorithm>
#include <cerrno>
#include <condition_variable>
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
// Contexts own their handles, but Samba also has process-wide state. Serialize
// API calls in arrival order so a streaming reader cannot starve browsing.
class SMBPermit {
public:
  explicit SMBPermit(const TransferControl *control = nullptr) {
    if (control)
      control->Check();
    std::unique_lock<std::mutex> lock(mutex);
    previous = last;
    if (last)
      last->next = this;
    else
      first = this;
    last = this;
    try {
      while (active || first != this) {
        if (control) {
          ready.wait_for(lock, std::chrono::milliseconds(10));
          lock.unlock();
          control->Check();
          lock.lock();
        } else {
          ready.wait(lock);
        }
      }
    } catch (...) {
      if (!lock.owns_lock())
        lock.lock();
      Unlink();
      ready.notify_all();
      throw;
    }
    Unlink();
    active = true;
  }
  SMBPermit(const SMBPermit &) = delete;
  SMBPermit &operator=(const SMBPermit &) = delete;
  ~SMBPermit() {
    std::lock_guard<std::mutex> lock(mutex);
    active = false;
    ready.notify_all();
  }

private:
  // The waiters own their queue nodes, so closing a native handle does not
  // allocate even when it needs to wait for another context.
  void Unlink() {
    if (previous)
      previous->next = next;
    else
      first = next;
    if (next)
      next->previous = previous;
    else
      last = previous;
  }
  SMBPermit *previous = nullptr, *next = nullptr;
  static inline SMBPermit *first = nullptr, *last = nullptr;
  static inline std::mutex mutex;
  static inline std::condition_variable ready;
  static inline bool active = false;
};
[[noreturn]] void Error(const std::string &s) {
  throw std::runtime_error(s + ": " + strerror(errno));
}
void Fill(FileEntry &e, const Location &l, const struct stat &st) {
  e.path = l.URL();
  e.name = LocationLeaf(e.path);
  e.directory = S_ISDIR(st.st_mode);
  e.link = S_ISLNK(st.st_mode);
  if (st.st_ino)
    e.providerIdentity = std::to_string(st.st_ino);
  e.size = st.st_size;
  e.SetModification(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
  e.permissions = st.st_mode & 0777;
}
struct SMBSession {
  DynamicLibrary &lib = SMB();
  SMBCCTX *ctx = nullptr;
  Connection auth;
  const TransferControl &control;
  explicit SMBSession(const Location &l, const TransferControl &c)
      : auth(GetConnection(l.URL(), c.connectionAttempt)), control(c) {
    if (!lib.Available())
      throw std::runtime_error(
          "Install samba4 from HaikuDepot to connect to SMB shares");
    SMBPermit permit(&control);
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
  SMBSession(const SMBSession &) = delete;
  SMBSession &operator=(const SMBSession &) = delete;
  ~SMBSession() {
    if (ctx) {
      SMBPermit permit;
      LIBCALL(lib, smbc_free_context)(ctx, 1);
    }
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
    SMBPermit permit(&control);
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
  SMBCFILE *file = nullptr;
  bool directory;
  SMBHandle(SMBSession &s, const Location &l, bool dir, int flags = 0)
      : session(s), directory(dir) {
    SMBPermit permit(&s.control);
    auto url = s.URL(l);
    if (dir)
      file = LIBCALL(s.lib, smbc_getFunctionOpendir)(s.ctx)(s.ctx, url.c_str());
    else
      file = LIBCALL(s.lib, smbc_getFunctionOpen)(s.ctx)(s.ctx, url.c_str(),
                                                       flags, 0600);
    if (!file)
      Error(dir ? "SMB open folder" : flags == O_RDONLY ? "SMB open file"
                                                       : "SMB create staged file");
  }
  SMBHandle(const SMBHandle &) = delete;
  SMBHandle &operator=(const SMBHandle &) = delete;
  ~SMBHandle() {
    if (file) {
      SMBPermit permit;
      CloseLocked();
    }
  }
  int CloseLocked() {
    auto &s = session;
    int r = directory
                ? LIBCALL(s.lib, smbc_getFunctionClosedir)(s.ctx)(s.ctx, file)
                : LIBCALL(s.lib, smbc_getFunctionClose)(s.ctx)(s.ctx, file);
    file = nullptr;
    return r;
  }
  void Close() {
    SMBPermit permit(&session.control);
    if (CloseLocked())
      Error("SMB close");
  }
};
struct CurlSession {
  DynamicLibrary &lib = Curl();
  CURL *curl = nullptr;
  Connection auth;
  const TransferControl &control;
  DataSink sink;
  FILE *input = nullptr;
  std::exception_ptr failure;
  char error[CURL_ERROR_SIZE] = {};
  curl_slist *commands = nullptr;
  explicit CurlSession(const Location &l, const TransferControl &c)
      : auth(GetConnection(l.URL(), c.connectionAttempt)), control(c) {
    curl = NewCurlHandle();
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
      Set(CURLOPT_PROTOCOLS_STR, "ftp,ftps");
      Set(CURLOPT_WRITEFUNCTION, &Write);
      Set(CURLOPT_WRITEDATA, this);
      Set(CURLOPT_XFERINFOFUNCTION, &Progress);
      Set(CURLOPT_XFERINFODATA, this);
      Set(CURLOPT_NOPROGRESS, 0L);
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
  CURLcode Perform(bool missingOK = false, bool accessDeniedOK = false, bool quoteErrorOK = false, bool mutation = false) {
    control.Check();
    if (control.cleanup) {
      // NOSIGNAL with a synchronous DNS resolver cannot bound a lookup.
      const auto *info = LIBCALL(lib, curl_version_info)(CURLVERSION_NOW);
      if (!info || !(info->features & CURL_VERSION_ASYNCHDNS))
        throw std::runtime_error("Remote cleanup deferred: resolver has no deadline.");
      Set(CURLOPT_TIMEOUT_MS, control.cleanup->TimeoutMs());
    }
    if (mutation && control.mutationSubmitted)
      *control.mutationSubmitted = true;
    auto r = LIBCALL(lib, curl_easy_perform)(curl);
    if (failure)
      std::rethrow_exception(failure);
    if (r != CURLE_OK && !(missingOK && r == CURLE_REMOTE_FILE_NOT_FOUND) &&
        !(accessDeniedOK && r == CURLE_REMOTE_ACCESS_DENIED) &&
        !(quoteErrorOK && r == CURLE_QUOTE_ERROR))
      throw std::runtime_error(error[0] ? error
                                        : LIBCALL(lib, curl_easy_strerror)(r));
    return r;
  }
};
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
struct UnverifiedFtpType : std::runtime_error {
  explicit UnverifiedFtpType(const std::string &name)
      : std::runtime_error("Unverified FTP entry type: " + name), name(name) {}
  std::string name;
};
FileEntry ParseList(const std::string &line, const Location &l, bool mlsd) {
  FileEntry e;
  e.permissionsKnown = !mlsd;
  if (mlsd) {
    e.sizeKnown = false;
    auto at = line.find(' ');
    if (at == std::string::npos)
      throw std::runtime_error("Invalid FTP MLSD response");
    e.name = line.substr(at + 1);
    std::string facts = line.substr(0, at);
    size_t i = 0;
    bool sawModify = false, sawType = false, validType = false;
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
        if (sawType)
          throw UnverifiedFtpType(e.name);
        sawType = true;
        if (v == "cdir" || v == "pdir")
          return {};
        e.directory = v == "dir";
        e.link = v == "os.unix=slink" || v.rfind("os.unix=slink:", 0) == 0 ||
                 v == "os.unix=symlink" || v.rfind("os.unix=symlink:", 0) == 0;
        validType = e.directory || e.link || v == "file";
      } else if (key == "unique") {
        e.providerIdentity = v;
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
        if (v.empty() || v.find_first_not_of("01234567") != std::string::npos)
          throw std::runtime_error("Invalid FTP file permissions");
        e.permissions = std::stoul(v, nullptr, 8) & 0777;
        e.permissionsKnown = true;
      }
      i = end + 1;
    }
    if (!validType)
      throw UnverifiedFtpType(e.name);
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
  if (!e.permissionsKnown)
    e.permissions = e.directory ? 0700 : 0600;
  e.path = ChildLocation(l.URL(), e.name);
  return e;
}
// MLSD is a listing, not an exact lookup. MLST asks the server to
// resolve the requested spelling, including on case-insensitive volumes.
enum class FtpLookup { Found, Missing, Unsupported };
FtpLookup LookupFtpName(const Location &l, FileEntry &entry, const TransferControl &control) {
  auto root = l;
  root.path = "/";
  CurlSession probe(root, control);
  std::string response;
  probe.sink = [&](const char *data, size_t size) {
    if (control.cleanup)
      control.cleanup->Consume(size);
    if (size > 65536 - response.size())
      throw std::runtime_error("FTP name lookup response is too large.");
    response.append(data, size);
  };
  probe.Set(CURLOPT_HEADERFUNCTION, &CurlSession::Write);
  probe.Set(CURLOPT_HEADERDATA, &probe);
  probe.Set(CURLOPT_NOBODY, 1L);
  probe.Command("MLST " + l.path.substr(1)); // Relative to the login directory.
  probe.Set(CURLOPT_QUOTE, probe.commands);
  const auto status = probe.Perform(false, false, true);
  std::istringstream headers(response);
  std::string line, facts, last;
  while (std::getline(headers, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (!line.empty())
      last = line;
    if (line.size() > 1 && line.front() == ' ') {
      const auto split = line.find(' ', 1);
      if (split != std::string::npos && line[split - 1] == ';')
        facts = line.substr(1, split - 1);
    }
  }
  if (status == CURLE_OK && !facts.empty()) {
    // Use the requested leaf, not the server's potentially absolute pathname.
    entry = ParseList(facts + " " + LocationLeaf(l.URL()), ParseLocation(ParentLocation(l.URL())), true);
    if (!entry.name.empty())
      return FtpLookup::Found;
  }
  // RFC 3659 also uses 550 for denied access; never treat the code
  // alone, unrecognized/localized text, or unsupported MLST as absence.
  if (status == CURLE_QUOTE_ERROR) {
    if (last == "550 No such file or directory.")
      return FtpLookup::Missing;
    if (last.compare(0, 4, "500 ") == 0 || last.compare(0, 4, "502 ") == 0 ||
        last.compare(0, 4, "504 ") == 0)
      return FtpLookup::Unsupported;
  }
  throw std::runtime_error("FTP server did not confirm the state of this name: " + l.URL());
}
} // namespace
bool RecheckFtpListingEntry(const Location &location, FileEntry &current,
                            const TransferControl &control) {
  const auto result = LookupFtpName(location, current, control);
  if (result != FtpLookup::Unsupported)
    return result == FtpLookup::Found;
  // A server can implement MLSD but not MLST. Re-read its directory rather
  // than certifying the old entry or treating command rejection as absence.
  return StatNetwork(location, current, control);
}

bool NetworkAvailable(const std::string &s) {
  // libssh2 is a linked package dependency; SFTP additionally needs curl for TCP.
  return s == "smb" ? SMB().Available() : Curl().Available();
}
static std::vector<FileEntry> ListNetworkEntries(const Location &l, const TransferControl &c,
                                                 ListingMethod *method,
                                                 const std::string *requestedName) {
  if (method)
    *method = ListingMethod::Provider;
  c.Check();
  if (l.scheme == "sftp")
    return ListSftp(l, c);
  std::vector<FileEntry> out;
  if (l.scheme == "smb") {
    SMBSession s(l, c);
    SMBHandle dir(s, l, true);
    for (;;) {
      std::string name;
      unsigned type;
      {
        SMBPermit permit(&c);
        errno = 0;
        auto *de =
            LIBCALL(s.lib, smbc_getFunctionReaddir)(s.ctx)(s.ctx, dir.file);
        if (!de) {
          if (errno)
            Error("SMB list folder");
          break;
        }
        name = de->name;
        type = de->smbc_type;
      }
      if (name == "." || name == "..")
        continue;
      if (type != SMBC_FILE_SHARE && type != SMBC_DIR &&
          type != SMBC_FILE && type != SMBC_LINK)
        continue;
      FileEntry e;
      e.name = name;
      e.path = ChildLocation(l.URL(), name);
      e.directory =
          type == SMBC_DIR || type == SMBC_FILE_SHARE;
      e.link = type == SMBC_LINK;
      if (type != SMBC_FILE_SHARE && !s.Stat(ParseLocation(e.path), e))
        continue;
      out.push_back(e);
    }
    return out;
  }
  std::string listing;
  bool mlsd = false;
  auto fetch = [&](const char *command) {
    listing.clear();
    CurlSession s(l, c);
    auto url = l.URL();
    if (url.back() != '/')
      url += '/';
    s.Set(CURLOPT_URL, url.c_str());
    if (command)
      s.Set(CURLOPT_CUSTOMREQUEST, command);
    s.sink = [&](const char *p, size_t n) {
      if (c.cleanup)
        c.cleanup->Consume(n);
      if (listing.size() + n > 64 * 1024 * 1024)
        throw std::runtime_error("Server listing is too large");
      listing.append(p, n);
    };
    try {
      s.Perform();
    } catch (const std::runtime_error &) {
      long reply = 0;
      LIBCALL(s.lib, curl_easy_getinfo)(s.curl, CURLINFO_RESPONSE_CODE, &reply);
      // Some FTP servers reject MLSD or interpret "-a" as a filename.
      // Do not retry cancellation, partial transfers or transport errors.
      if (command && !s.failure &&
          (reply == 500 || reply == 501 || reply == 502 || reply == 504 ||
           reply == 550))
        return false;
      throw;
    }
    // libcurl can accept a 450 list rejection as an empty directory.
    long reply = 0;
    LIBCALL(s.lib, curl_easy_getinfo)(s.curl, CURLINFO_RESPONSE_CODE, &reply);
    if (reply >= 400)
      throw std::runtime_error("FTP listing failed (reply " + std::to_string(reply) + ").");
    return true;
  };
  mlsd = fetch("MLSD");
  if (!mlsd && !fetch("LIST -a"))
    fetch(nullptr);
  if (method)
    *method = mlsd ? ListingMethod::FtpMlsd : ListingMethod::FtpList;
  std::istringstream lines(listing);
  std::string line;
  while (std::getline(lines, line)) {
    c.Check();
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;
    try {
      auto e = ParseList(line, l, mlsd);
      if (!e.name.empty())
        out.push_back(std::move(e));
    } catch (const UnverifiedFtpType &unknown) {
      if (requestedName && *requestedName != unknown.name)
        continue; // An unrelated row cannot invalidate an exact lookup.
      if (!c.listingWarning || requestedName)
        throw;
      *c.listingWarning = "Incomplete listing: FTP entries with unverified types are not shown.";
    }
  }
  return out;
}
std::vector<FileEntry> ListNetwork(const Location &l, const TransferControl &c,
                                   ListingMethod *method) {
  return ListNetworkEntries(l, c, method, nullptr);
}
bool StatNetwork(const Location &l, FileEntry &e, const TransferControl &c,
                 const FileEntry *discovered) {
  c.Check();
  if (l.scheme == "sftp")
    return StatSftp(l, e, c);
  if (l.scheme == "smb") {
    SMBSession s(l, c);
    return s.Stat(l, e);
  }
  if (l.path == "/") {
    e = {};
    e.path = l.URL();
    e.directory = true;
    return true;
  }
  FileEntry entry;
  const auto url = l.URL();
  if (discovered && discovered->path == url && !discovered->directory &&
      !discovered->link) {
    // Only the initial copy check supplies a sibling from its own traversal.
    // The post-read check still obtains the size, type, and time afresh.
    entry = *discovered;
  } else {
    ListingMethod method;
    const auto name = LocationLeaf(url);
    auto items = ListNetworkEntries(ParseLocation(ParentLocation(url)), c, &method, &name);
    auto item = std::find_if(items.begin(), items.end(), [&](const FileEntry &entry) {
      return entry.name == name;
    });
    if (item == items.end()) {
      if (VisibleEntriesOnly(method)) {
        const auto result = LookupFtpName(l, e, c);
        if (result == FtpLookup::Unsupported)
          throw std::runtime_error("FTP server did not confirm the state of this name: " + url);
        return result == FtpLookup::Found;
      }
      CurlSession probe(l, c);
      probe.Set(CURLOPT_NOBODY, 1L);
      if (probe.Perform(true) == CURLE_REMOTE_FILE_NOT_FOUND)
        return false;
      throw std::runtime_error("Remote name resolves but its entry type is not verified: " + url);
    }
    entry = std::move(*item);
  }
  e = std::move(entry);
  return true;
}
void ReadNetwork(const Location &l, const DataSink &sink,
                 const TransferControl &c) {
  if (l.scheme == "sftp")
    return ReadSftp(l, sink, c);
  if (l.scheme == "smb") {
    SMBSession s(l, c);
    SMBHandle f(s, l, false, O_RDONLY);
    char buf[32768];
    for (;;) {
      ssize_t n;
      {
        SMBPermit permit(&c);
        n = LIBCALL(s.lib, smbc_getFunctionRead)(s.ctx)(s.ctx, f.file, buf,
                                                       sizeof(buf));
        if (n < 0)
          Error("SMB read");
      }
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
  if (l.scheme == "sftp")
    return UploadSftp(local, l, c);
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
    SMBSession s(l, c);
    SMBHandle f(s, l, false, O_WRONLY | O_CREAT | O_EXCL);
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
        ssize_t wrote;
        {
          SMBPermit permit(&c);
          wrote = LIBCALL(s.lib, smbc_getFunctionWrite)(s.ctx)(
              s.ctx, f.file, buf + at, n - at);
          if (wrote <= 0)
            Error("SMB write");
        }
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
  if (l.scheme == "sftp")
    return ChangeSftp(l, action, destination, c);
  std::string a = action;
  if (l.scheme == "smb") {
    SMBSession s(l, c);
    SMBPermit permit(&c);
    int r;
    if (c.mutationSubmitted)
      *c.mutationSubmitted = true;
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
  {
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
  s.Perform(false, false, false, true);
}
} // namespace Listi
