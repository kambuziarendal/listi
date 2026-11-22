// SPDX-License-Identifier: MIT
#pragma once
#include <Message.h>
#include <string>
namespace Listi {
// Bounded regular-file read. Missing file returns an empty message.
BMessage LoadMessageStore(const std::string &file);
// Failures before rename leave the previous file untouched. A later directory
// flush error explicitly reports that replacement occurred.
void SaveMessageStore(const std::string &file, const BMessage &message);
}
