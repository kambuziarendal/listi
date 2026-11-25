// SPDX-License-Identifier: MIT
#pragma once
#include "FileOperation.h"
#include <Clipboard.h>
#include <Entry.h>
#include <Message.h>
#include <string>
#include <vector>

namespace Listi {
inline constexpr size_t kFileSelectionLimit = 1024;
struct ClipboardItem {
  entry_ref ref;
  NodeIdentity node;
  std::string key, path;
  bool cut = false;
};
struct ClipboardPaste {
  std::vector<ClipboardItem> items;
  std::vector<Job> jobs;
  std::string lease;
  bool archiveCut = false;
};
std::string ReadClipboardText(BClipboard *clipboard = be_clipboard);
void PutPathsAsText(const std::vector<std::string> &,
                    BClipboard *clipboard = be_clipboard);
void CopySelectionText(uint32 action, const std::vector<std::string> &paths,
                       const std::string &root, BClipboard *clipboard = be_clipboard);
bool HasClipboardFiles(const BMessage &);
// Tracker's native file-reference wire format. No text paths or credentials.
std::vector<ClipboardItem> ReadFileClipboard(const BMessage &);
BMessage MakeFileSelection(const std::vector<std::string> &, bool cut = false,
                           const std::map<std::string, NodeIdentity> &expected = {});
void PutFileClipboard(const std::vector<std::string> &, bool cut,
                      BClipboard *clipboard = be_clipboard,
                      const std::map<std::string, NodeIdentity> &expected = {});
ClipboardPaste PrepareFilePaste(const std::string &destination,
                                BClipboard *clipboard = be_clipboard);
// Consume only successfully moved roots, and never a newer clipboard selection.
void FinishFilePaste(const ClipboardPaste &,
                     const std::vector<std::string> &moved,
                     BClipboard *clipboard = be_clipboard);
} // namespace Listi
