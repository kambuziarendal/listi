// SPDX-License-Identifier: MIT
#pragma once
#include <map>
#include <string>

class DynamicLibrary;

namespace Listi {
struct Hooks;
void VerifyDataIsoOutput(DynamicLibrary &, int output,
                         std::map<std::string, std::string> members,
                         const Hooks &);
void CopyDataIsoSeal(int source, int destination);
void SealDataIso(int fd, const Hooks &);
void RequireVerifiedDataIso(int fd, const Hooks &);
}
