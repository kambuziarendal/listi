// SPDX-License-Identifier: MIT
#pragma once
#include "FileOperation.h"

namespace Listi {
// Prepare a transfer from any UI source without changing files or refreshing
// stale identities. Only image drops may opt into a same-folder target.
Job PrepareTransfer(Job job, bool allowSameFolder = false);
// Check the complete local paste before reserving or queuing separate jobs.
void CheckTransferOutputTrees(const std::vector<std::string> &sources,
                              const std::string &destination);
bool InTransferSourceFolder(const Job &job);
}
