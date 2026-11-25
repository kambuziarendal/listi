// SPDX-License-Identifier: MIT
#include "AutomaticHistory.h"

#include <Entry.h>
#include <Roster.h>

namespace Listi {
status_t LaunchHistoryHelper(bool showSettings) {
  entry_ref ref;
  auto status = get_ref_for_path(kHistoryHelperPath, &ref);
  if (status != B_OK)
    return status;
  const char *argv[] = {"--settings", nullptr};
  status = be_roster->Launch(&ref, showSettings ? 1 : 0,
                             showSettings ? argv : nullptr);
  return status == B_ALREADY_RUNNING ? B_OK : status;
}
}
