// SPDX-License-Identifier: MIT
#include "ImagePreview.h"
#include "InspectorContent.h"
#include "VirtualFile.h"
#include <Application.h>
#include <Bitmap.h>
#include <cassert>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
void Write(const std::string &path, const std::string &contents) {
  std::ofstream stream(path, std::ios::binary);
  stream.write(contents.data(), contents.size());
  stream.close();
  assert(stream);
}

std::string PdfFixture() {
  const std::string red = "1 0 0 rg 0 0 80 60 re f\n";
  const std::string blue = "0 0 1 rg 0 0 80 60 re f\n";
  const std::vector<std::string> objects{
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 80 60] /Contents 5 0 R >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 80 60] /Contents 6 0 R >>",
      "<< /Length " + std::to_string(red.size()) + " >>\nstream\n" + red + "endstream",
      "<< /Length " + std::to_string(blue.size()) + " >>\nstream\n" + blue + "endstream"};
  std::ostringstream file;
  file << "%PDF-1.4\n";
  int index = 0;
  std::vector<std::streamoff> offsets;
  for (const auto &object : objects) {
    offsets.push_back(file.tellp());
    file << ++index << " 0 obj\n" << object << "\nendobj\n";
  }
  const auto xref = file.tellp();
  file << "xref\n0 7\n0000000000 65535 f \n";
  for (const auto offset : offsets)
    file << std::setw(10) << std::setfill('0') << offset << " 00000 n \n";
  file << "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";
  return file.str();
}

void AppendLE(std::string &output, uint32 value, size_t count) {
  for (size_t i = 0; i < count; ++i)
    output.push_back(char((value >> (8 * i)) & 0xff));
}

std::string WaveFixture() {
  const uint32 sampleRate = 44100, dataBytes = sampleRate * 2 * 2 / 5;
  std::string wave("RIFF");
  AppendLE(wave, 36 + dataBytes, 4);
  wave += "WAVEfmt ";
  AppendLE(wave, 16, 4);
  AppendLE(wave, 1, 2);
  AppendLE(wave, 2, 2);
  AppendLE(wave, sampleRate, 4);
  AppendLE(wave, sampleRate * 4, 4);
  AppendLE(wave, 4, 2);
  AppendLE(wave, 16, 2);
  wave += "data";
  AppendLE(wave, dataBytes, 4);
  for (uint32 i = 0; i < dataBytes / 2; ++i)
    AppendLE(wave, i % 200 < 100 ? 1500 : -1500, 2);
  return wave;
}

// Mono, 16-bit AIFF at 44100 Hz. No filename or MIME hint is needed.
std::string AiffFixture() {
  constexpr uint32 frames = 4410;
  std::string bytes = "FORM";
  auto bigEndian = [&](uint32 value, int width) {
    for (int i = width - 1; i >= 0; --i)
      bytes.push_back(static_cast<char>(value >> (8 * i)));
  };
  bigEndian(46 + frames * 2, 4);
  bytes += "AIFFCOMM";
  bigEndian(18, 4);
  bigEndian(1, 2);
  bigEndian(frames, 4);
  bigEndian(16, 2);
  bytes.append("\x40\x0e\xac\x44\0\0\0\0\0\0", 10); // 80-bit sample rate
  bytes += "SSND";
  bigEndian(8 + frames * 2, 4);
  bytes.append(8 + frames * 2, '\0'); // Zero offset, block size and sample data.
  return bytes;
}

void TestText(const std::string &path) {
  const std::string html = "<script>window.alert('not run')</script>\n";
  Write(path, "\xef\xbb\xbf" + html + "Æøå\r\nlast\r");
  {
    Inspector::Input input(path);
    bool truncated = true;
    assert(Inspector::Identify("inline.HTML", input) == Inspector::Kind::Html);
    assert(Inspector::Identify("README.md", input) == Inspector::Kind::Markdown);
    assert(Inspector::Identify("code.cpp", input) == Inspector::Kind::Code);
    assert(Inspector::Identify("main.ts", input) == Inspector::Kind::Code);
    assert(Inspector::Identify("note.txt", input) == Inspector::Kind::Text);
    assert(Inspector::Identify("note.custom-format", input) == Inspector::Kind::Unknown);
    assert(Inspector::Identify("details", input) == Inspector::Kind::Text);
    assert(Inspector::ReadText(input, truncated) == html + "Æøå\nlast\n");
    assert(!truncated);
  }
  Write(path, "RIFF");
  {
    Inspector::Input input(path);
    assert(Inspector::Identify("note", input) == Inspector::Kind::Text);
  }
  Write(path, std::string(256 * 1024 - 1, 'a') + "Æ\n");
  {
    Inspector::Input input(path);
    bool truncated = false;
    assert(Inspector::ReadText(input, truncated).size() == 256 * 1024 - 1);
    assert(truncated);
  }
  // A complete 1024-byte file is not a deliberately truncated probe.
  Write(path, std::string(1023, 'a') + char(0xc3));
  {
    Inspector::Input input(path);
    assert(Inspector::Identify("transport.ts", input) == Inspector::Kind::Video);
  }
  // A cut-off sequence can still contain a provably invalid second byte.
  Write(path, std::string(1022, 'a') + char(0xf0) + "XYZ");
  {
    Inspector::Input input(path);
    assert(Inspector::Identify("transport.ts", input) == Inspector::Kind::Video);
  }
  Write(path, std::string("bad\0input", 9));
  {
    Inspector::Input input(path);
    assert(Inspector::Identify("binary.dat", input) == Inspector::Kind::Unknown);
    assert(Inspector::Identify("transport.ts", input) == Inspector::Kind::Video);
    bool rejected = false, truncated;
    try {
      Inspector::ReadText(input, truncated);
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    assert(rejected);
  }
}

void TestPdf(const std::string &path) {
  Write(path, PdfFixture());
  Inspector::Input input(path);
  assert(Inspector::Identify("without-extension", input) == Inspector::Kind::Pdf);
  Listi::TransferControl control;
  auto first = Inspector::RenderPdf(input, 1, control);
  auto second = Inspector::RenderPdf(input, 2, control);
  assert(first->Bounds() == second->Bounds() && first->Bounds().Width() >= 79);
  const auto *red = static_cast<const uint8 *>(first->Bits());
  const auto *blue = static_cast<const uint8 *>(second->Bits());
  assert(red[2] > 240 && red[0] < 10);
  assert(blue[0] > 240 && blue[2] < 10);
  bool unavailable = false;
  try {
    Inspector::RenderPdf(input, 3, control);
  } catch (const std::runtime_error &) {
    unavailable = true;
  }
  assert(unavailable);
  bool cancelled = false;
  control.proceed = [] { return false; };
  try {
    Inspector::RenderPdf(input, 1, control);
  } catch (const Listi::Cancelled &) {
    cancelled = true;
  }
  assert(cancelled);
}

void TestMedia(const std::string &path, bool expectNoAudio) {
  Write(path, AiffFixture());
  {
    Inspector::Input input(path);
    assert(Inspector::Identify("recording", input) == Inspector::Kind::Audio);
    assert(Inspector::Identify("recording.unlisted", input) == Inspector::Kind::Audio);
    assert(input.Position() == 0);
    auto noFrame = [](std::unique_ptr<BBitmap>) { assert(false); };
    assert(Inspector::InspectMedia(input, Inspector::Kind::Audio, false, {}, noFrame)
               .find("Audio") == 0);
  }
  // A standalone bitmap remains a photo, not a one-frame media track.
  {
    Inspector::Input input("tests/inspector-still.png");
    assert(Inspector::Identify("untitled", input) == Inspector::Kind::Image);
    assert(input.Position() == 0);
  }
  Write(path, WaveFixture());
  {
    Inspector::Input input(path);
    assert(Inspector::Identify("untitled", input) == Inspector::Kind::Audio);
    auto noFrame = [](std::unique_ptr<BBitmap>) { assert(false); };
    assert(Inspector::InspectMedia(input, Inspector::Kind::Audio, false, {}, noFrame)
               .find("Audio") == 0);
    if (expectNoAudio) {
      bool reported = false;
      try {
        Inspector::InspectMedia(input, Inspector::Kind::Audio, true, {}, noFrame);
      } catch (const std::runtime_error &e) {
        reported = std::string(e.what()) == "No system audio output is available";
      }
      assert(reported);
    }
  }
  // Synthetic 3-frame testsrc2 clip, encoded with libx264 as yuv420p.
  Inspector::Input input("tests/inspector-video.mp4");
  assert(Inspector::Identify("unlisted.clip", input) == Inspector::Kind::Video);
  int frames = 0;
  auto frame = [&](std::unique_ptr<BBitmap> image) {
    assert(image && image->Bounds() == BRect(0, 0, 31, 31));
    ++frames;
  };
  assert(Inspector::InspectMedia(input, Inspector::Kind::Video, true, {}, frame)
             .find("Video 32") == 0);
  assert(frames == 3);
}
} // namespace

int main(int argc, char **) {
  BApplication app("application/x-vnd.kambuzi-inspector-test");
  char directory[] = "/tmp/Listi-inspector-test-XXXXXX";
  assert(mkdtemp(directory));
  std::string path = std::string(directory) + "/fixture";
  TestText(path);
  TestPdf(path);
  TestMedia(path, argc > 1);
  assert(unlink(path.c_str()) == 0);
  assert(mkfifo(path.c_str(), 0600) == 0);
  bool rejected = false;
  try {
    Inspector::Input fifo(path);
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  assert(rejected);
  assert(unlink(path.c_str()) == 0);
  assert(rmdir(directory) == 0);
}
