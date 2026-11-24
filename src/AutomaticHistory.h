// SPDX-License-Identifier: MIT
#pragma once

#include <SupportDefs.h>

namespace Listi {
inline constexpr uint32 kAutomaticHistorySettings = 'ahst';
inline constexpr char kHistoryHelperSignature[] = "application/x-vnd.Kambuzi-ListiHistory";
inline constexpr char kHistoryHelperPath[] = "/boot/system/lib/listi/ListiHistory";
inline constexpr char kHistoryDeskbarItem[] = "Listi Folder History";

status_t LaunchHistoryHelper(bool showSettings);
}
