// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <stdexcept>
#include <sys/stat.h>

namespace Listi {
inline struct stat FavoriteFolderIdentity(const std::filesystem::path &folder) {
  struct stat info{};
  if (!folder.is_absolute() || stat(folder.c_str(), &info) != 0 ||
      !S_ISDIR(info.st_mode))
    throw std::runtime_error("The local folder is not available.");
  return info;
}

enum class FavoriteResult { Added, AlreadyFavorite, NameInUse };

inline FavoriteResult AddFilePanelFavorite(const std::filesystem::path &settings,
    const std::filesystem::path &folder, const std::string &name,
    dev_t device, ino_t inode) {
  namespace fs = std::filesystem;
  const auto current = FavoriteFolderIdentity(folder);
  if (current.st_dev != device || current.st_ino != inode)
    throw std::runtime_error("The folder changed while the question was open.");
  if (name.empty() || name == "." || name == ".." || name.find('/') != name.npos)
    throw std::runtime_error("Choose a single favorite name without a slash.");

  const auto favorites = settings / "Tracker" / "Go";
  fs::create_directories(favorites);
  for (const auto &item : fs::directory_iterator(favorites)) {
    struct stat target{};
    if (item.is_symlink() && stat(item.path().c_str(), &target) == 0 &&
        target.st_dev == device && target.st_ino == inode)
      return FavoriteResult::AlreadyFavorite;
  }
  std::error_code error;
  fs::create_symlink(folder, favorites / name, error);
  if (error == std::errc::file_exists)
    return FavoriteResult::NameInUse;
  if (error)
    throw std::runtime_error("Could not add file panel favorite: " + error.message());
  return FavoriteResult::Added;
}
}
