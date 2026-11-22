// SPDX-License-Identifier: MIT
#pragma once
#include "FileOperation.h"
#include <stdexcept>

namespace Listi {
// Archive moves and permanent local merge cleanup must stay on one volume.
inline void RequireSameMoveVolume(int32_t source, int32_t destination) {
  if (source < 0 || destination < 0 || source != destination)
    throw std::runtime_error(
        "Move between volumes is not supported. Use Copy instead; keep the "
        "originals until you have checked the copy on the destination volume.");
}

inline void CheckMoveVolumes(const Job &job) {
  for (const auto &entry : job.expectedSources)
    RequireSameMoveVolume(entry.second.device, job.expectedDestination.device);
}
} // namespace Listi
