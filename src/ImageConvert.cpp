// SPDX-License-Identifier: MIT
#include "ImageConvert.h"
#include "ExclusiveFileOutput.h"
#include "LeafName.h"

#include <DataIO.h>
#include <Bitmap.h>
#include <ByteOrder.h>
#include <cmath>
#include <Message.h>
#include <MimeType.h>
#include <TranslatorFormats.h>
#include <TranslatorRoster.h>
#include <TypeConstants.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fs_attr.h>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace Listi {
namespace {
constexpr off_t kLimit = 256 * 1024 * 1024;
struct ConversionCancelled {};
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

bool Matches(const struct stat &st, const NodeIdentity &identity) {
  return st.st_dev == identity.device && st.st_ino == identity.inode;
}

class ConversionIO : public BPositionIO {
  int fd;
  const Hooks &hooks;
  bool writable;
  off_t position = 0, length;

  bool Proceed() const { return !hooks.proceed || hooks.proceed(); }

public:
  ConversionIO(int fd, const Hooks &hooks, off_t length, bool writable = false)
      : fd(fd), hooks(hooks), writable(writable), length(length) {}
  off_t Length() const { return length; }
  ssize_t ReadAt(off_t offset, void *data, size_t size) override {
    if (!Proceed())
      return B_CANCELED;
    if (offset < 0)
      return B_BAD_VALUE;
    if (offset >= length)
      return 0;
    ssize_t result = pread(fd, data, std::min<size_t>(size, length - offset),
                           offset);
    return result < 0 ? errno : result;
  }
  ssize_t WriteAt(off_t offset, const void *data, size_t size) override {
    if (!writable)
      return B_NOT_ALLOWED;
    if (!Proceed())
      return B_CANCELED;
    if (offset < 0 || offset > kLimit ||
        size > static_cast<size_t>(kLimit - offset))
      return B_DEVICE_FULL;
    ssize_t result = pwrite(fd, data, size, offset);
    if (result < 0)
      return errno;
    length = std::max(length, offset + result);
    return result;
  }
  off_t Seek(off_t offset, uint32 mode) override {
    if (mode != SEEK_SET && mode != SEEK_CUR && mode != SEEK_END)
      return B_BAD_VALUE;
    off_t base = mode == SEEK_SET ? 0 : mode == SEEK_CUR ? position : length;
    if (offset < -base || offset > kLimit - base)
      return B_BAD_VALUE;
    position = base + offset;
    return position;
  }
  off_t Position() const override { return position; }
  status_t SetSize(off_t size) override {
    if (!writable)
      return B_NOT_ALLOWED;
    if (!Proceed())
      return B_CANCELED;
    if (size < 0 || size > kLimit)
      return B_DEVICE_FULL;
    if (ftruncate(fd, size))
      return errno;
    length = size;
    return B_OK;
  }
};

void Check(const Hooks &hooks) {
  if (hooks.proceed && !hooks.proceed())
    throw ConversionCancelled{};
}

void Require(status_t status, const char *message) {
  if (status != B_OK)
    throw std::runtime_error(std::string(message) + ": " + strerror(status));
}

struct TempFile {
  std::unique_ptr<FILE, decltype(&fclose)> file{tmpfile(), fclose};
  int Descriptor() const {
    if (!file)
      throw std::runtime_error("Could not create temporary conversion data.");
    return fileno(file.get());
  }
};

struct Input {
  const std::string &path;
  const NodeIdentity &identity;
  FD fd;
  struct stat snapshot{};

  explicit Input(const Job &job)
      : path(job.sources.front()), identity(job.expectedSources.at(path)),
        fd(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW)) {
    if (fd.value < 0 || fstat(fd.value, &snapshot) ||
        !S_ISREG(snapshot.st_mode) || !Matches(snapshot, identity))
      throw std::runtime_error("The dropped image changed or is unavailable.");
    if (snapshot.st_size <= 0 || snapshot.st_size > kLimit)
      throw std::runtime_error(
          "Image conversion requires a nonempty file up to 256 MiB.");
  }

  void Verify() const {
    struct stat now;
    if (lstat(path.c_str(), &now) || !Matches(now, identity) ||
        now.st_size != snapshot.st_size ||
        now.st_mtim.tv_sec != snapshot.st_mtim.tv_sec ||
        now.st_mtim.tv_nsec != snapshot.st_mtim.tv_nsec ||
        now.st_ctim.tv_sec != snapshot.st_ctim.tv_sec ||
        now.st_ctim.tv_nsec != snapshot.st_ctim.tv_nsec)
      throw std::runtime_error("The dropped image changed during conversion.");
  }
};

translator_info Decode(BTranslatorRoster &roster, ConversionIO &input,
                       ConversionIO &bitmap, const Hooks &hooks) {
  translator_info format{};
  auto status = roster.Identify(&input, nullptr, &format, 0, nullptr,
                                B_TRANSLATOR_BITMAP);
  Check(hooks);
  if (status != B_OK || format.group != B_TRANSLATOR_BITMAP)
    throw std::runtime_error(
        "No installed Haiku image translator recognizes this file.");
  Require(input.Seek(0, SEEK_SET), "Could not rewind the image");
  status =
      roster.Translate(&input, &format, nullptr, &bitmap, B_TRANSLATOR_BITMAP);
  Check(hooks);
  Require(status, "Could not decode the image");
  if (!bitmap.Length())
    throw std::runtime_error("The image translator produced no bitmap.");
  return format;
}

// Convert only alpha-bearing translator bitmaps. Stream one scanline at a time
// into a private scratch file; the user's original is never opened for writing.
bool WhiteMatteForJPEG(ConversionIO &source, ConversionIO &matte, const Hooks &hooks) {
  TranslatorBitmap header;
  if (source.ReadAt(0, &header, sizeof(header)) != sizeof(header))
    throw std::runtime_error("Incomplete decoded bitmap.");
  const auto space = color_space(B_BENDIAN_TO_HOST_INT32(header.colors));
  if (space != B_RGBA32 && space != B_RGBA32_BIG && space != B_CMAP8 &&
      space != B_RGBA15 && space != B_RGBA15_BIG && space != B_RGBA64 &&
      space != B_RGBA64_BIG)
    return false;
  const double width = double(B_BENDIAN_TO_HOST_FLOAT(header.bounds.right)) -
                       B_BENDIAN_TO_HOST_FLOAT(header.bounds.left) + 1;
  const double height = double(B_BENDIAN_TO_HOST_FLOAT(header.bounds.bottom)) -
                        B_BENDIAN_TO_HOST_FLOAT(header.bounds.top) + 1;
  const uint32 stride = B_BENDIAN_TO_HOST_INT32(header.rowBytes);
  if (!(width >= 1 && height >= 1 && width == std::floor(width) &&
        height == std::floor(height) && width * height * 4 <= kLimit - sizeof(header)) ||
      stride == 0 || sizeof(header) + height * stride > source.Length())
    throw std::runtime_error("Invalid or oversized decoded image.");
  BBitmap row(BRect(0, 0, width - 1, 0), B_RGBA32);
  Require(row.InitCheck(), "Could not allocate a conversion row");
  std::vector<uint8> data(stride);
  header.colors = color_space(B_HOST_TO_BENDIAN_INT32(B_RGB32));
  header.rowBytes = B_HOST_TO_BENDIAN_INT32(row.BytesPerRow());
  header.dataSize = B_HOST_TO_BENDIAN_INT32(uint32(height) * row.BytesPerRow());
  if (matte.Write(&header, sizeof(header)) != sizeof(header))
    throw std::runtime_error("Could not stage JPEG background.");
  for (int32 y = 0; y < int32(height); ++y) {
    Check(hooks);
    if (source.ReadAt(sizeof(header) + off_t(y) * stride, data.data(), stride) != stride)
      throw std::runtime_error("Could not read the decoded image row.");
    Require(row.ImportBits(data.data(), stride, stride, 0, space),
            "Could not convert transparent pixels");
    auto *pixel = static_cast<uint8 *>(row.Bits());
    for (int32 x = 0; x < int32(width); ++x, pixel += 4) {
      for (int channel = 0; channel < 3; ++channel)
        pixel[channel] = 255 - ((255 - pixel[channel]) * pixel[3] + 127) / 255;
      pixel[3] = 255;
    }
    if (matte.Write(row.Bits(), row.BytesPerRow()) != row.BytesPerRow())
      throw std::runtime_error("Could not stage JPEG pixels.");
  }
  return true;
}

bool ValidExtension(const std::string &extension) {
  return !extension.empty() && extension.size() <= 16 &&
         extension.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") ==
             std::string::npos;
}

std::string Extension(const std::string &mime) {
  // Prefer familiar canonical suffixes for the system translators.
  static const std::map<std::string, std::string> known = {
      {"image/jpeg", "jpg"}, {"image/png", "png"}, {"image/gif", "gif"},
      {"image/bmp", "bmp"}, {"image/tiff", "tiff"}, {"image/webp", "webp"},
      {"image/x-targa", "tga"}, {"image/x-icon", "ico"},
      {"image/vnd.microsoft.icon", "ico"}, {"image/x-photoshop", "psd"},
      {"image/vnd.adobe.photoshop", "psd"},
      {"image/x-portable-pixmap", "ppm"}, {"image/x-portable-bitmap", "pbm"},
      {"image/x-portable-graymap", "pgm"}, {"image/x-portable-anymap", "pnm"},
      {"image/x-xpixmap", "xpm"}, {"image/x-xbitmap", "xbm"},
      {"image/x-ms-bmp", "bmp"}, {"image/x-hvif", "hvif"},
      {"image/jp2", "jp2"}, {"image/x-pcx", "pcx"},
      {"image/x-exr", "exr"}, {"image/x-hdr", "hdr"}};
  auto found = known.find(mime);
  if (found != known.end())
    return found->second;
  const char *ext = nullptr;
  BMimeType type(mime.c_str());
  BMessage extensions;
  if (type.GetFileExtensions(&extensions) == B_OK) {
    for (int32 i = 0; extensions.FindString("extensions", i, &ext) == B_OK; ++i)
      if (ValidExtension(ext))
        return ext;
  }
  // Translators may be installed before their corresponding MIME type.
  std::string subtype = mime.substr(6);
  if (subtype.compare(0, 2, "x-") == 0)
    subtype.erase(0, 2);
  return ValidExtension(subtype) ? subtype : std::string();
}

struct Encoding {
  ImageOutput output;
  translator_id translator;
};

std::vector<Encoding> FindOutputs(BTranslatorRoster &roster,
                                  const translator_info &input,
                                  ConversionIO &bitmap, const Hooks &hooks) {
  translator_id *ids = nullptr;
  int32 count = 0;
  Require(roster.GetAllTranslators(&ids, &count),
          "Could not enumerate installed image translators");
  std::unique_ptr<translator_id[]> release(ids);
  std::vector<Encoding> choices;
  for (int32 i = 0; i < count; ++i) {
    const translation_format *outputs = nullptr;
    int32 numOutputs = 0;
    if (roster.GetOutputFormats(ids[i], &outputs, &numOutputs) != B_OK ||
        !outputs || numOutputs <= 0)
      continue;
    // The roster may rescan add-ons on the next query.
    std::vector<translation_format> installed(outputs, outputs + numOutputs);
    for (const auto &format : installed) {
      Check(hooks);
      if (format.group != B_TRANSLATOR_BITMAP ||
          format.type == B_TRANSLATOR_BITMAP || format.type == input.type ||
          strcmp(format.MIME, input.MIME) == 0 ||
          strncmp(format.MIME, "image/", 6) != 0)
        continue;
      if (std::any_of(choices.begin(), choices.end(), [&](const auto &output) {
            return output.output.mime == format.MIME;
          }))
        continue;
      std::string extension = Extension(format.MIME);
      if (extension.empty())
        continue;
      translator_info *matches = nullptr;
      int32 numMatches = 0;
      auto status = roster.GetTranslators(&bitmap, nullptr, &matches,
                                          &numMatches, B_TRANSLATOR_BITMAP,
                                          nullptr, format.type);
      std::unique_ptr<translator_info[]> releaseMatches(matches);
      if (status != B_OK)
        continue;
      for (int32 match = 0; match < numMatches; ++match) {
        if (matches[match].translator == ids[i] &&
            matches[match].type == B_TRANSLATOR_BITMAP) {
          choices.push_back({{format.type, format.MIME, format.name, extension},
                             ids[i]});
          break;
        }
      }
    }
  }
  std::sort(choices.begin(), choices.end(), [](const auto &a, const auto &b) {
    return a.output.name < b.output.name;
  });
  return choices;
}

void VerifyFolder(const Job &job, int directory) {
  struct stat pinned, current;
  if (directory < 0 || fstat(directory, &pinned) || !S_ISDIR(pinned.st_mode) ||
      !Matches(pinned, job.expectedDestination) ||
      lstat(job.destination.c_str(), &current) ||
      !Matches(current, job.expectedDestination))
    throw std::runtime_error(
        "The drop destination changed. Drop the image again.");
}
} // namespace

bool CanProbeImageDrop(const Job &job) {
  if (job.sources.size() != 1 || job.sources.front().empty() ||
      job.sources.front()[0] != '/' || job.destination.empty() ||
      job.destination[0] != '/' || job.expectedDestination.device < 0)
    return false;
  const auto &path = job.sources.front();
  auto expected = job.expectedSources.find(path);
  struct stat st;
  return expected != job.expectedSources.end() && lstat(path.c_str(), &st) == 0 &&
         S_ISREG(st.st_mode) && Matches(st, expected->second);
}

std::vector<ImageOutput> ImageOutputs(const Job &job) {
  if (!CanProbeImageDrop(job))
    throw std::runtime_error("Drop a single local image file to convert it.");
  const Hooks hooks;
  Input source(job);
  TempFile scratch;
  ConversionIO input(source.fd.value, hooks, source.snapshot.st_size);
  ConversionIO bitmap(scratch.Descriptor(), hooks, 0, true);
  BTranslatorRoster roster;
  Require(roster.AddTranslators(), "Could not load installed image translators");
  auto identified = Decode(roster, input, bitmap, hooks);
  source.Verify();
  std::vector<ImageOutput> outputs;
  for (auto &format : FindOutputs(roster, identified, bitmap, hooks))
    outputs.push_back(std::move(format.output));
  return outputs;
}

std::string ImageOutputName(const std::string &source,
                            const std::string &extension) {
  std::string name = Leaf(source);
  auto dot = name.find_last_of('.');
  if (dot != std::string::npos && dot != 0)
    name.resize(dot);
  return SuffixedLeafName(name, "." + extension);
}

Result RunImageConversion(const Job &job, const Hooks &hooks) {
  Result result;
  try {
    Check(hooks);
    if (!CanProbeImageDrop(job))
      throw std::runtime_error("The dropped image changed or is unavailable.");
    FD folder(open(job.destination.c_str(), O_RDONLY | O_NOFOLLOW | O_DIRECTORY));
    VerifyFolder(job, folder.value);
    Input source(job);
    ConversionIO input(source.fd.value, hooks, source.snapshot.st_size);
    TempFile decodedFile, encodedFile;
    ConversionIO bitmap(decodedFile.Descriptor(), hooks, 0, true);
    ConversionIO encoded(encodedFile.Descriptor(), hooks, 0, true);
    BTranslatorRoster roster;
    Require(roster.AddTranslators(), "Could not load installed image translators");
    auto identified = Decode(roster, input, bitmap, hooks);
    auto choices = FindOutputs(roster, identified, bitmap, hooks);
    auto choice =
        std::find_if(choices.begin(), choices.end(), [&](const auto &encoding) {
          return encoding.output.type == job.imageFormat &&
                 encoding.output.mime == job.imageMime;
        });
    if (choice == choices.end())
      throw std::runtime_error("The chosen image output is no longer available.");
    std::unique_ptr<TempFile> matteFile;
    std::unique_ptr<ConversionIO> matte;
    ConversionIO *pixels = &bitmap;
    if (job.imageMime == "image/jpeg") {
      matteFile = std::make_unique<TempFile>();
      matte = std::make_unique<ConversionIO>(matteFile->Descriptor(), hooks, 0, true);
      if (WhiteMatteForJPEG(bitmap, *matte, hooks))
        pixels = matte.get();
    }
    Require(pixels->Seek(0, SEEK_SET), "Could not rewind the decoded image");
    auto status = roster.Translate(choice->translator, pixels, nullptr,
                                   &encoded, choice->output.type);
    Check(hooks);
    Require(status, "Could not encode the selected image format");
    if (!encoded.Length())
      throw std::runtime_error("The image translator produced an empty file.");
    source.Verify();
    VerifyFolder(job, folder.value);
    auto leaf = ImageOutputName(source.path, choice->output.extension);
    auto destination = Join(job.destination, leaf);
    // BFS cannot publish a staged file with hard links or atomic no-replace
    // rename. Reserve the final leaf exclusively, then copy the staged bytes.
    const mode_t outputMode = source.snapshot.st_mode & 0666;
    SaveExclusiveFile(folder.value, leaf, outputMode, [&](int fd) {
      std::array<char, 64 * 1024> buffer;
      for (off_t offset = 0; offset < encoded.Length();) {
        Check(hooks);
        const ssize_t n = encoded.ReadAt(offset, buffer.data(), buffer.size());
        if (n <= 0)
          throw std::runtime_error("Could not read the staged converted image.");
        WriteOutputBytes(fd, buffer.data(), n);
        offset += n;
      }
      // Filesystems without attributes still receive a valid image and suffix.
      fs_write_attr(fd, "BEOS:TYPE", B_MIME_STRING_TYPE, 0,
                    choice->output.mime.c_str(), choice->output.mime.size() + 1);
      Check(hooks);
      VerifyFolder(job, folder.value);
      source.Verify();
    }, [&] { VerifyFolder(job, folder.value); });
    ++result.completed;
    result.completedRoots.emplace_back(source.path, destination);
    if (hooks.fileDone)
      hooks.fileDone(source.path, destination);
  } catch (const ConversionCancelled &) {
    result.cancelled = true;
  } catch (const std::exception &error) {
    result.errors.push_back(error.what());
    if (hooks.fileFailed && !job.sources.empty())
      hooks.fileFailed(job.sources.front(), error.what());
  }
  return result;
}
}
