// SPDX-License-Identifier: MIT
#pragma once
#include <Messenger.h>
#include <Rect.h>
#include <memory>
#include <cstddef>
#include <string>

namespace Listi {
class Shelf;
constexpr uint32 kShowShelf = 'sfsh', kAddToShelf = 'sfad',
                 kRefreshShelf = 'sfrf', kSubmitShelfTransfer = 'sfst',
                 kShelfChanged = 'sfch';
size_t AddShelfDrop(Shelf &, const BMessage &);
void OpenShelfWindow(BMessenger owner, const std::shared_ptr<Shelf> &,
                     BMessenger &window, BRect listerFrame,
                     const std::string &settingsFile = {});
} // namespace Listi
