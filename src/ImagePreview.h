// SPDX-License-Identifier: MIT
#pragma once
#include "TerminalReplies.h"
#include "InspectorContent.h"
#include "InspectorPhoto.h"
#include <View.h>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
class BBitmap;
class BitmapMailbox;
namespace Listi { class ReadWorker; }
class BButton;
class BScrollView;
class ThemeTextView;
class ImagePreview : public BView {
public:
  explicit ImagePreview(bool standalone = false);
  static void OpenWindow(const std::string &path);
  ~ImagePreview();
  void AttachedToWindow() override;
  void DetachedFromWindow() override;
  void Pulse() override;
  void MessageReceived(BMessage *) override;
  void Draw(BRect) override;
  void FrameResized(float, float) override;
  void MouseDown(BPoint) override;
  void MouseMoved(BPoint, uint32, const BMessage *) override;
  void MouseUp(BPoint) override;
  void SetQuickViewCloseVisible(bool visible);
  bool ShowFile(const std::string &);
  bool HasEditor() const { return editor != nullptr; }
  bool BeginEditing(const std::string &);
  bool CloseEditor();
  const std::string &File() const { return path; }

private:
  std::shared_ptr<TerminalReplies> terminal = std::make_shared<TerminalReplies>();
  void Request(int page, bool playback = false);
  void UpdateControls();
  void RemoveEditor();
  void LayoutControls();
  BRect PhotoArea() const;
  void PhotoMenu();
  void ZoomPhoto(double factor, BPoint anchor);
  BButton *photoActions;
  Inspector::PhotoView photoView;
  std::string cameraDetails;
  bool hasLocation = false, draggingPhoto = false;
  double latitude = 0, longitude = 0;
  BPoint lastPointer;
  BBitmap *bitmap = nullptr;
  std::shared_ptr<BitmapMailbox> replies;
  Listi::ReadWorker *loader = nullptr;
  std::shared_ptr<std::atomic<int64>> generation;
  ThemeTextView *text;
  BScrollView *textScroll;
  BButton *previous, *next, *play;
  BButton *fileAction, *closeQuickView;
  BButton *detachEditor, *doneEditing;
  BView *editor = nullptr;
  bool submissionFailed = false;
  bool ready = false, canEdit = false;
  Inspector::Kind kind = Inspector::Kind::Unknown;
  const bool standalone;
  int page = 1, lastPage = 0;
  bool playing = false;
  std::string path, caption = "Select a file to inspect";
  std::optional<std::string> pendingFile;
};

namespace Listi {
struct TransferControl;
}
BBitmap *LoadPreviewBitmap(const std::string &, const Listi::TransferControl &,
                           int maxSize = 0);
