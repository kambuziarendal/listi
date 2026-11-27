// SPDX-License-Identifier: MIT
#pragma once
#include "FileOperation.h"
#include <string>
#include <vector>
namespace Listi {
struct ArchiveFormat {
  std::string id, label, extension;
  bool compression;
  bool encryption = false;
  bool encryptedNames = false;
};
std::vector<ArchiveFormat> WritableArchiveFormats();
Job PrepareArchiveJob(bool extract, const std::vector<std::string> &sources);
Job PrepareArchiveEditTarget(const std::string &archive,
                             const std::string &internalFolder = "/",
                             const Hooks &hooks = {});
void SetArchiveEditAdditions(Job &, const std::vector<std::string> &add);
Job PrepareArchiveEditJob(const std::string &archive,
                          const std::vector<std::string> &add,
                          const std::vector<std::string> &remove,
                          const std::string &internalFolder = "/");
Job PrepareArchiveCopy(const Job &transfer);
Job PrepareArchiveMutation(const Job &transfer);
Job PrepareArchiveMemberChange(const Job &transfer, Job inspectedTarget);
Job PrepareArchiveMove(const Job &transfer);
void FreezeArchiveTargets(Job &transfer);
void SetArchiveDestination(Job &, const std::string &folder,
                           const std::string &name);
Result RunArchiveJob(const Job &, const Hooks &);
}
