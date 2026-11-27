// SPDX-License-Identifier: MIT
#pragma once
#include "FileOperation.h"
#include <Message.h>
#include <initializer_list>
namespace Listi {
// Freeze local entry identities before a question or queue delay. Remote
// providers retain their own capability/metadata checks, not local inode claims.
void FreezeLocalJob(Job &, const std::string &destinationFolder = "");
// Mandatory payload and confirmation fields must be appended before submission.
void RequireJobField(status_t);
BMessage EncodeJob(const Job &, uint32 what);
Job DecodeJob(const BMessage &);
// A completed UI question may submit only its own action, within this app.
std::optional<Job> DecodeSubmittedJob(const BMessage &,
                                      std::initializer_list<Action>);
}
