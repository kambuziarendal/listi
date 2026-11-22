// SPDX-License-Identifier: MIT
#include "RecoveryInventory.h"
#include "MessageStore.h"
#include <FindDirectory.h>
#include <Path.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>
#include <vector>

namespace Listi {
namespace fs = std::filesystem;
namespace {
constexpr uint32 kRecovery = 'rcvr';
constexpr size_t kReviewLimit = 16;
bool Same(const struct stat &a, const struct stat &b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
std::string Directory(const std::string &override, bool create) {
  std::string directory = override;
  if (directory.empty()) {
    BPath path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK ||
        path.Append("Listi-recovery") != B_OK)
      throw std::runtime_error("Cannot locate Listi recovery settings.");
    directory = path.Path();
  }
  if (create) {
    if (mkdir(directory.c_str(), 0700) == 0) {
      const int parent = open(fs::path(directory).parent_path().c_str(),
                              O_RDONLY | O_DIRECTORY);
      if (parent < 0)
        throw std::runtime_error("Cannot open the recovery inventory parent.");
      const bool synced = fsync(parent) == 0;
      close(parent);
      if (!synced)
        throw std::runtime_error("Cannot sync the recovery inventory parent.");
    } else if (errno != EEXIST) {
      throw std::runtime_error("Cannot create private Listi recovery inventory.");
    }
  }
  struct stat st;
  if (lstat(directory.c_str(), &st)) {
    if (!create && errno == ENOENT)
      return {};
    throw std::runtime_error("Cannot inspect Listi recovery inventory.");
  }
  if (!S_ISDIR(st.st_mode) || (st.st_mode & 0077) || st.st_uid != geteuid())
    throw std::runtime_error("Listi recovery inventory is not a private directory.");
  return directory;
}
std::string Printable(const std::string &path) {
  std::string out;
  for (unsigned char byte : path) {
    if (byte < ' ' || byte == 127)
      out += '?';
    else
      out += char(byte);
  }
  return out;
}
} // namespace

RecoveryRecord::RecoveryRecord(const std::string &kind, const std::string &entry,
                               const struct stat &expected,
                               const std::string &inventory) {
  const auto directory = Directory(inventory, true);
  BMessage message(kRecovery);
  const auto parent = fs::path(entry).parent_path();
  struct stat parentStat;
  if (stat(parent.c_str(), &parentStat))
    throw std::runtime_error("Cannot identify the recovery entry's parent.");
  if (message.AddString("kind", kind.c_str()) != B_OK ||
      message.AddString("path", entry.c_str()) != B_OK ||
      message.AddInt64("device", expected.st_dev) != B_OK ||
      message.AddInt64("node", expected.st_ino) != B_OK ||
      message.AddInt64("created", expected.st_crtime) != B_OK ||
      message.AddInt64("parent_device", parentStat.st_dev) != B_OK ||
      message.AddInt64("parent_node", parentStat.st_ino) != B_OK)
    throw std::runtime_error("Cannot prepare Listi recovery record.");

  auto pattern = directory + "/record-XXXXXX";
  const int fd = mkstemp(pattern.data());
  if (fd < 0)
    throw std::runtime_error("Cannot reserve a Listi recovery record.");
  close(fd);
  path = std::move(pattern);
  SaveMessageStore(path, message);
  if (lstat(path.c_str(), &identity) || !S_ISREG(identity.st_mode))
    throw std::runtime_error("Cannot identify the stored Listi recovery record.");
}

void RecoveryRecord::Retire() {
  const auto record = fs::path(path);
  const int parent = open(record.parent_path().c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (parent < 0)
    throw std::runtime_error("Cannot open the Listi recovery inventory.");
  struct stat now;
  const auto leaf = record.filename();
  const bool removed =
      !fstatat(parent, leaf.c_str(), &now, AT_SYMLINK_NOFOLLOW) &&
      Same(identity, now) && !unlinkat(parent, leaf.c_str(), 0) && !fsync(parent);
  close(parent);
  if (!removed)
    throw std::runtime_error("Could not confirm removal of the Listi recovery record.");
}

std::string ReviewRecovery(const std::string &inventory) {
  const auto directory = Directory(inventory, false);
  if (directory.empty())
    return "No pending recovery records.";
  std::string review;
  size_t count = 0;
  for (const auto &record : fs::directory_iterator(directory)) {
    const auto name = record.path().filename().string();
    if (name.size() != 13 || name.rfind("record-", 0) != 0 ||
        !fs::is_regular_file(record.symlink_status()))
      continue;
    if (++count > kReviewLimit) {
      review += "\nMore records are available in " + Printable(directory) + ".";
      break;
    }
    try {
      const auto message = LoadMessageStore(record.path());
      const std::string kind = message.GetString("kind", "");
      const std::string path = message.GetString("path", "");
      if (message.what != kRecovery || (kind != "archive" && kind != "rename") ||
          path.empty() || path[0] != '/')
        throw std::runtime_error("Invalid record.");
      struct stat st, parent;
      const bool matched = !lstat(path.c_str(), &st) &&
          !stat(fs::path(path).parent_path().c_str(), &parent) &&
          st.st_dev == message.GetInt64("device", -1) &&
          st.st_ino == message.GetInt64("node", -1) &&
          st.st_crtime == message.GetInt64("created", -1) &&
          parent.st_dev == message.GetInt64("parent_device", -1) &&
          parent.st_ino == message.GetInt64("parent_node", -1);
      review += (kind == "archive" ? "Archive staging: " : "Bulk rename journal: ");
      review += Printable(path);
      review += matched ? "\nRecorded identity matches. Review only.\n"
                        : "\nMissing or identity changed. Do not reuse this path.\n";
    } catch (const std::exception &) {
      review += "Unreadable recovery record: " + Printable(record.path()) + "\n";
    }
  }
  return count ? review : "No pending recovery records.";
}
} // namespace Listi
