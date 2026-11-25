// SPDX-License-Identifier: MIT
#pragma once
#include "WorkspaceLayout.h"
#include <Messenger.h>
class BWindow;
struct Preferences;
namespace Listi {
inline constexpr uint32 kGuideChoice = 'wlay';
BMessenger OpenLayoutIntroduction(BWindow &, BMessenger previous,
                                  const Preferences &settings, bool firstRun);
}
