// SPDX-License-Identifier: MIT
#pragma once
#include <Messenger.h>
#include <memory>
#include <string>

class BView;

namespace Listi {
class Shelf;
struct Job;
Job FlatSelection(const BView &view);
constexpr uint32 kFlatView = 'flvw', kFlatContext = 'flcx';
BView *CreateFlatView(BMessenger owner, std::shared_ptr<Shelf> shelf,
                      const char *root, int32 pane);
std::string FlatSelectedFile(const BView &view);
}
