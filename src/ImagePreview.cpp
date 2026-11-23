// SPDX-License-Identifier: MIT
#include "ReadWorker.h"
#include "StagingSpace.h"
#include "AdaptiveScrollView.h"
#include "ImagePreview.h"
#include "WindowOverview.h"
#include "ViewVisibility.h"
#include "BitmapMailbox.h"
#include "Common.h"
#include <LayoutBuilder.h>
#include <Window.h>
#include "InspectorContent.h"
#include "InspectorDocument.h"
#include "InspectorEditor.h"
#include <Alert.h>
#include <Application.h>
#include <Entry.h>
#include <Roster.h>
#include <sys/stat.h>
#include "ThemeTextView.h"
#include "VirtualFile.h"
#include <Bitmap.h>
#include <BitmapStream.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Button.h>
#include <ScrollView.h>
#include <DataIO.h>
#include <Font.h>
#include <Looper.h>
#include <Message.h>
#include <Messenger.h>
#include <String.h>
#include <TranslationUtils.h>
#include <TranslatorFormats.h>
#include <TranslatorRoster.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
namespace {
constexpr uint32 kLoad = 'pimg', kReady = 'pird', kFrame = 'pifr',
                 kFileAction = 'pifa', kPrevious = 'pipv', kNext = 'pinx', kPlay = 'pipl';

constexpr uint32 kPhotoMenu = 'pipm';
constexpr uint32 kDetachEditor = 'pidt', kDoneEditing = 'pide';

// Native JPEG translation writes a bitmap header followed by rows. Decode
// directly into the final bitmap, not a second image-sized memory stream.
constexpr size_t kPhotoPixels = 48000000;
class PhotoBitmapStream : public BBitmapStream {
  const Listi::TransferControl &control;
  bool sawHeader = false;

public:
  explicit PhotoBitmapStream(const Listi::TransferControl &c) : control(c) {}
  ssize_t WriteAt(off_t offset, const void *data, size_t size) override {
    try {
      control.Check();
    } catch (...) {
      return B_CANCELED;
    }
    if (!sawHeader) {
      if (offset != 0 || size < sizeof(TranslatorBitmap))
        return B_BAD_DATA;
      struct {
        uint32 magic;
        float left, top, right, bottom;
        uint32 rowBytes, colors, dataSize;
      } header;
      static_assert(sizeof(header) == sizeof(TranslatorBitmap));
      std::memcpy(&header, data, sizeof(header));
      if (B_HOST_IS_LENDIAN)
        swap_data(B_UINT32_TYPE, &header, sizeof(header), B_SWAP_ALWAYS);
      double width = header.right + 1., height = header.bottom + 1.;
      if (header.magic != B_TRANSLATOR_BITMAP || header.left != 0 ||
          header.top != 0 || !(width >= 1 && width <= 65535) ||
          !(height >= 1 && height <= 65535) || std::floor(width) != width ||
          std::floor(height) != height || width * height > kPhotoPixels ||
          header.rowBytes < width || header.dataSize > kPhotoPixels * 4 ||
          height * header.rowBytes != header.dataSize)
        return B_BAD_DATA;
      sawHeader = true;
    } else if (offset < static_cast<off_t>(sizeof(TranslatorBitmap))) {
      return B_BAD_DATA;
    }
    return BBitmapStream::WriteAt(offset, data, size);
  }
};

BBitmap *DecodePhoto(Inspector::Input &input, const Inspector::PhotoInfo &photo,
                     const Listi::TransferControl &control) {
  if (!photo.width || !photo.height)
    throw std::runtime_error("Cannot read the JPEG frame dimensions");
  if (uint64_t(photo.width) * photo.height > kPhotoPixels)
    throw std::runtime_error("Image exceeds the 48 megapixel preview limit");
  off_t bytes;
  input.GetSize(&bytes);
  if (bytes > 64 * 1024 * 1024)
    throw std::runtime_error("Image previews are limited to 64 MiB");
  PhotoBitmapStream stream(control);
  auto status = BTranslatorRoster::Default()->Translate(
      &input, nullptr, nullptr, &stream, B_TRANSLATOR_BITMAP);
  control.Check();
  BBitmap *bitmap = nullptr;
  if (status != B_OK || stream.DetachBitmap(&bitmap) != B_OK)
    throw std::runtime_error("Cannot decode this JPEG image");
  return bitmap;
}

class CachedPreview {
public:
  CachedPreview(const std::string &source, const Listi::TransferControl &control)
      : path(source) {
    if (!Listi::IsVirtual(source))
      return;
    char pattern[] = "/tmp/Listi-preview-XXXXXX";
    int fd = mkstemp(pattern);
    if (fd < 0)
      throw std::runtime_error("Cannot create preview cache");
    try {
      size_t total = 0;
      Listi::ReadFile(
          source,
          [&](const char *data, size_t size) {
            total += size;
            if (total > 32 * 1024 * 1024)
              throw std::runtime_error("File is too large for a network preview");
            while (size) {
              ssize_t written = Listi::WriteStaging(fd, data, size);
              if (written < 0 && errno == EINTR)
                continue;
              if (written <= 0)
                throw std::runtime_error("Preview cache write failed");
              data += written;
              size -= written;
            }
          },
          control);
    } catch (...) {
      close(fd);
      unlink(pattern);
      throw;
    }
    close(fd);
    path = pattern;
    cached = true;
  }
  ~CachedPreview() {
    if (cached)
      unlink(path.c_str());
  }
  std::string path;

private:
  bool cached = false;
};

class BoundedBitmap : public BMallocIO {
  const Listi::TransferControl &control;

public:
  explicit BoundedBitmap(const Listi::TransferControl &c) : control(c) {}
  status_t SetSize(off_t size) override {
    if (size < 0 || size > 64 * 1024 * 1024)
      return B_NO_MEMORY;
    return BMallocIO::SetSize(size);
  }
  ssize_t WriteAt(off_t position, const void *data, size_t size) override {
    try {
      control.Check();
    } catch (...) {
      return B_CANCELED;
    }
    constexpr off_t limit = 64 * 1024 * 1024;
    if (position < 0 || position > limit ||
        size > static_cast<size_t>(limit - position))
      return B_NO_MEMORY;
    return BMallocIO::WriteAt(position, data, size);
  }
};
BBitmap *DecodeImage(Inspector::Input &input, const Listi::TransferControl &control) {
  constexpr off_t limit = 64 * 1024 * 1024;
  off_t bytes;
  input.GetSize(&bytes);
  if (bytes > limit)
    throw std::runtime_error("Image previews are limited to 64 MiB");
  BoundedBitmap decoded(control);
  const auto status = BTranslatorRoster::Default()->Translate(
      &input, nullptr, nullptr, &decoded, B_TRANSLATOR_BITMAP);
  control.Check();
  if (status != B_OK)
    throw std::runtime_error("No preview available");
  decoded.Seek(0, SEEK_SET);
  auto *image = BTranslationUtils::GetBitmap(&decoded);
  if (!image)
    throw std::runtime_error("Cannot decode this image");
  return image;
}

class Loader : public Listi::ReadWorker {
  BMessenger target;
  std::shared_ptr<TerminalReplies> terminal;
  std::shared_ptr<BitmapMailbox> replies;
  std::shared_ptr<std::atomic<int64>> generation;

public:
  Loader(BMessenger t, std::shared_ptr<std::atomic<int64>> g,
         std::shared_ptr<BitmapMailbox> r, std::shared_ptr<TerminalReplies> queue)
      : ReadWorker("image preview reader"), target(t), terminal(queue), replies(r), generation(g) {}
  void MessageReceived(BMessage *m) override {
    if (m->what != kLoad) {
      BLooper::MessageReceived(m);
      return;
    }
    int64 serial = m->GetInt64("serial", -1);
    Listi::TransferControl c;
    c.proceed = [&] { return generation->load() == serial; };
    BMessage result(kReady);
    result.AddInt64("serial", serial);
    std::unique_ptr<BBitmap> image;
    Inspector::Kind kind = Inspector::Kind::Unknown;
    int page = m->GetInt32("page", 1);
    try {
      c.Check();
      const std::string source = m->GetString("path", "");
      CachedPreview file(source, c);
      Inspector::Input input(file.path);
      kind = Inspector::Identify(Listi::LocationLeaf(source), input);
      off_t length = 0;
      input.GetSize(&length);
      struct stat st;
      const bool writable = !Listi::IsVirtual(source) && length <= off_t(Inspector::kDocumentLimit) &&
          lstat(source.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
          (st.st_mode & 0222) != 0 && access(source.c_str(), W_OK) == 0;
      result.AddBool("writable", writable);
      switch (kind) {
      case Inspector::Kind::Text:
      case Inspector::Kind::Code:
      case Inspector::Kind::Markdown:
      case Inspector::Kind::Html: {
        bool truncated;
        const bool standalone = m->GetBool("standalone", false);
        const auto text = Inspector::ReadText(input, truncated, standalone ? 1024 * 1024 : 256 * 1024);
        BString caption = kind == Inspector::Kind::Markdown ? "Markdown source"
                          : kind == Inspector::Kind::Html ? "HTML source"
                          : kind == Inspector::Kind::Code ? "Code"
                          : "Text";
        if (truncated)
          caption << (standalone ? " (first 1 MiB)" : " (first 256 KiB)");
        result.AddString("caption", caption);
        result.AddString("text", text.c_str());
        break;
      }
      case Inspector::Kind::Pdf: {
        image = Inspector::RenderPdf(input, page, c);
        BString caption("PDF · page ");
        caption << page;
        result.AddString("caption", caption);
        result.AddBool("pageValid", true);
        break;
      }
      case Inspector::Kind::Audio:
      case Inspector::Kind::Video: {
        bool play = m->GetBool("play", false);
        auto caption = Inspector::InspectMedia(
            input, kind, play, c, [&](std::unique_ptr<BBitmap> frame) {
              if (!play) {
                image = std::move(frame);
                return;
              }
              BMessage next(kFrame);
              next.AddInt64("serial", serial);
              Send(next, std::move(frame));
            });
        result.AddString("caption", caption.c_str());
        if (play)
          result.AddBool("keepBitmap", true);
        break;
      }
      default: {
        // Older or extensionless images still use all installed translators.
        auto photo = Inspector::ReadPhotoInfo([&](size_t p, void *data, size_t n) {
          c.Check();
          return input.ReadAt(p, data, n) == static_cast<ssize_t>(n);
        });
        image.reset(photo.jpeg ? DecodePhoto(input, photo, c) : DecodeImage(input, c));
        kind = Inspector::Kind::Image;
        result.AddString("camera", photo.cameraDetails.c_str());
        if (photo.hasLocation) {
          result.AddDouble("latitude", photo.latitude);
          result.AddDouble("longitude", photo.longitude);
        }
        BString caption;
        caption << int32(image->Bounds().Width() + 1) << " × "
                << int32(image->Bounds().Height() + 1);
        result.AddString("caption", caption);
        break;
      }
      }
      c.Check();
    } catch (const Listi::Cancelled &) {
      return;
    } catch (const std::exception &e) {
      result.AddString("caption", e.what());
    }
    result.AddInt32("kind", static_cast<int32>(kind));
    result.AddInt32("page", page);
    Send(result, std::move(image));
  }

private:
  void Send(BMessage &message, std::unique_ptr<BBitmap> image) {
    if (message.GetInt64("serial", -1) != generation->load())
      return;
    int64 reply = image ? replies->Put(image.release()) : -1;
    message.AddInt64("bitmap", reply);
    if (message.what == kReady) {
      terminal->Deliver(message, *generation, [&] {
        return target.SendMessage(&message, (BHandler *)nullptr, 50000);
      });
      return;
    }
    if (target.SendMessage(&message, (BHandler *)nullptr, 50000) != B_OK)
      replies->Take(reply);
  }
};
} // namespace
BBitmap *LoadPreviewBitmap(const std::string &source,
                           const Listi::TransferControl &c, int maxSize) {
  BBitmap *image = nullptr;
  try {
    c.Check();
    CachedPreview file(source, c);
    Inspector::Input input(file.path, 64 * 1024 * 1024);
    image = DecodeImage(input, c);

    if (maxSize > 0 && (image->Bounds().Width() + 1 > maxSize ||
                        image->Bounds().Height() + 1 > maxSize)) {
      auto bounds = image->Bounds();
      float scale = std::min(float(maxSize) / (bounds.Width() + 1),
                             float(maxSize) / (bounds.Height() + 1));
      auto small = new BBitmap(
          BRect(0, 0, std::max(1.f, (bounds.Width() + 1) * scale) - 1,
                std::max(1.f, (bounds.Height() + 1) * scale) - 1),
          B_BITMAP_ACCEPTS_VIEWS, B_RGBA32);
      if (small->InitCheck() != B_OK) {
        delete small;
        throw std::runtime_error("Cannot allocate thumbnail");
      }
      auto drawing =
          new BView(small->Bounds(), "thumbnail", B_FOLLOW_NONE, B_WILL_DRAW);
      small->AddChild(drawing);
      small->Lock();
      drawing->SetHighColor(0, 0, 0, 0);
      drawing->FillRect(drawing->Bounds());
      drawing->SetDrawingMode(B_OP_ALPHA);
      drawing->DrawBitmap(image, image->Bounds(), drawing->Bounds());
      drawing->Sync();
      small->Unlock();
      small->RemoveChild(drawing);
      delete drawing;
      delete image;
      image = small;
    }
    c.Check();
    return image;
  } catch (...) {
    delete image;
    throw;
  }
}
void ImagePreview::OpenWindow(const std::string &path) {
  auto window = new Listi::WorkingWindow(BRect(120, 100, 900, 650), path.c_str(),
                                         "Preview", B_AUTO_UPDATE_SIZE_LIMITS);
  window->Describe(path, path);
  auto inspector = new ImagePreview(true);
  inspector->ShowFile(path);
  BLayoutBuilder::Group<>(window, B_VERTICAL, 0).Add(inspector);
  window->AddShortcut(B_ESCAPE, B_NO_COMMAND_KEY,
                      new BMessage(B_QUIT_REQUESTED));
  window->AddShortcut('W', B_COMMAND_KEY, new BMessage(B_QUIT_REQUESTED));
  window->Show();
}

ImagePreview::ImagePreview(bool standalone)
    : BView("inspector", B_PULSE_NEEDED | B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE |
                             B_FRAME_EVENTS | B_SUPPORTS_LAYOUT),
      replies(std::make_shared<BitmapMailbox>(2)),
      generation(std::make_shared<std::atomic<int64>>(0)), standalone(standalone) {
  SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
  SetLowUIColor(B_DOCUMENT_BACKGROUND_COLOR);
  SetHighUIColor(B_DOCUMENT_TEXT_COLOR);
  SetExplicitMinSize(BSize(180, 128));
  SetExplicitPreferredSize(BSize(280, 400));

  text = new ThemeTextView("preview text", B_DOCUMENT_BACKGROUND_COLOR,
                           B_DOCUMENT_TEXT_COLOR);
  text->SetFontAndColor(be_fixed_font);
  text->MakeEditable(false);
  text->SetWordWrap(true);
  text->SetInsets(4, 3, 4, 3);
  text->SetResizingMode(B_FOLLOW_ALL);
  textScroll = new Listi::AdaptiveScrollView("preview scroll", text, false);
  AddChild(textScroll);
  textScroll->Hide();

  auto addButton = [this](const char *label, uint32 command) {
    auto button = new BButton(label, new BMessage(command));
    AddChild(button);
    button->ResizeToPreferred();
    button->Hide();
    return button;
  };
  fileAction = addButton("Edit", kFileAction);
  previous = addButton("Previous", kPrevious);
  photoActions = addButton("Image…", kPhotoMenu);
  photoActions->SetToolTip("Image size, zoom and camera details. Scroll to zoom; drag to pan.");
  next = addButton("Next", kNext);
  play = addButton("Play preview", kPlay);
  detachEditor = addButton("Detach", kDetachEditor);
  detachEditor->SetToolTip("Move the editor to its own window");
  doneEditing = addButton("Done", kDoneEditing);
  doneEditing->SetToolTip("Close the editor and return to preview");
  closeQuickView = addButton("×", kCloseQuickView);
  closeQuickView->ResizeTo(closeQuickView->Bounds().Height(), closeQuickView->Bounds().Height());
  closeQuickView->SetName("close quick view");
  closeQuickView->SetToolTip("Close Quick view (Esc)");
}
ImagePreview::~ImagePreview() {
  generation->store(-1);
  Listi::RetireReadWorker(loader);
  delete bitmap;
}
void ImagePreview::AttachedToWindow() {
  closeQuickView->SetTarget(Window());
  fileAction->SetTarget(this);
  detachEditor->SetTarget(this);
  doneEditing->SetTarget(this);
  StartWatching(BMessenger(be_app), 'isav');
  previous->SetTarget(this);
  photoActions->SetTarget(this);
  next->SetTarget(this);
  play->SetTarget(this);
  FrameResized(Bounds().Width(), Bounds().Height());
  if (loader)
    return;
  loader = new Loader(BMessenger(this), generation, replies, terminal);
  Listi::StartReadWorker(loader);
  if (!path.empty() && !editor)
    Request(1);
}
void ImagePreview::DetachedFromWindow() {
  StopWatching(BMessenger(be_app), 'isav');
  // A reattached view receives a new handler token. Do not keep a loader
  // that posts completed reads to the token from the previous layout.
  ++*generation;
  Listi::RetireReadWorker(loader);
  terminal->Take();
  replies->Clear();
  BView::DetachedFromWindow();
}
bool ImagePreview::ShowFile(const std::string &p) {
  if (editor) {
    // Selection and listing updates can arrive while a draft is open.
    // Keep the document pinned, but restore the newest preview on exit.
    if (path == p)
      pendingFile.reset();
    else
      pendingFile = p;
    return path == p;
  }
  if (path == p && !submissionFailed)
    return true;
  path = p;
  photoView = {};
  draggingPhoto = false;
  cameraDetails.clear();
  hasLocation = false;
  page = 1;
  lastPage = 0;
  kind = Inspector::Kind::Unknown;
  playing = false;
  Request(1);
  return true;
}
bool ImagePreview::BeginEditing(const std::string &p) {
  if (p.empty() || Listi::IsVirtual(p))
    return false;
  if (editor && (path == p || !CloseEditor()))
    return path == p;
  if (standalone) {
    OpenInspectorEditor(p, Window());
    return true;
  }
  editor = OpenEmbeddedInspectorEditor(p, this, this);
  if (!editor)
    return false;
  path = p;
  pendingFile.reset();
  ++*generation;
  terminal->Take();
  replies->Clear();
  Listi::SetViewVisible(textScroll, false);
  delete bitmap;
  bitmap = nullptr;
  UpdateControls();
  return true;
}
bool ImagePreview::CloseEditor() {
  if (!editor)
    return true;
  if (!InspectorEditorAllowClose(editor))
    return false;
  editor->RemoveSelf();
  delete editor;
  RemoveEditor();
  return true;
}
void ImagePreview::RemoveEditor() {
  editor = nullptr;
  const auto nextFile = std::move(pendingFile);
  pendingFile.reset();
  if (nextFile && *nextFile != path) {
    ShowFile(*nextFile);
  } else {
    kind = Inspector::Kind::Unknown;
    Request(1);
  }
}
void ImagePreview::Request(int requestedPage, bool playback) {
  ready = false;
  submissionFailed = false;
  int64 serial = ++*generation;
  terminal->Take();
  replies->Clear();
  if (!loader && Window() && !path.empty()) {
    loader = new Loader(BMessenger(this), generation, replies, terminal);
    Listi::StartReadWorker(loader);
    submissionFailed = !loader;
  }
  if (kind == Inspector::Kind::Unknown) {
    delete bitmap;
    bitmap = nullptr;
  }
  caption = path.empty() ? "Select a file to inspect"
                        : playback ? "Playing preview (up to 15 seconds)"
                                   : loader ? "Loading preview…" : "Preview reader unavailable.";
  Listi::SetViewVisible(textScroll, false);
  if (!path.empty() && loader) {
    BMessage m(kLoad);
    m.AddInt64("serial", serial);
    m.AddString("path", path.c_str());
    m.AddInt32("page", requestedPage);
    m.AddBool("play", playback);
    m.AddBool("standalone", standalone);
    if (loader->PostMessage(&m) != B_OK) {
      submissionFailed = true;
      caption = "Preview reader busy. Select the file again.";
    }
  }
  UpdateControls();
}
void ImagePreview::UpdateControls() {
  const bool pdf = !editor && kind == Inspector::Kind::Pdf;
  const bool media =
      !editor && (kind == Inspector::Kind::Audio || kind == Inspector::Kind::Video);
  const bool sourceText = kind == Inspector::Kind::Text || kind == Inspector::Kind::Code ||
                          kind == Inspector::Kind::Markdown || kind == Inspector::Kind::Html;
  const bool unknown = kind == Inspector::Kind::Unknown && !bitmap;
  const bool local = !path.empty() && !Listi::IsVirtual(path);
  const char *label = media ? "MediaPlayer" : !canEdit ? "Read only" :
                      unknown ? "Try as text…" : "Edit";
  fileAction->SetLabel(label);
  fileAction->ResizeToPreferred();
  fileAction->SetEnabled(media || canEdit);
  Listi::SetViewVisible(fileAction, !editor && ready && !path.empty() &&
                       (sourceText || unknown || (media && local)));
  Listi::SetViewVisible(detachEditor, editor);
  Listi::SetViewVisible(doneEditing, editor);
  previous->SetEnabled(pdf && page > 1);
  next->SetEnabled(pdf && page != lastPage);
  Listi::SetViewVisible(previous, pdf);
  Listi::SetViewVisible(photoActions, !editor && kind == Inspector::Kind::Image && bitmap);
  Listi::SetViewVisible(next, pdf);
  Listi::SetViewVisible(play, media);
  play->SetLabel(playing ? "Stop preview" : "Play preview");
  LayoutControls();
  Invalidate();
}
void ImagePreview::Pulse() {
  for (auto &reply : terminal->Take())
    MessageReceived(&reply);
}
void ImagePreview::MessageReceived(BMessage *m) {
  switch (m->what) {
  case kDetachEditor:
    if (editor && DetachInspectorEditor(editor))
      RemoveEditor();
    return;
  case kDoneEditing:
  case kInspectorEmbeddedClose:
    CloseEditor();
    return;
  case kPhotoMenu:
    if (kind == Inspector::Kind::Image && bitmap)
      PhotoMenu();
    return;
  case B_MOUSE_WHEEL_CHANGED: {
    if (kind != Inspector::Kind::Image || !bitmap) {
      BView::MessageReceived(m);
      return;
    }
    float dy = m->GetFloat("be:wheel_delta_y", 0);
    if (dy != 0) {
      BPoint pointer;
      uint32 buttons;
      GetMouse(&pointer, &buttons, false);
      ZoomPhoto(dy < 0 ? 1.5 : 1. / 1.5, pointer);
    }
    return;
  }
  case B_OBSERVER_NOTICE_CHANGE:
    if (!editor && m->GetInt32(B_OBSERVE_WHAT_CHANGE, 0) == 'isav' &&
        !path.empty() && path == m->GetString("path", ""))
      Request(1);
    return;
  case kFileAction: {
    if (!ready || path.empty() || Listi::IsVirtual(path))
      return;
    if (kind == Inspector::Kind::Audio || kind == Inspector::Kind::Video) {
      entry_ref ref;
      BMessage open(B_REFS_RECEIVED);
      if (get_ref_for_path(path.c_str(), &ref) == B_OK && open.AddRef("refs", &ref) == B_OK) {
        auto status = be_roster->Launch("application/x-vnd.Haiku-MediaPlayer", &open);
        if (status == B_OK || status == B_ALREADY_RUNNING)
          return;
      }
      (new BAlert("MediaPlayer", "Could not open this file in Haiku MediaPlayer.",
                  "OK"))->Go(nullptr);
    } else if (canEdit && !bitmap) {
      BeginEditing(path);
    }
    return;
  }
  case kPrevious:
    if (kind == Inspector::Kind::Pdf && page > 1)
      Request(page - 1);
    return;
  case kNext:
    if (kind == Inspector::Kind::Pdf && page != lastPage && page < INT_MAX)
      Request(page + 1);
    return;
  case kPlay:
    if (kind == Inspector::Kind::Audio || kind == Inspector::Kind::Video) {
      playing = !playing;
      Request(1, playing);
    }
    return;
  case kReady:
  case kFrame: {
    auto image = replies->Take(m->GetInt64("bitmap", -1));
    if (m->GetInt64("serial", -1) != generation->load())
      return;
    if (m->what == kFrame) {
      if (image) {
        delete bitmap;
        bitmap = image.release();
        Invalidate();
      }
      return;
    }
    kind = static_cast<Inspector::Kind>(m->GetInt32("kind", 0));
    cameraDetails = m->GetString("camera", "");
    hasLocation = m->FindDouble("latitude", &latitude) == B_OK &&
                  m->FindDouble("longitude", &longitude) == B_OK;
    ready = true;
    canEdit = m->GetBool("writable", false);
    const int resultPage = m->GetInt32("page", 1);
    const bool validPage = m->GetBool("pageValid", false);
    if (validPage)
      page = resultPage;
    bool beyondLastPage = kind == Inspector::Kind::Pdf && !validPage &&
                          resultPage > page && bitmap;
    if (beyondLastPage)
      lastPage = page;
    if (!beyondLastPage && !m->GetBool("keepBitmap", false)) {
      delete bitmap;
      bitmap = image.release();
    }
    caption = m->GetString("caption", "");
    if (beyondLastPage)
      caption += " (showing page " + std::to_string(page) + ")";
    if (const char *content = m->GetString("text", nullptr)) {
      text->SetText(content);
      text->Select(0, 0);
      text->ScrollToOffset(0);
      textScroll->Show();
    }
    if (kind == Inspector::Kind::Unknown && !bitmap && canEdit)
      caption = "Unknown format. Try opening it as UTF-8 text?";
    playing = false;
    UpdateControls();
    return;
  }
  default:
    BView::MessageReceived(m);
  }
}

BRect ImagePreview::PhotoArea() const {
  BRect area = Bounds().InsetByCopy(12, 12);
  area.top = std::max(area.top + 22, photoActions->Frame().bottom + 4);
  area.bottom = Bounds().bottom - 45;
  return area;
}

void ImagePreview::ZoomPhoto(double factor, BPoint anchor) {
  auto area = PhotoArea(), bounds = bitmap->Bounds();
  double w = bounds.Width() + 1, h = bounds.Height() + 1;
  double vw = area.Width() + 1, vh = area.Height() + 1;
  auto before = photoView.Region(w, h, vw, vh);
  if (before.scale <= 0)
    return;
  const auto fit = Inspector::PhotoView{}.Region(w, h, vw, vh);
  photoView.zoom = std::clamp(before.scale * factor, fit.scale, 8.);
  auto after = photoView.Region(w, h, vw, vh);
  // Keep the pixel under the pointer stationary when zoom changes.
  double x = anchor.x - area.left, y = anchor.y - area.top;
  double sourceX = before.left + (x - before.x) / before.scale;
  double sourceY = before.top + (y - before.y) / before.scale;
  double left = sourceX - (x - after.x) / after.scale;
  double top = sourceY - (y - after.y) / after.scale;
  photoView.centerX = (std::clamp(left, 0., w-after.width) + after.width/2) / w;
  photoView.centerY = (std::clamp(top, 0., h-after.height) + after.height/2) / h;
  Invalidate();
}

void ImagePreview::PhotoMenu() {
  BPopUpMenu menu("Image", false, false);
  auto add = [&](const char *label) {
    auto *item = new BMenuItem(label, nullptr);
    menu.AddItem(item);
    return item;
  };
  auto *fit = add("Fit to window"), *actual = add("Actual pixels (100%)");
  auto *larger = add("Zoom in"), *smaller = add("Zoom out");
  menu.AddSeparatorItem();
  auto *details = add("Camera details…"), *gps = add("Show GPS coordinates…");
  details->SetEnabled(!cameraDetails.empty());
  gps->SetEnabled(hasLocation);
  BPoint location = photoActions->Frame().LeftBottom() + BPoint(0, 2);
  auto *chosen = menu.Go(ConvertToScreen(location), false, true);
  if (!chosen)
    return;
  if (chosen == details) {
    (new BAlert("Camera details", cameraDetails.c_str(), "OK"))->Go(nullptr);
  } else if (chosen == gps && hasLocation) {
    char text[100];
    std::snprintf(text, sizeof(text), "Latitude: %.6f\nLongitude: %.6f", latitude, longitude);
    (new BAlert("GPS coordinates", text, "OK"))->Go(nullptr);
  } else if (chosen == larger || chosen == smaller) {
    BRect area = PhotoArea();
    BPoint center((area.left + area.right) / 2, (area.top + area.bottom) / 2);
    ZoomPhoto(chosen == larger ? 1.5 : 1. / 1.5, center);
  } else {
    if (chosen == fit)
      photoView = {};
    else if (chosen == actual)
      photoView.zoom = 1;
    Invalidate();
  }
}

void ImagePreview::MouseDown(BPoint where) {
  if (kind == Inspector::Kind::Image && bitmap && PhotoArea().Contains(where)) {
    draggingPhoto = true;
    lastPointer = where;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    return;
  }
  BView::MouseDown(where);
}
void ImagePreview::MouseMoved(BPoint where, uint32 transit, const BMessage *drag) {
  if (draggingPhoto && bitmap && kind == Inspector::Kind::Image) {
    auto bounds = bitmap->Bounds(), area = PhotoArea();
    photoView.Pan(where.x - lastPointer.x, where.y - lastPointer.y,
                 bounds.Width()+1, bounds.Height()+1, area.Width()+1, area.Height()+1);
    lastPointer = where;
    Invalidate();
    return;
  }
  BView::MouseMoved(where, transit, drag);
}
void ImagePreview::MouseUp(BPoint where) {
  draggingPhoto = false;
  BView::MouseUp(where);
}

void ImagePreview::SetQuickViewCloseVisible(bool visible) {
  if (Listi::SetViewVisible(closeQuickView, visible))
    FrameResized(Bounds().Width(), Bounds().Height());
}

void ImagePreview::FrameResized(float, float) {
  LayoutControls();
  Invalidate();
}

void ImagePreview::LayoutControls() {
  const float width = Bounds().Width(), height = Bounds().Height();
  const float closeLeft = width - closeQuickView->Bounds().Width() - 5;
  closeQuickView->MoveTo(std::max(0.f, closeLeft), 3);
  const float actionsRight = closeQuickView->IsHidden(closeQuickView) ? width : closeLeft;
  photoActions->MoveTo(std::max(0.f, actionsRight - photoActions->Bounds().Width() - 8), 3);
  fileAction->MoveTo(std::max(0.f, actionsRight - fileAction->Bounds().Width() - 8), 3);
  const float doneLeft = actionsRight - doneEditing->Bounds().Width() - 8;
  doneEditing->MoveTo(std::max(0.f, doneLeft), 3);
  const float detachLeft = doneLeft - detachEditor->Bounds().Width() - 5;
  detachEditor->MoveTo(std::max(0.f, detachLeft), 3);
  if (editor) {
    editor->MoveTo(0, 35);
    editor->ResizeTo(width, std::max(0.f, height - 35));
  }
  textScroll->MoveTo(8, 35);
  textScroll->ResizeTo(std::max(32.f, width - 16), std::max(32.f, height - 70));
  const float buttonTop = height - 44 - previous->Bounds().Height();
  previous->MoveTo(12, buttonTop);
  next->MoveTo(20 + previous->Bounds().Width(), buttonTop);
  play->MoveTo(12, buttonTop);
}
void ImagePreview::Draw(BRect) {
  SetHighUIColor(B_DOCUMENT_TEXT_COLOR);
  BString title(editor ? "Edit"
                      : closeQuickView->IsHidden(closeQuickView) ? "Inspector" : "Quick view");
  float right = Bounds().right;
  for (auto button : {photoActions, fileAction, closeQuickView, detachEditor, doneEditing})
    if (!button->IsHidden(button))
      right = std::min(right, button->Frame().left);
  TruncateString(&title, B_TRUNCATE_END, std::max(0.f, right - 16));
  DrawString(title, BPoint(12, 22));
  if (editor)
    return;
  BRect area = Bounds().InsetByCopy(12, 12);
  area.top += 22;
  area.bottom = previous->IsHidden(previous) && play->IsHidden(play)
                    ? Bounds().bottom - 53
                    : previous->Frame().top - 6;
  BString line(caption.c_str());
  if (bitmap && kind == Inspector::Kind::Image) {
    area = PhotoArea();
    const auto bounds = bitmap->Bounds();
    const auto region = photoView.Region(bounds.Width()+1, bounds.Height()+1,
                                         area.Width()+1, area.Height()+1);
    if (region.scale > 0) {
      BRect src(0, 0, std::max(0., region.width-1), std::max(0., region.height-1));
      src.OffsetTo(bounds.left + region.left, bounds.top + region.top);
      BRect dest(0, 0, std::max(0., region.width*region.scale-1),
                 std::max(0., region.height*region.scale-1));
      dest.OffsetTo(area.left + region.x, area.top + region.y);
      SetDrawingMode(B_OP_ALPHA);
      DrawBitmap(bitmap, src, dest);
      SetDrawingMode(B_OP_COPY);
      line << " · " << int32(std::lround(region.scale * 100)) << "%";
    }
  } else if (bitmap && area.Width() > 0 && area.Height() > 0) {
    BRect src = bitmap->Bounds();
    float scale = std::min(1.f, std::min(area.Width() / (src.Width() + 1),
                                         area.Height() / (src.Height() + 1)));
    float width = (src.Width() + 1) * scale,
          height = (src.Height() + 1) * scale;
    BRect dest(area.left + (area.Width() - width) / 2,
               area.top + (area.Height() - height) / 2, 0, 0);
    dest.right = dest.left + width - 1;
    dest.bottom = dest.top + height - 1;
    SetDrawingMode(B_OP_ALPHA);
    DrawBitmap(bitmap, src, dest);
    SetDrawingMode(B_OP_COPY);
  }
  TruncateString(&line, B_TRUNCATE_END, std::max(0.f, Bounds().Width() - 24));
  DrawString(line, BPoint(12, Bounds().bottom - 26));
  BString name(path.empty() ? "" : Listi::LocationLeaf(path).c_str());
  TruncateString(&name, B_TRUNCATE_MIDDLE,
                 std::max(0.f, Bounds().Width() - 24));
  DrawString(name, BPoint(12, Bounds().bottom - 9));
}
