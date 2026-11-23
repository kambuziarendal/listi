// SPDX-License-Identifier: MIT
#pragma once
#include <Rect.h>
#include <string>
class BWindow;
namespace Listi {
BWindow *OpenRecoveryReview(BRect owner, const std::string &inventory = {});
}
