// SPDX-License-Identifier: MIT
#pragma once
#include "FileClipboard.h"
#include <Message.h>
class PaneModel;
namespace Listi {
BMessage MakePaneFileDrag(const PaneModel &);
// Local refs for Tracker interoperability, with a private identity snapshot.
BMessage MakeFileDrag(const std::vector<std::string> &paths,
                      const std::map<std::string, NodeIdentity> &expected = {});
// Captures identities at drop time for external refs, at drag time for Listi.
Job PrepareFileDrop(const BMessage &, const std::string &destination,
                    bool allowSameFolder = false, std::string *missingLeaf = nullptr);
// parent is the frozen, validated drop onto the existing parent.
Job CreateFileDropDestination(Job parent, const std::string &leaf);
bool CanMoveFileDrop(const Job &job);
}
