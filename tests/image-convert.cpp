// SPDX-License-Identifier: MIT
#include "FileDrop.h"
#include "ImageConvert.h"
#include <Application.h>
#include <StorageDefs.h>
#include <Bitmap.h>
#include <TranslationUtils.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
// A black PNG with zero alpha; merely dropping alpha would produce black JPEG pixels.
const unsigned char kPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0xf3, 0xff, 0x61, 0x00, 0x00, 0x00,
    0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x18, 0x05, 0xa3,
    0x60, 0x14, 0x8c, 0x02, 0x08, 0x00, 0x00, 0x04, 0x10, 0x00, 0x01, 0xaf,
    0x45, 0x88, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
    0x42, 0x60, 0x82
};

std::string Read(const std::string &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}

void Write(const std::string &path, const std::string &contents) {
  std::ofstream stream(path, std::ios::binary);
  stream << contents;
}

bool Exists(const std::string &path) {
  struct stat st;
  return lstat(path.c_str(), &st) == 0;
}

void Check(bool passed, const char *description) {
  std::cout << (passed ? "PASS " : "FAIL ") << description << std::endl;
  if (!passed)
    throw std::runtime_error(description);
}
}

int main() {
  BApplication app("application/x-vnd.Kambuzi-Listi-ImageConvertTest");
  char scratch[] = "/boot/home/Listi-convert-XXXXXX";
  if (!mkdtemp(scratch))
    return 2;
  int status = 0;
  try {
    using namespace Listi;
    const std::string root(scratch);
    const auto source = Join(root, "original misleading.txt");
    const auto destination = Join(root, "destination");
    const auto png = std::string(reinterpret_cast<const char *>(kPng),
                                 sizeof(kPng));
    Write(source, png);
    mkdir(destination.c_str(), 0700);

    auto drag = MakeFileDrag({source});
    auto job = PrepareFileDrop(drag, destination);
    Check(CanProbeImageDrop(job), "single guarded regular file is eligible");
    auto outputs = ImageOutputs(job);
    auto jpeg = std::find_if(outputs.begin(), outputs.end(), [](const auto &out) {
      return out.mime == "image/jpeg" && out.extension == "jpg";
    });
    Check(jpeg != outputs.end(),
          "native JPEG encoder offered for PNG with misleading suffix");
    Check(std::none_of(outputs.begin(), outputs.end(), [](const auto &out) {
            return out.mime == "image/png" || out.mime == "image/x-be-bitmap";
          }), "source format and intermediate Be bitmap are not offered");
    Check(ImageOutputName("/tmp/.hidden", "jpg") == ".hidden.jpg",
          "hidden filename stem is preserved");

    job.action = Action::ConvertImage;
    job.imageFormat = jpeg->type;
    job.imageMime = jpeg->mime;
    const auto target = Join(destination, "original misleading.jpg");
    auto converted = Run(job, {});
    Check(converted.completed == 1 && Read(target).compare(0, 2, "\xff\xd8") == 0 &&
              Read(source) == png,
          "queue operation creates genuine JPEG and leaves source unchanged");
    std::unique_ptr<BBitmap> jpg(BTranslationUtils::GetBitmap(target.c_str()));
    Check(jpg && jpg->InitCheck() == B_OK, "JPEG is decodable");
    BBitmap rgba(jpg->Bounds(), B_RGBA32);
    Check(rgba.ImportBits(jpg.get()) == B_OK, "read JPEG pixels in RGBA form");
    auto *pixel = static_cast<const uint8 *>(rgba.Bits());
    Check(pixel[0] >= 250 && pixel[1] >= 250 && pixel[2] >= 250,
          "transparent PNG pixels appear white in the converted JPEG");


    // The source fits the filesystem, but appending ".jpg" would split Ø
    // at the byte limit. Keep the entire suffix and only whole characters.
    const std::string prefix(B_FILE_NAME_LENGTH - 6, 'a');
    const auto longSource = Join(root, prefix + "Øxyz");
    const auto longTarget = Join(destination, prefix + ".jpg");
    Write(longSource, png);
    auto longJob = PrepareFileDrop(MakeFileDrag({longSource}), destination);
    longJob.action = job.action;
    longJob.imageFormat = jpeg->type;
    longJob.imageMime = jpeg->mime;
    Check(Run(longJob, {}).completed == 1 &&
              Read(longTarget).compare(0, 2, "\xff\xd8") == 0 && Read(longSource) == png,
          "conversion fits its extension without splitting a UTF-8 filename");
    Write(longTarget, "preserve this file");
    Check(Run(longJob, {}).completed == 0 && Read(longTarget) == "preserve this file",
          "collision after shortening the output name preserves the existing file");

    Write(target, "existing target");
    auto collision = Run(job, {});
    Check(collision.completed == 0 && !collision.errors.empty() &&
              Read(target) == "existing target" && Read(source) == png,
          "existing file is never overwritten");
    unlink(target.c_str());
    symlink("missing", target.c_str());
    struct stat link;
    auto linkCollision = Run(job, {});
    Check(linkCollision.completed == 0 && !linkCollision.errors.empty() &&
              lstat(target.c_str(), &link) == 0 && S_ISLNK(link.st_mode),
          "existing dangling symlink is never overwritten");
    unlink(target.c_str());

    Hooks cancel;
    cancel.proceed = [&] { return !Exists(target); };
    auto cancelled = Run(job, cancel);
    Check(cancelled.cancelled && !Exists(target) && Read(source) == png,
          "cancellation after exclusive creation removes partial output");

    bool replaced = false;
    Hooks replaceOutput;
    replaceOutput.proceed = [&] {
      if (!replaced && Exists(target)) {
        replaced = true;
        unlink(target.c_str());
        Write(target, "user replacement");
      }
      return true;
    };
    auto interrupted = Run(job, replaceOutput);
    Check(replaced && !interrupted.completed && !interrupted.errors.empty() &&
              Read(target) == "user replacement",
          "a replaced output survives and is not reported as a converted image");
    unlink(target.c_str());

    // An in-place editor or synchronizer can retain the source size and
    // restore its modification time. The conversion still describes old bytes.
    struct stat beforeEdit;
    Check(stat(source.c_str(), &beforeEdit) == 0, "snapshot original image times");
    const std::string updated(png.size(), 'x');
    bool edited = false;
    Hooks editSource;
    editSource.proceed = [&] {
      if (!edited && Exists(target)) {
        Write(source, updated);
        const struct timespec times[] = {beforeEdit.st_atim, beforeEdit.st_mtim};
        Check(utimensat(AT_FDCWD, source.c_str(), times, 0) == 0,
              "restore modification time after in-place edit");
        edited = true;
      }
      return true;
    };
    auto staleBytes = Run(job, editSource);
    Check(edited && !staleBytes.completed && !staleBytes.errors.empty() &&
              !Exists(target) && Read(source) == updated,
          "conversion refuses edited source even with its size and modification time restored");
    Write(source, png);

    auto sameFolder = PrepareFileDrop(drag, root, true);
    sameFolder.action = job.action;
    sameFolder.imageFormat = job.imageFormat;
    sameFolder.imageMime = job.imageMime;
    Check(Run(sameFolder, {}).completed == 1 &&
              Read(Join(root, "original misleading.jpg")).compare(0, 2, "\xff\xd8") == 0 &&
              Read(source) == png,
          "same-folder conversion creates a differently named sibling");

    const auto text = Join(root, "plain.png");
    Write(text, "not an image");
    bool unsupported = false;
    try {
      unsupported = ImageOutputs(PrepareFileDrop(MakeFileDrag({text}), destination)).empty();
    } catch (const std::runtime_error &) {
      unsupported = true;
    }
    Check(unsupported, "unsupported contents do not offer output formats");

    auto virtualTarget = PrepareFileDrop(drag, "sftp://example.invalid/target");
    Check(!CanProbeImageDrop(virtualTarget),
          "virtual destinations do not offer local conversion");

    rename(destination.c_str(), Join(root, "previous destination").c_str());
    mkdir(destination.c_str(), 0700);
    auto staleFolder = Run(job, {});
    Check(staleFolder.completed == 0 && !staleFolder.errors.empty() &&
              !Exists(target),
          "replacement destination is not written");

    auto staleSource = PrepareFileDrop(drag, destination);
    staleSource.action = job.action;
    staleSource.imageFormat = job.imageFormat;
    staleSource.imageMime = job.imageMime;
    rename(source.c_str(), Join(root, "previous source").c_str());
    Write(source, png);
    auto changed = Run(staleSource, {});
    Check(changed.completed == 0 && !changed.errors.empty() && !Exists(target),
          "replacement source with identical content is not converted");
  } catch (const std::exception &error) {
    std::cerr << error.what() << std::endl;
    status = 1;
  }
  std::error_code cleanup;
  std::filesystem::remove_all(scratch, cleanup);
  return status;
}
