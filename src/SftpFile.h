// SPDX-License-Identifier: MIT
#pragma once
#include "VirtualFile.h"
#include <set>

namespace Listi {
struct UnsupportedSftpEntry : std::runtime_error {
  using std::runtime_error::runtime_error;
};
std::vector<FileEntry> ListSftp(const Location &, const TransferControl &);
std::set<std::string> ListSftpNames(const Location &, const TransferControl &);
SftpPeer BindSftpPeer(const Location &, const TransferControl &);
std::string CanonicalSftpPath(const Location &, const TransferControl &);
bool StatSftp(const Location &, FileEntry &, const TransferControl &);
void ReadSftp(const Location &, const DataSink &, const TransferControl &);
void ReadSftpPrefix(const Location &, const FileEntry &expected, size_t,
                    const DataSink &, const TransferControl &);
void UploadSftp(const std::string &, const Location &, const TransferControl &);
void ChangeSftp(const Location &, const std::string &action, const std::string &destination,
                const TransferControl &);
}
