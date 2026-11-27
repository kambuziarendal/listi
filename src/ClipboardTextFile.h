// SPDX-License-Identifier: MIT
#pragma once
#include "FileClipboard.h"
#include <optional>

namespace Listi {
enum class ClipboardContent { Text, Image };
struct ClipboardFile {
  std::string folder, bytes, suggestedName;
  NodeIdentity destination;
};
bool CanSaveClipboardIn(const std::string &folder);
// File selections (including archive entries) use the existing transfer path.
// Unsupported content is rejected; nothing is saved at this stage.
std::optional<ClipboardContent>
DetectClipboardSaveContent(BClipboard *clipboard = be_clipboard);
ClipboardFile PrepareClipboardFile(const std::string &folder,
                                   ClipboardContent content,
                                   BClipboard *clipboard = be_clipboard,
                                   NodeIdentity expectedDestination = {});
void SaveClipboardFile(const ClipboardFile &, const std::string &name);
}
