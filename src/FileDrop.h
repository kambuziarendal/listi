// SPDX-License-Identifier: MIT
#pragma once
#include "FileClipboard.h"
#include <Message.h>
class Pane;
class PaneModel;
namespace Listi {
BMessage MakePaneFileDrag(const PaneModel &);
// Use the receiving column's listing, even when its pane is not active.
void PostPaneFileDrop(Pane &, const PaneModel &receiver, const BMessage &drag,
                      const std::string &destination);
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
