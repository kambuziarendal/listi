// SPDX-License-Identifier: MIT
#pragma once
#include "FileOperation.h"
namespace Listi {
// One local entry, frozen before the name question. Duplicate uses its parent;
// Link uses the specified destination folder. No source/clipboard mutation.
Job PrepareNamedEntry(Action, const std::vector<std::string> &selection,
                      const std::string &linkDestination = "",
                      const std::map<std::string, NodeIdentity> &expected = {},
                      NodeIdentity expectedDestination = {});
void ValidateEntryLeaf(const std::string &name);
// Both the dialog and worker validate. The worker also commits exclusively.
void ValidateNewEntryName(const std::string &folder, const std::string &name);
std::string LinkText(const Job &);
}
