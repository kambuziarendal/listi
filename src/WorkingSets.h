// SPDX-License-Identifier: MIT
#pragma once
#include "MainWindow.h"
namespace Listi {
class WorkingWindow;
constexpr uint32 kSaveWorkingSet = 'wksa', kOpenWorkingSet = 'wkop',
                 kCaptureWorkingWindow = 'wkcp';
void ShowWorkingSets(WorkingWindow &, bool saving, BrowserOpener);
}
