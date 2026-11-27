// SPDX-License-Identifier: MIT
#pragma once
#include "MainWindow.h"
namespace Listi {
class WorkingWindow;
constexpr uint32 kSaveWorkingSet = 'wksa', kOpenWorkingSet = 'wkop',
                 kCaptureWorkingWindow = 'wkcp';
BMessage CaptureWorkingSet(const std::string &label, const std::vector<BMessenger> &selected);
void QueueWorkingSet(const BMessage &set, const BrowserOpener &open);
void ShowWorkingSets(WorkingWindow &, bool saving, BrowserOpener);
}
