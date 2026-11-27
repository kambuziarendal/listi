// SPDX-License-Identifier: MIT
#include "FileOperation.h"
#include "Trash.h"
#include <Directory.h>
#include <Entry.h>
#include <Node.h>
#include <String.h>
#include <TypeConstants.h>
#include <cstring>
#include <fs_attr.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace Listi;
namespace fs = std::filesystem;
namespace {
enum class Fault { None, Read, Move, Rollback };
Fault fault = Fault::None;
const char *attribute = "_trk/original_path";
constexpr char oldPath[] = "/previous/Tracker restore path";
unsigned moves = 0;

void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
  std::cout << "PASS " << message << std::endl;
}
std::string Bytes(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}
}

extern "C" ssize_t __real__ZNK5BNode8ReadAttrEPKcjlPvm(
    const BNode *, const char *, type_code, off_t, void *, size_t);
extern "C" ssize_t __wrap__ZNK5BNode8ReadAttrEPKcjlPvm(
    const BNode *node, const char *name, type_code type, off_t at,
    void *data, size_t size) {
  if (fault == Fault::Read && !strcmp(name, attribute))
    return B_IO_ERROR;
  return __real__ZNK5BNode8ReadAttrEPKcjlPvm(node, name, type, at, data, size);
}
extern "C" ssize_t __real__ZN5BNode9WriteAttrEPKcjlPKvm(
    BNode *, const char *, type_code, off_t, const void *, size_t);
extern "C" ssize_t __wrap__ZN5BNode9WriteAttrEPKcjlPKvm(
    BNode *node, const char *name, type_code type, off_t at,
    const void *data, size_t size) {
  if (fault == Fault::Rollback && !strcmp(name, attribute) &&
      size == sizeof(oldPath) && !memcmp(data, oldPath, size))
    return B_IO_ERROR;
  return __real__ZN5BNode9WriteAttrEPKcjlPKvm(node, name, type, at, data, size);
}
extern "C" status_t __real__ZN6BEntry6MoveToEP10BDirectoryPKcb(
    BEntry *, BDirectory *, const char *, bool);
extern "C" status_t __wrap__ZN6BEntry6MoveToEP10BDirectoryPKcb(
    BEntry *entry, BDirectory *dir, const char *name, bool clobber) {
  ++moves;
  if (fault == Fault::Move || fault == Fault::Rollback)
    return B_PERMISSION_DENIED;
  return __real__ZN6BEntry6MoveToEP10BDirectoryPKcb(entry, dir, name, clobber);
}

int main() try {
  char path[] = "/boot/home/listi-trash-metadata-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0)
    throw std::runtime_error("create disposable BFS file");
  close(fd);
  std::ofstream(path) << "payload";
  BNode node(path);
  const auto destination = fs::path(TrashDirectory(path, true)) / fs::path(path).filename();
  for (auto scenario : {Fault::Read, Fault::Move, Fault::Rollback}) {
    Check(node.WriteAttr(attribute, B_STRING_TYPE, 0, oldPath, sizeof(oldPath)) ==
              ssize_t(sizeof(oldPath)), "seed existing restore metadata");
    moves = 0;
    fault = scenario;
    auto result = Run({Action::Trash, {path}, {}}, {});
    fault = Fault::None;
    Check(!result.completed && result.errors.size() == 1 && Bytes(path) == "payload" &&
              !fs::exists(destination) && moves == (scenario == Fault::Read ? 0u : 1u),
          "failed Trash leaves file bytes in place and does not claim a move");
    const bool rollbackError = scenario == Fault::Rollback;
    Check((result.errors.front().find("Trash metadata may have changed") != std::string::npos) ==
              rollbackError, "job distinguishes failed metadata rollback");
    const std::string expected = rollbackError ? path : oldPath;
    attr_info saved{};
    std::string raw(expected.size() + 1, '\0');
    Check(node.GetAttrInfo(attribute, &saved) == B_OK &&
              saved.type == B_STRING_TYPE && saved.size == ssize_t(raw.size()) &&
              node.ReadAttr(attribute, saved.type, 0, raw.data(), raw.size()) == saved.size &&
              raw == expected + '\0', "rollback leaves the reported attribute type, size and bytes");
    BString value;
    Check(node.ReadAttrString(attribute, &value) == B_OK &&
              value == (rollbackError ? path : oldPath),
          "previous Tracker path survives unless the job explicitly reports rollback failure");
    auto retry = Run({Action::Trash, {path}, {}}, {});
    Check(retry.completed == 1 && retry.errors.empty() && !fs::exists(path) &&
              Bytes(destination.string()) == "payload" &&
              node.ReadAttrString(attribute, &value) == B_OK && value == path,
          "explicit retry moves file bytes and saves the correct restore path");
    auto restore = Run({Action::Restore, {destination.string()}, {}}, {});
    attr_info info;
    Check(restore.completed == 1 && restore.errors.empty() && !fs::exists(destination) &&
              Bytes(path) == "payload" && node.GetAttrInfo(attribute, &info) == B_ENTRY_NOT_FOUND,
          "retry can be restored with the metadata cleared");
  }
  unlink(path);
  return 0;
} catch (const std::exception &e) {
  std::cerr << "FAIL " << e.what() << std::endl;
  return 1;
}
