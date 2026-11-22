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
void Field(status_t status) {
  if (status != B_OK)
    throw std::runtime_error("Cannot prepare Listi recovery record.");
}
void Identify(BMessage &message, const struct stat &expected) {
  Field(message.SetInt64("device", expected.st_dev));
  Field(message.SetInt64("node", expected.st_ino));
  Field(message.SetInt64("created", expected.st_crtime));
}
void IdentifyParent(BMessage &message, const std::string &entry) {
  struct stat st;
  if (stat(fs::path(entry).parent_path().c_str(), &st))
    throw std::runtime_error("Cannot identify the recovery entry's parent.");
  Field(message.SetInt64("parent_device", st.st_dev));
  Field(message.SetInt64("parent_node", st.st_ino));
}
} // namespace

RecoveryRecord::~RecoveryRecord() = default;

void RecoveryRecord::Reserve(const std::string &directory) {
  if (lstat(directory.c_str(), &inventoryIdentity) || !S_ISDIR(inventoryIdentity.st_mode) ||
      (inventoryIdentity.st_mode & 0077) || inventoryIdentity.st_uid != geteuid())
    throw std::runtime_error("Listi recovery inventory changed.");
  path = directory + "/record-XXXXXX";
  const int fd = mkstemp(path.data());
  if (fd < 0)
    throw std::runtime_error("Cannot reserve a Listi recovery record.");
  const bool known = !fstat(fd, &identity);
  if (close(fd) || !known)
    throw std::runtime_error("Cannot identify a reserved recovery record.");
}

void RecoveryRecord::Save() {
  SaveMessageStore(path, *message, &identity, &identity, &inventoryIdentity);
}

RecoveryRecord::RecoveryRecord(const std::string &kind, const std::string &entry,
                               const struct stat &expected,
                               const std::string &inventory)
    : message(std::make_unique<BMessage>(kRecovery)), stage(entry) {
  Field(message->AddString("kind", kind.c_str()));
  Field(message->AddString("path", entry.c_str()));
  Identify(*message, expected);
  IdentifyParent(*message, entry);
  Reserve(Directory(inventory, true));
  try { Save(); }
  catch (...) { Retire(); throw; }
}

RecoveryRecord::RecoveryRecord(const std::string &kind, const std::string &folder,
                               const std::string &prefix, const std::string &source,
                               const std::string &destination, const std::string &inventory)
    : message(std::make_unique<BMessage>(kRecovery)) {
  Reserve(Directory(inventory, true));
  const auto id = path.substr(path.rfind('-') + 1);
  stage = folder + (folder.back() == '/' ? "" : "/") + prefix + id;
  if (kind == "upload") {
    backup = stage + ".previous";
    stage += ".part";
    Field(message->AddString("backup", backup.c_str()));
  } else {
    IdentifyParent(*message, stage);
  }
  Field(message->AddString("kind", kind.c_str()));
  Field(message->AddString("path", stage.c_str()));
  Field(message->AddString("source", source.c_str()));
  Field(message->AddString("destination", destination.c_str()));
  Field(message->AddInt32("phase", static_cast<int32>(phase)));
  if (kind == "copy") {
    struct stat st;
    if (!lstat(source.c_str(), &st)) {
      Field(message->AddInt64("source_device", st.st_dev));
      Field(message->AddInt64("source_node", st.st_ino));
    }
    if (!lstat(destination.c_str(), &st)) {
      Field(message->AddInt64("original_device", st.st_dev));
      Field(message->AddInt64("original_node", st.st_ino));
    }
  }
  try { Save(); }
  catch (...) { Retire(); throw; }
}

void RecoveryRecord::Advance(RecoveryPhase next, const struct stat *bound) {
  if (int(next) != int(phase) + 1)
    throw std::logic_error("Invalid Listi recovery transition.");
  if (bound)
    Identify(*message, *bound);
  SavePhase(next);
}

void RecoveryRecord::PrepareCopyPublication() {
  if (phase != RecoveryPhase::Writing || std::string(message->GetString("kind", "")) != "copy")
    throw std::logic_error("Invalid Listi copy publication transition.");
  // No namespace operation separates Ready from Publishing in a local copy.
  // Until this save succeeds the durable Writing state still excludes rename.
  SavePhase(RecoveryPhase::Publishing);
}

void RecoveryRecord::SavePhase(RecoveryPhase next) {
  Field(message->SetInt32("phase", static_cast<int32>(next)));
  Save();
  phase = next;
}

void RecoveryRecord::NoteFailure(bool cancelled) noexcept {
  try {
    Field(message->SetString("outcome", cancelled ? "Cancelled" : "Failed or interrupted"));
    Save();
  } catch (...) {
    // The last synced phase is still sufficient for conservative review.
  }
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
      !fstat(parent, &now) && Same(inventoryIdentity, now) &&
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
  std::vector<std::pair<fs::file_time_type, fs::path>> records;
  for (const auto &record : fs::directory_iterator(directory)) {
    const auto name = record.path().filename().string();
    if (name.size() != 13 || name.rfind("record-", 0) != 0 ||
        !fs::is_regular_file(record.symlink_status()))
      continue;
    std::error_code error;
    const auto stamp = record.last_write_time(error);
    records.emplace_back(error ? fs::file_time_type::min() : stamp, record.path());
  }
  std::sort(records.rbegin(), records.rend());
  std::string review;
  for (size_t i = 0; i < std::min(kReviewLimit, records.size()); ++i) {
    const auto &record = records[i].second;
    try {
      const auto message = LoadMessageStore(record, true);
      const std::string kind = message.GetString("kind", "");
      const std::string path = message.GetString("path", "");
      const bool remote = kind == "upload";
      if (message.what != kRecovery ||
          (kind != "archive" && kind != "rename" && kind != "copy" && kind != "codec" && kind != "scratch" && !remote) ||
          path.empty() || (!remote && path[0] != '/'))
        throw std::runtime_error("Invalid record.");
      struct stat st, parent;
      const bool matched = !remote && !lstat(path.c_str(), &st) &&
          !stat(fs::path(path).parent_path().c_str(), &parent) &&
          st.st_dev == message.GetInt64("device", -1) &&
          st.st_ino == message.GetInt64("node", -1) &&
          st.st_crtime == message.GetInt64("created", -1) &&
          parent.st_dev == message.GetInt64("parent_device", -1) &&
          parent.st_ino == message.GetInt64("parent_node", -1);
      const int phase = message.GetInt32("phase", -1);
      review += kind == "archive" ? "Archive staging: " : kind == "rename" ?
          "Bulk rename journal: " : remote ? "Remote upload: " :
          kind == "codec" ? "Codec scratch: " : kind == "scratch" ? "Local transfer staging: " : "File copy: ";
      review += Printable(path);
      if (phase >= 0) {
        review += "\nOperation: " + Printable(record.filename()) +
                  " (" + Printable(message.GetString("outcome", "interrupted or active")) + ")";
        review += "\nSource: " + Printable(message.GetString("source", "")) +
                  "\nDestination: " + Printable(message.GetString("destination", ""));
        if (remote)
          review += "\nPossible original (manual inspection): " +
                    Printable(message.GetString("backup", ""));
        const char *labels[] = {"Stage creation not confirmed; inspect manually.",
            "Writing or verifying; not published.", "Ready for publication.",
            "Uncertain publication; inspect both names.",
            "Published; cleanup or directory flush not confirmed."};
        review += "\n" + std::string(phase < 5 ? labels[phase] : "Unknown stage; inspect manually.");
      }
      if (remote) {
        review += "\nRemote ownership cannot be proved; review only.\n";
        continue;
      }
      review += matched ? "\nRecorded identity matches. Review only.\n"
                        : "\nMissing or identity changed. Do not reuse this path.\n";
    } catch (const std::exception &) {
      review += "Unreadable recovery record: " + Printable(record) + "\n";
    }
  }
  if (records.size() > kReviewLimit)
    review += "\nMore records are available in " + Printable(directory) + ".";
  return records.empty() ? "No pending recovery records." : review;
}
} // namespace Listi
