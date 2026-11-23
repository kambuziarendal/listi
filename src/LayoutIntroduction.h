// SPDX-License-Identifier: MIT
#pragma once
#include "WorkspaceLayout.h"
#include <Messenger.h>
class BWindow;
namespace Listi {
inline constexpr uint32 kGuideChoice = 'wlay';
BMessenger OpenLayoutIntroduction(BWindow &, BMessenger previous,
                                  Workspace current, bool firstRun);
}
