// SPDX-License-Identifier: MIT
#include "FileProperties.h"
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Path.h>
#include <fs_attr.h>
#include <fs_index.h>
#include <Volume.h>
#include <Query.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <type_traits>
namespace Listi {
namespace {
constexpr size_t MaxValue = 65536, MaxText = 4096, MaxAttributes = 256;
void Require(status_t s, const char *what) {
  if (s != B_OK)
    throw std::runtime_error(std::string(what) + ": " + strerror(s));
}
struct NodeLock {
  BNode &node;
  explicit NodeLock(BNode &n) : node(n) {
    Require(node.Lock(), "Lock file metadata");
  }
  ~NodeLock() { node.Unlock(); }
};
bool SyncDirectory(const char *path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (fd < 0)
    return false;
  const bool flushed = fsync(fd) == 0;
  return close(fd) == 0 && flushed;
}
bool Reserved(const std::string &name) {
  return name.compare(0, 5, "BEOS:") == 0 || name.compare(0, 4, "_trk") == 0 ||
         name.compare(0, 6, "_Haiku") == 0 || name.compare(0, 6, "_listi") == 0;
}
template <class T> std::vector<uint8> Bytes(T value) {
  auto p = reinterpret_cast<const uint8 *>(&value);
  return {p, p + sizeof(value)};
}
template <class T>
bool Decode(const std::vector<uint8> &bytes, std::string &out) {
  if (bytes.size() != sizeof(T))
    return false;
  T value;
  memcpy(&value, bytes.data(), sizeof(value));
  if constexpr (std::is_floating_point<T>::value) {
    if (!std::isfinite(value))
      return false;
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::setprecision(std::numeric_limits<T>::max_digits10) << value;
    out = s.str();
  } else
    out = std::to_string(value);
  return true;
}
bool ValidText(const std::string &text) {
  // Reject malformed UTF-8 and invisible control characters, except text layout.
  for (size_t i = 0; i < text.size();) {
    uint8 c = text[i++];
    if (c < 0x80) {
      if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127)
        return false;
      continue;
    }
    unsigned count;
    uint32 code, min;
    if (c >= 0xc2 && c <= 0xdf) {
      count = 1;
      code = c & 31;
      min = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
      count = 2;
      code = c & 15;
      min = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
      count = 3;
      code = c & 7;
      min = 0x10000;
    } else
      return false;
    if (i + count > text.size())
      return false;
    while (count--) {
      uint8 next = text[i++];
      if ((next & 0xc0) != 0x80)
        return false;
      code = (code << 6) | (next & 63);
    }
    if (code < min || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
      return false;
  }
  return true;
}
bool Text(FileAttribute &a) {
  switch (a.type) {
  case B_STRING_TYPE: {
    // A native empty string may have no terminating byte at all.
    if (a.bytes.empty())
      return true;
    if (a.bytes.size() > MaxText + 1 ||
        a.bytes.back() != 0 ||
        std::find(a.bytes.begin(), a.bytes.end() - 1, 0) != a.bytes.end() - 1)
      return false;
    std::string decoded(reinterpret_cast<const char *>(a.bytes.data()),
                        a.bytes.size() - 1);
    if (!ValidText(decoded))
      return false;
    a.text = std::move(decoded);
    return true;
  }
  case B_BOOL_TYPE:
    if (a.bytes.size() != 1 || a.bytes[0] > 1)
      return false;
    a.text = a.bytes[0] ? "true" : "false";
    return true;
  case B_INT32_TYPE:
    return Decode<int32>(a.bytes, a.text);
  case B_UINT32_TYPE:
    return Decode<uint32>(a.bytes, a.text);
  case B_INT64_TYPE:
    return Decode<int64>(a.bytes, a.text);
  case B_UINT64_TYPE:
    return Decode<uint64>(a.bytes, a.text);
  case B_FLOAT_TYPE:
    return Decode<float>(a.bytes, a.text);
  case B_DOUBLE_TYPE:
    return Decode<double>(a.bytes, a.text);
  default:
    return false;
  }
}
std::string Date(time_t value) {
  char out[64];
  struct tm time {};
  localtime_r(&value, &time);
  strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &time);
  return out;
}
bool Equal(const FileAttribute &a, const FileAttribute &b) {
  return a.name == b.name && a.type == b.type && a.size == b.size &&
         a.bytes == b.bytes;
}
}
std::string AttributeTypeName(type_code type) {
  switch (type) {
  case B_STRING_TYPE:
    return "String";
  case B_BOOL_TYPE:
    return "Boolean";
  case B_INT32_TYPE:
    return "Int32";
  case B_UINT32_TYPE:
    return "UInt32";
  case B_INT64_TYPE:
    return "Int64";
  case B_UINT64_TYPE:
    return "UInt64";
  case B_FLOAT_TYPE:
    return "Float";
  case B_DOUBLE_TYPE:
    return "Double";
  default: {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << type;
    return out.str();
  }
  }
}
std::vector<uint8> EncodeAttribute(type_code type, const std::string &text) {
  if (text.size() > MaxText || !ValidText(text))
    throw std::runtime_error(
        "Value exceeds 4096 bytes or is not valid plain UTF-8 text.");
  if (type == B_STRING_TYPE) {
    std::vector<uint8> bytes(text.begin(), text.end());
    bytes.push_back(0);
    return bytes;
  }
  if (type == B_BOOL_TYPE) {
    if (text == "true" || text == "1")
      return {1};
    if (text == "false" || text == "0")
      return {0};
    throw std::runtime_error("Use true or false for a Boolean.");
  }
  if (text.empty() || text.find_first_of(" \t\r\n") != std::string::npos)
    throw std::runtime_error("Enter one number without spaces.");
  try {
    size_t end = 0;
    switch (type) {
    case B_INT32_TYPE:
    case B_INT64_TYPE: {
      auto value = std::stoll(text, &end, 10);
      if (end != text.size() ||
          (type == B_INT32_TYPE && (value < INT32_MIN || value > INT32_MAX)))
        throw std::out_of_range("number");
      return type == B_INT32_TYPE ? Bytes(int32(value)) : Bytes(int64(value));
    }
    case B_UINT32_TYPE:
    case B_UINT64_TYPE: {
      if (text[0] == '-')
        throw std::out_of_range("number");
      auto value = std::stoull(text, &end, 10);
      if (end != text.size() || (type == B_UINT32_TYPE && value > UINT32_MAX))
        throw std::out_of_range("number");
      return type == B_UINT32_TYPE ? Bytes(uint32(value))
                                   : Bytes(uint64(value));
    }
    case B_FLOAT_TYPE: {
      auto value = std::stof(text, &end);
      if (end != text.size() || !std::isfinite(value))
        throw std::out_of_range("number");
      return Bytes(value);
    }
    case B_DOUBLE_TYPE: {
      auto value = std::stod(text, &end);
      if (end != text.size() || !std::isfinite(value))
        throw std::out_of_range("number");
      return Bytes(value);
    }
    default:
      throw std::runtime_error("This attribute type is read-only.");
    }
  } catch (const std::invalid_argument &) {
    throw std::runtime_error("Invalid numeric value.");
  } catch (const std::out_of_range &) {
    throw std::runtime_error("Value is outside this type's range.");
  }
}
PropertySession::PropertySession(const std::string &p) : path(p) {
  if (path.empty() || path[0] != '/')
    throw std::runtime_error(
        "Properties currently require one local file or folder.");
  BEntry entry(path.c_str(), false);
  Require(entry.InitCheck(), "Open file properties");
  BPath canonical;
  Require(entry.GetPath(&canonical), "Resolve property path");
  path = canonical.Path();
  Require(node.SetTo(&entry), "Open native metadata");
  Require(node.GetNodeRef(&identity), "Read file identity");
  Guard();
}
void PropertySession::Guard() {
  struct stat current, opened;
  Require(node.GetStat(&opened), "Read open file identity");
  if (lstat(path.c_str(), &current) || current.st_dev != identity.device ||
      current.st_ino != identity.node || opened.st_dev != identity.device ||
      opened.st_ino != identity.node)
    throw std::runtime_error(
        "This file was moved, removed or replaced. Close properties and open it again.");
}
FileAttribute PropertySession::Attribute(const std::string &name) {
  FileAttribute a;
  a.name = name;
  attr_info info;
  Require(node.GetAttrInfo(name.c_str(), &info), "Read attribute information");
  a.type = info.type;
  a.size = info.size;
  if (info.size < 0 || info.size > off_t(MaxValue)) {
    a.text = "Large value: inspection only";
    return a;
  }
  a.bytes.resize(info.size);
  if (info.size && node.ReadAttr(name.c_str(), info.type, 0, a.bytes.data(),
                                 a.bytes.size()) != info.size)
    throw std::runtime_error(
        "Attribute changed or could not be read. Refresh before editing.");
  attr_info after;
  Require(node.GetAttrInfo(name.c_str(), &after), "Recheck attribute");
  if (after.type != info.type || after.size != info.size)
    throw std::runtime_error("Attribute changed while reading. Refresh again.");
  a.editable = Text(a) && !Reserved(name);
  if (a.text.empty() && !a.editable) {
    std::ostringstream out;
    out << "Hex:";
    for (size_t i = 0; i < std::min<size_t>(32, a.bytes.size()); ++i)
      out << ' ' << std::hex << std::setw(2) << std::setfill('0')
          << unsigned(a.bytes[i]);
    if (a.bytes.size() > 32)
      out << " ...";
    a.text = out.str();
  }
  return a;
}
std::string AttributeCell(BNode &node, const char *name, type_code *type) {
  if (type)
    *type = 0;
  attr_info info;
  auto status = node.GetAttrInfo(name, &info);
  if (status == B_ENTRY_NOT_FOUND)
    return "(missing)";
  if (status != B_OK)
    return "(unavailable)";
  FileAttribute value;
  value.type = info.type;
  if (info.size < 0 || info.size > off_t(MaxText + 1))
    return AttributeTypeName(info.type) + " (" + std::to_string(info.size) +
           " bytes)";
  value.bytes.resize(info.size);
  if (info.size && node.ReadAttr(name, info.type, 0, value.bytes.data(),
                                 info.size) != info.size)
    return "(changed)";
  if (!Text(value))
    return AttributeTypeName(info.type) + " (" + std::to_string(info.size) +
           " bytes)";
  if (type)
    *type = info.type;
  if (value.text.empty())
    return "(empty)";
  std::replace_if(
      value.text.begin(), value.text.end(),
      [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
  return value.text;
}

bool IndexableAttribute(type_code type) {
  return type == B_STRING_TYPE || type == B_INT32_TYPE ||
         type == B_INT64_TYPE || type == B_FLOAT_TYPE || type == B_DOUBLE_TYPE;
}

std::string PropertySession::MakeSearchable(const FileAttribute &expected) {
  Guard();
  const auto attribute = Attribute(expected.name);
  if (!IndexableAttribute(attribute.type) || !Equal(expected, attribute))
    throw std::runtime_error(
        "The selected attribute changed or its type cannot be indexed.");
  BVolume volume(identity.device);
  if (volume.InitCheck() != B_OK || !volume.KnowsQuery() || !volume.KnowsAttr())
    throw std::runtime_error("This volume does not support attribute queries.");
  index_info info;
  if (fs_stat_index(identity.device, attribute.name.c_str(), &info) != 0) {
    if (errno != ENOENT || volume.IsReadOnly())
      throw std::runtime_error(
          "Cannot inspect or create an index on this volume.");
    if (fs_create_index(identity.device, attribute.name.c_str(), attribute.type,
                        0) != 0 &&
        errno != EEXIST)
      throw std::runtime_error(std::string("Create volume index: ") +
                               strerror(errno));
    if (fs_stat_index(identity.device, attribute.name.c_str(), &info) != 0)
      throw std::runtime_error("Cannot verify the volume index.");
  }
  if (info.type != attribute.type)
    throw std::runtime_error(
        "The existing volume index has a different type. It was not changed.");
  // Ask BFS itself. Creating an index is not a request to rewrite older attributes.
  BQuery query;
  status_t status = query.SetVolume(&volume);
  if (status == B_OK)
    status = query.PushAttr(attribute.name.c_str());
  if (status == B_OK) {
    auto value = attribute;
    if (!Text(value))
      return "Index ready. This value could not be checked with a query.";
    switch (value.type) {
    case B_STRING_TYPE:
      status = query.PushString(value.text.c_str());
      break;
    case B_INT32_TYPE:
      status = query.PushInt32(std::stol(value.text));
      break;
    case B_INT64_TYPE:
      status = query.PushInt64(std::stoll(value.text));
      break;
    case B_FLOAT_TYPE:
      status = query.PushFloat(std::stof(value.text));
      break;
    default:
      status = query.PushDouble(std::stod(value.text));
      break;
    }
  }
  if (status == B_OK)
    status = query.PushOp(B_EQ);
  if (status == B_OK)
    status = query.Fetch();
  if (status != B_OK)
    return "Index ready. The query could not be verified.";
  BEntry item;
  for (size_t n = 0; n < 1024; ++n) {
    auto next = query.GetNextEntry(&item);
    if (next == B_ENTRY_NOT_FOUND)
      return "Index ready. This existing entry was not returned; older attributes may need reindexing.";
    if (next != B_OK)
      break;
    node_ref ref;
    if (item.GetNodeRef(&ref) == B_OK && ref == identity)
      return "Index ready. This entry was found by a volume query.";
  }
  return "Index ready. This entry was not verified within the query limit.";
}

PropertySnapshot PropertySession::Read() {
  Guard();
  PropertySnapshot result;
  result.path = path;
  Require(node.GetStat(&result.stat), "Read file properties");
  result.writableKind =
      S_ISREG(result.stat.st_mode) || S_ISDIR(result.stat.st_mode);
  std::ostringstream info;
  info << (S_ISDIR(result.stat.st_mode)   ? "Folder"
           : S_ISLNK(result.stat.st_mode) ? "Symbolic link"
                                          : "File")
       << " | " << result.stat.st_size << " bytes (entry size) | owner "
       << result.stat.st_uid << " | group " << result.stat.st_gid
       << "\nModified: " << Date(result.stat.st_mtime)
       << " | Created: " << Date(result.stat.st_crtime);
  if (S_ISLNK(result.stat.st_mode)) {
    char link[PATH_MAX];
    auto n = readlink(path.c_str(), link, sizeof(link));
    if (n > 0)
      info << "\nLink target: " << std::string(link, n) << " (not followed)";
  }
  result.summary = info.str();
  Require(node.RewindAttrs(), "Read attributes");
  char name[B_ATTR_NAME_LENGTH];
  size_t total = 0;
  for (;;) {
    status_t status = node.GetNextAttrName(name);
    if (status == B_ENTRY_NOT_FOUND)
      break;
    Require(status, "Read attribute name");
    if (result.attributes.size() >= MaxAttributes || total >= 1024 * 1024) {
      result.limited = true;
      break;
    }
    auto attribute = Attribute(name);
    total += attribute.bytes.size();
    attribute.editable &= result.writableKind;
    result.attributes.push_back(std::move(attribute));
  }
  std::sort(result.attributes.begin(), result.attributes.end(),
            [](const auto &a, const auto &b) { return a.name < b.name; });
  Guard();
  return result;
}
void PropertySession::Change(const FileAttribute *expected,
                             const std::string &name, type_code type,
                             const std::vector<uint8> *bytes, bool remove) {
  if (name.empty() || name.size() >= B_ATTR_NAME_LENGTH || !ValidText(name) ||
      name.find_first_of("\n\r\t") != std::string::npos || Reserved(name))
    throw std::runtime_error(
        "Choose an ordinary attribute name. Native system attributes are read-only here.");
  NodeLock lock(node);
  Guard();
  struct stat st;
  Require(node.GetStat(&st), "Read file mode");
  if (!(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)))
    throw std::runtime_error("Link and special-file attributes are read-only.");
  attr_info info;
  auto status = node.GetAttrInfo(name.c_str(), &info);
  if (expected) {
    if (!expected->editable || expected->name != name || status != B_OK ||
        !Equal(*expected, Attribute(name)))
      throw std::runtime_error(
          "Attribute changed since it was loaded. Refresh and inspect the new value.");
  } else if (status != B_ENTRY_NOT_FOUND)
    throw std::runtime_error(
        "Attribute already exists or cannot be inspected. Nothing changed.");
  // Save a bounded recovery record before the first mutation. It also makes
  // partial-write or sync failures recoverable without changing the file bytes.
  BPath directory;
  Require(find_directory(B_USER_SETTINGS_DIRECTORY, &directory),
          "Find recovery folder");
  const std::string settingsFolder = directory.Path();
  Require(directory.Append("Lister_attribute_recovery"), "Locate recovery folder");
  if (mkdir(directory.Path(), 0700) && errno != EEXIST)
    throw std::runtime_error("Cannot create metadata recovery folder.");
  struct stat directoryStat;
  if (lstat(directory.Path(), &directoryStat) ||
      !S_ISDIR(directoryStat.st_mode) || directoryStat.st_uid != geteuid() ||
      (directoryStat.st_mode & 0077))
    throw std::runtime_error(
        "Metadata recovery folder must be a private directory owned by the current user.");
  BMessage record;
  Require(record.AddString("path", path.c_str()), "Record recovery path");
  Require(record.AddString("attribute", name.c_str()), "Record attribute name");
  Require(record.AddInt32("device", identity.device), "Record device");
  Require(record.AddInt64("inode", identity.node), "Record inode");
  Require(record.AddBool("existed", expected != nullptr),
          "Record original attribute existence");
  if (expected) {
    Require(record.AddInt32("type", expected->type), "Record attribute type");
    // BMessage does not accept zero-length raw data. Distinguish an existing
    // empty attribute from an absent attribute or a missing payload.
    Require(record.AddInt64("size", expected->bytes.size()),
            "Record attribute size");
    if (!expected->bytes.empty())
      Require(record.AddData("bytes", B_RAW_TYPE, expected->bytes.data(),
                             expected->bytes.size()), "Record attribute bytes");
  }
  const ssize_t size = record.FlattenedSize();
  if (size <= 0 || size > ssize_t(MaxValue + PATH_MAX + B_ATTR_NAME_LENGTH + 1024))
    throw std::runtime_error("Invalid metadata recovery record size. Nothing changed.");
  std::vector<char> flattened(size);
  Require(record.Flatten(flattened.data(), flattened.size()),
          "Prepare recovery record");
  std::string pattern = std::string(directory.Path()) + "/change-XXXXXX";
  std::vector<char> file(pattern.begin(), pattern.end());
  file.push_back(0);
  int fd = mkstemp(file.data());
  if (fd < 0)
    throw std::runtime_error(
        "Cannot create metadata recovery record. Nothing changed.");
  std::string recovery = file.data();
  size_t writtenBytes = 0;
  while (writtenBytes < flattened.size()) {
    ssize_t n = write(fd, flattened.data() + writtenBytes,
                      flattened.size() - writtenBytes);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    writtenBytes += n;
  }
  const bool backupOK = writtenBytes == flattened.size() && fsync(fd) == 0;
  const bool closed = close(fd) == 0;
  // File sync alone does not persist the new directory entry. The recovery
  // file and its private folder must be discoverable before changing metadata.
  if (!backupOK || !closed || !SyncDirectory(directory.Path()) ||
      !SyncDirectory(settingsFolder.c_str()))
    throw std::runtime_error(
        "Cannot flush metadata recovery record. Nothing changed. Record: " +
        recovery);
  Guard();
  if (expected && !Equal(*expected, Attribute(name)))
    throw std::runtime_error(
        "Attribute changed before saving. Recovery record: " + recovery);
  if (!expected && node.GetAttrInfo(name.c_str(), &info) != B_ENTRY_NOT_FOUND)
    throw std::runtime_error(
        "Attribute appeared before saving. Recovery record: " + recovery);
  status = remove ? node.RemoveAttr(name.c_str())
                  : status_t(node.WriteAttr(name.c_str(), type, 0,
                                            bytes->data(), bytes->size()));
  bool written = remove ? status == B_OK : status == ssize_t(bytes->size());
  try {
    if (written && node.Sync() == B_OK) {
      bool verified = false;
      if (remove)
        verified = node.GetAttrInfo(name.c_str(), &info) == B_ENTRY_NOT_FOUND;
      else {
        auto now = Attribute(name);
        verified = now.type == type && now.bytes == *bytes;
      }
      if (verified) {
        Guard();
        unlink(recovery.c_str());
        return;
      }
    }
  } catch (
      const std::exception &) { /* Report the retained recovery path below. */
  }
  // Do not blindly roll back over another application's write. Keep the exact
  // pre-edit bytes in the recovery record and report the uncertainty explicitly.
  throw std::runtime_error(
      "Metadata save could not be verified. Inspect before retrying. Original metadata recovery record: " +
      recovery);
}
void PropertySession::Save(const FileAttribute *expected,
                           const std::string &name, type_code type,
                           const std::string &text) {
  if (expected && type != expected->type)
    throw std::runtime_error(
        "Existing attribute types cannot be changed here.");
  auto bytes = EncodeAttribute(type, text);
  Change(expected, name, type, &bytes, false);
}
void PropertySession::Remove(const FileAttribute &expected) {
  Change(&expected, expected.name, expected.type, nullptr, true);
}
void PropertySession::Permissions(mode_t expected, const std::string &octal) {
  if (octal.size() < 3 || octal.size() > 4 ||
      octal.find_first_not_of("01234567") != std::string::npos)
    throw std::runtime_error(
        "Enter permissions as three octal digits, such as 644 or 755.");
  auto requested = std::stoul(octal, nullptr, 8);
  if (requested > 0777)
    throw std::runtime_error("Special permission bits are read-only here.");
  NodeLock lock(node);
  Guard();
  struct stat st;
  Require(node.GetStat(&st), "Read permissions");
  if (!(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) ||
      (st.st_mode & 07777) != expected || (expected & 07000))
    throw std::runtime_error(
        "Permissions changed or this entry is read-only here. Refresh first.");
  Require(node.SetPermissions(requested), "Set permissions");
  if (node.Sync() != B_OK)
    throw std::runtime_error(
        "Permissions were changed, but could not be flushed. Refresh before retrying.");
  Guard();
  Require(node.GetStat(&st), "Verify permissions");
  if ((st.st_mode & 07777) != requested)
    throw std::runtime_error(
        "Permission change could not be verified. Refresh.");
}
}
