// SPDX-License-Identifier: MIT
#include "ClipboardTextFile.h"
#include <Application.h>
#include <Bitmap.h>
#include <DataIO.h>
#include <TranslationUtils.h>
#include <memory>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

using namespace Listi;

namespace {
int failWrite = 0;
bool failOutputClose = false, failDirectorySync = false;
std::string swapOutput, movedOutput;
struct Fixture {
  char folder[64] = "/boot/home/Listi-clipboard-text-test-XXXXXX";
  Fixture() {
    if (!mkdtemp(folder))
      throw std::runtime_error("Cannot create fixture.");
  }
  ~Fixture() {
    std::string root(folder);
    for (const char *name :
         {"Clipboard.txt", "note.txt", "link", "partial.txt", "image.png",
          "replaced.txt", "moved.txt", "close.txt", "dir-sync.txt"})
      unlink((root + "/" + name).c_str());
    rmdir((root + "/destination").c_str());
    rmdir((root + "/old-destination").c_str());
    rmdir(folder);
  }
};
void Check(bool passed, const char *description) {
  if (!passed)
    throw std::runtime_error(description);
  printf("PASS %s\n", description);
}
template <class F> bool Rejected(F action) {
  try {
    action();
    return false;
  } catch (const std::exception &) {
    return true;
  }
}
std::string Read(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}
void Set(BClipboard &clipboard, const BMessage &contents) {
  if (!clipboard.Lock())
    throw std::runtime_error("Cannot lock test clipboard.");
  *clipboard.Data() = contents;
  auto status = clipboard.Commit(true);
  clipboard.Unlock();
  if (status != B_OK)
    throw std::runtime_error("Cannot populate test clipboard.");
}
BMessage Text(const std::string &bytes) {
  BMessage message;
  if (message.AddData("text/plain", B_MIME_TYPE, bytes.data(), bytes.size()) !=
      B_OK)
    throw std::runtime_error("Cannot add test clipboard text.");
  return message;
}
}

extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" ssize_t __wrap_write(int fd, const void *buffer, size_t size) {
  if (!swapOutput.empty()) {
    auto original = std::move(swapOutput);
    swapOutput.clear();
    if (rename(original.c_str(), movedOutput.c_str()) != 0)
      return -1;
    std::ofstream(original) << "replacement";
  }
  if (failWrite == 0)
    return __real_write(fd, buffer, size);
  if (failWrite++ == 1)
    return __real_write(fd, buffer, 1);
  errno = ENOSPC;
  return -1;
}

extern "C" int __real_close(int);
extern "C" int __wrap_close(int fd) {
  if (failOutputClose) {
    struct stat st;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode)) {
      failOutputClose = false;
      __real_close(fd);
      errno = EIO;
      return -1;
    }
  }
  return __real_close(fd);
}

extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  if (failDirectorySync) {
    struct stat st;
    if (!fstat(fd, &st) && S_ISDIR(st.st_mode)) {
      failDirectorySync = false;
      errno = EIO;
      return -1;
    }
  }
  return __real_fsync(fd);
}

int main(int argc, char **argv) {
  BApplication app("application/x-vnd.Kambuzi-ListiClipboardTextFileTest");
  try {
    // Seed real GUI clipboard without exposing it in the default audit.
    if (argc == 3 && std::strcmp(argv[1], "--seed-text") == 0) {
      Set(*be_clipboard, Text(argv[2]));
      return 0;
    }
    BBitmap native(BRect(0, 0, 1, 1), B_RGBA32);
    Check(native.InitCheck() == B_OK, "create native bitmap");
    const unsigned char pixel[] = {12, 34, 56, 255};
    for (int i = 0; i < native.BitsLength(); i += 4)
      memcpy(static_cast<char *>(native.Bits()) + i, pixel, 4);
    BMessage archiveBitmap, bitmap;
    Check(native.Archive(&archiveBitmap) == B_OK &&
              bitmap.AddMessage("image/bitmap", &archiveBitmap) == B_OK,
          "build ShowImage-style bitmap clipboard");
    if (argc == 2 && std::strcmp(argv[1], "--seed-image") == 0) {
      Set(*be_clipboard, bitmap);
      return 0;
    }
    Fixture fixture;
    const std::string folder(fixture.folder);
    const char payload[] = "echo do not run\nUTF-8: æøå\n\0trailing bytes";
    const std::string original(payload, sizeof(payload) - 1);
    auto text = Text(original);
    BClipboard clipboard("listi-clipboard-text-file-test");
    Set(clipboard, text);
    Check(CanSaveClipboardIn(folder) && !CanSaveClipboardIn("@disks") &&
              !CanSaveClipboardIn(folder + "/missing"),
          "only available local destinations are allowed");
    const auto textContent = DetectClipboardSaveContent(&clipboard);
    Check(textContent == ClipboardContent::Text, "direct Paste detects text");
    auto first = PrepareClipboardFile(folder, *textContent, &clipboard);
    Check(first.suggestedName == "Clipboard.txt", "default text filename");
    Check(access((folder + "/" + first.suggestedName).c_str(), F_OK) != 0 &&
              errno == ENOENT,
          "preparing or dismissing the filename prompt writes nothing");
    Check(Rejected([&] {
            PrepareClipboardFile("sftp://example.invalid/", *textContent,
                                 &clipboard);
          }),
          "direct Paste of content does not target virtual folders");
    std::ofstream(folder + "/Clipboard.txt") << "existing";
    auto frozen =
        PrepareClipboardFile(folder, ClipboardContent::Text, &clipboard);
    Check(frozen.suggestedName == "Clipboard 2.txt",
          "suggest an unused filename");
    Set(clipboard, Text("newer clipboard data"));
    auto clipboardCount = clipboard.SystemCount();
    SaveClipboardFile(frozen, "note.txt");
    Check(Read(folder + "/note.txt") == original &&
              clipboard.SystemCount() == clipboardCount,
          "save literal frozen bytes without changing clipboard");
    Check(Rejected([&] { SaveClipboardFile(first, "Clipboard.txt"); }) &&
              Read(folder + "/Clipboard.txt") == "existing",
          "a later collision never overwrites a file");
    Check(symlink("missing-target", (folder + "/link").c_str()) == 0 &&
              Rejected([&] { SaveClipboardFile(frozen, "link"); }),
          "a dangling symlink is never followed or replaced");
    Check(Rejected([&] { SaveClipboardFile(frozen, "../escape"); }) &&
              Rejected([&] { SaveClipboardFile(frozen, "."); }),
          "the name must be a single leaf");
    const auto destination = folder + "/destination";
    const auto old = folder + "/old-destination";
    Check(mkdir(destination.c_str(), 0700) == 0, "create destination fixture");
    auto stale =
        PrepareClipboardFile(destination, ClipboardContent::Text, &clipboard);
    Check(rename(destination.c_str(), old.c_str()) == 0 &&
              mkdir(destination.c_str(), 0700) == 0 &&
              Rejected([&] { SaveClipboardFile(stale, "unexpected.txt"); }),
          "a replaced destination is rejected");
    failWrite = 1;
    bool rejected = Rejected([&] { SaveClipboardFile(frozen, "partial.txt"); });
    failWrite = 0;
    struct stat partial;
    Check(rejected && lstat((folder + "/partial.txt").c_str(), &partial) < 0 &&
              errno == ENOENT,
          "failed partial write leaves no output");
    failOutputClose = true;
    Check(Rejected([&] { SaveClipboardFile(frozen, "close.txt"); }) &&
              !failOutputClose &&
              lstat((folder + "/close.txt").c_str(), &partial) < 0 &&
              errno == ENOENT,
          "file close error is reported and removes the incomplete output");
    failDirectorySync = true;
    Check(Rejected([&] { SaveClipboardFile(frozen, "dir-sync.txt"); }) &&
              !failDirectorySync && Read(folder + "/dir-sync.txt") == original,
          "directory flush error is reported without deleting completed bytes");
    swapOutput = folder + "/replaced.txt";
    movedOutput = folder + "/moved.txt";
    Check(Rejected([&] { SaveClipboardFile(frozen, "replaced.txt"); }) &&
              Read(folder + "/replaced.txt") == "replacement" &&
              Read(movedOutput) == original,
          "output replaced during writing is preserved, not reported as a successful save");
    BMessage refs = MakeFileSelection({folder + "/Clipboard.txt"}, true);
    Set(clipboard, refs);
    Check(Rejected([&] {
            PrepareClipboardFile(folder, ClipboardContent::Text, &clipboard);
          }),
          "native file references are not text");
    refs.AddData("text/plain", B_MIME_TYPE, original.data(), original.size());
    Set(clipboard, refs);
    Check(!DetectClipboardSaveContent(&clipboard),
          "direct Paste keeps file Cut ahead of clipboard text");
    Check(Rejected([&] {
            PrepareClipboardFile(folder, ClipboardContent::Text, &clipboard);
          }),
          "mixed file and text clipboard is not converted");
    BMessage archive;
    text.AddMessage("listi:archive-copy", &archive);
    Set(clipboard, text);
    Check(!DetectClipboardSaveContent(&clipboard),
          "direct Paste keeps archive selections ahead of clipboard text");
    Check(Rejected([&] {
            PrepareClipboardFile(folder, ClipboardContent::Text, &clipboard);
          }),
          "archive selection is not converted");
    Set(clipboard, BMessage());
    Check(Rejected([&] { DetectClipboardSaveContent(&clipboard); }),
          "direct Paste rejects an empty clipboard");
    Check(Rejected([&] {
            PrepareClipboardFile(folder, ClipboardContent::Text, &clipboard);
          }),
          "empty clipboard is rejected");
    Set(clipboard, bitmap);
    const auto imageContent = DetectClipboardSaveContent(&clipboard);
    Check(imageContent == ClipboardContent::Image,
          "direct Paste detects a native clipboard bitmap");
    const auto image = PrepareClipboardFile(folder, *imageContent, &clipboard);
    Check(image.suggestedName == "Clipboard.png", "default PNG filename");
    Set(clipboard, Text("later"));
    SaveClipboardFile(image, "image.png");
    auto saved = Read(folder + "/image.png");
    BMemoryIO input(saved.data(), saved.size());
    std::unique_ptr<BBitmap> decoded(BTranslationUtils::GetBitmap(&input));
    Check(decoded && decoded->Bounds() == native.Bounds() &&
              !memcmp(decoded->Bits(), pixel, sizeof(pixel)),
          "native image saved as PNG with original dimensions and pixel");
    BMessage png;
    png.AddData("image/png", B_MIME_TYPE, saved.data(), saved.size());
    png.AddData("text/plain", B_MIME_TYPE, original.data(), original.size());
    Set(clipboard, png);
    Check(DetectClipboardSaveContent(&clipboard) == ClipboardContent::Image,
          "direct Paste prefers PNG over accompanying text");
    Check(PrepareClipboardFile(folder, ClipboardContent::Image, &clipboard)
                  .bytes == saved,
          "encoded PNG clipboard data preserved unchanged");
    refs.AddData("image/png", B_MIME_TYPE, saved.data(), saved.size());
    Set(clipboard, refs);
    Check(!DetectClipboardSaveContent(&clipboard),
          "direct Paste keeps file Cut ahead of clipboard images");
    Check(Rejected([&] {
            PrepareClipboardFile(folder, ClipboardContent::Image, &clipboard);
          }),
          "file references are not images");
    BMessage invalid;
    invalid.AddData("image/png", B_MIME_TYPE, "not PNG", 7);
    Set(clipboard, invalid);
    Check(Rejected([&] {
            PrepareClipboardFile(folder, ClipboardContent::Image, &clipboard);
          }),
          "invalid PNG clipboard is rejected");
    archiveBitmap.ReplaceInt32("_rowbytes", 1);
    BMessage badBitmap;
    badBitmap.AddMessage("image/bitmap", &archiveBitmap);
    Set(clipboard, badBitmap);
    Check(Rejected([&] {
            PrepareClipboardFile(folder, ClipboardContent::Image, &clipboard);
          }),
          "inconsistent native bitmap size is rejected");
    saved[16] = 127;
    BMessage huge;
    huge.AddData("image/png", B_MIME_TYPE, saved.data(), saved.size());
    Set(clipboard, huge);
    Check(Rejected([&] {
            PrepareClipboardFile(folder, ClipboardContent::Image, &clipboard);
          }),
          "oversized declared PNG dimensions are rejected");
  } catch (const std::exception &e) {
    fprintf(stderr, "FAIL %s\n", e.what());
    return 1;
  }
  return 0;
}
