// SPDX-License-Identifier: MIT
#include "ClipboardTextFile.h"
#include "ClipboardImage.h"
#include "ClipboardLock.h"
#include "ExclusiveFileOutput.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace Listi {
namespace {
struct FD {
  int value;
  explicit FD(int fd) : value(fd) {}
  ~FD() {
    if (value >= 0)
      close(value);
  }
  FD(const FD &) = delete;
  FD &operator=(const FD &) = delete;
};

bool IsFullPath(const std::string &folder) {
  return !folder.empty() && folder.front() == '/' &&
         folder.find('\0') == std::string::npos;
}

bool IsWritable(int directory) {
  struct statvfs volume;
  return directory >= 0 && fstatvfs(directory, &volume) == 0 &&
         !(volume.f_flag & ST_RDONLY) &&
         faccessat(directory, ".", W_OK | X_OK, 0) == 0;
}

void Fail(const char *operation) {
  throw std::runtime_error(std::string(operation) + ": " + strerror(errno));
}
}

bool CanSaveClipboardIn(const std::string &folder) {
  if (!IsFullPath(folder))
    return false;
  FD directory(open(folder.c_str(), O_RDONLY | O_DIRECTORY));
  return IsWritable(directory.value);
}

std::optional<ClipboardContent> DetectClipboardSaveContent(BClipboard *clipboard) {
  ClipboardLock lock(clipboard);
  const BMessage *message = clipboard->Data();
  if (!message)
    throw std::runtime_error("Could not read the clipboard.");
  if (HasClipboardFiles(*message))
    return std::nullopt;
  // Prefer actual image data to an accompanying text representation.
  if (message->HasData("image/png", B_MIME_TYPE) ||
      message->HasMessage("image/bitmap") ||
      message->HasMessage("image/x-be-bitmap"))
    return ClipboardContent::Image;
  if (message->HasData("text/plain", B_MIME_TYPE))
    return ClipboardContent::Text;
  throw std::runtime_error("Copy files, plain text, or an image before pasting.");
}

ClipboardFile PrepareClipboardFile(const std::string &folder,
                                   ClipboardContent content,
                                   BClipboard *clipboard) {
  FD directory(IsFullPath(folder) ? open(folder.c_str(), O_RDONLY | O_DIRECTORY)
                                  : -1);
  struct stat st;
  if (!IsWritable(directory.value) || fstat(directory.value, &st))
    throw std::runtime_error("Open a writable local folder first.");
  const bool image = content == ClipboardContent::Image;
  std::string extension = image ? ".png" : ".txt";
  ClipboardFile file{folder,
                     image ? ReadClipboardPNG(clipboard)
                           : ReadClipboardText(clipboard),
                     "Clipboard" + extension,
                     {int32_t(st.st_dev), int64_t(st.st_ino)}};
  for (int i = 1; i <= 1000; ++i) {
    if (i > 1)
      file.suggestedName = "Clipboard " + std::to_string(i) + extension;
    if (fstatat(directory.value, file.suggestedName.c_str(), &st,
                AT_SYMLINK_NOFOLLOW) == 0)
      continue;
    if (errno == ENOENT)
      return file;
    Fail("Check suggested filename");
  }
  // Leave the field editable even if all default names are in use.
  return file;
}

void SaveClipboardFile(const ClipboardFile &file, const std::string &name) {
  FD directory(open(file.folder.c_str(), O_RDONLY | O_DIRECTORY));
  auto verifyFolder = [&] {
    struct stat current;
    if (directory.value < 0 || stat(file.folder.c_str(), &current) ||
        current.st_dev != file.destination.device ||
        current.st_ino != file.destination.inode)
      throw std::runtime_error(
          "The destination folder changed. Check the saved file before retrying.");
  };
  struct stat pinned;
  if (directory.value < 0 || fstat(directory.value, &pinned) ||
      pinned.st_dev != file.destination.device ||
      pinned.st_ino != file.destination.inode)
    throw std::runtime_error(
        "The destination folder changed. Start the save again.");
  verifyFolder();
  SaveExclusiveFile(directory.value, name, 0600, [&](int fd) {
    WriteOutputBytes(fd, file.bytes.data(), file.bytes.size());
  }, verifyFolder);
}
}
