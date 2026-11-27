// SPDX-License-Identifier: MIT
#pragma once
#include "FileOperation.h"

namespace Listi {
// Prepare a transfer from any UI source without changing files or refreshing
// stale identities. Only image drops may opt into a same-folder target.
Job PrepareTransfer(Job job, bool allowSameFolder = false);
// Worker-only: inspect projected merges for the whole selection before writing.
void CheckTransferOutputTrees(const std::vector<std::string> &sources,
                              const std::string &destination,
                              const std::function<bool()> &proceed);
bool InTransferSourceFolder(const Job &job);
}
