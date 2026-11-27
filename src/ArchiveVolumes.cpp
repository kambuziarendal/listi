// SPDX-License-Identifier: MIT
#include "ArchiveVolumes.h"
#include "ArchivePasswords.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <stdexcept>
#include <dirent.h>
#include <map>
#include <memory>
#include <regex>
#include <sys/stat.h>
namespace Listi {
namespace {
std::string Lower(std::string s) {
  for (auto &c : s)
    c = std::tolower(static_cast<unsigned char>(c));
  return s;
}
struct Name {
  std::string family;
  int kind = 0, index = 0;
};
Name Parse(const std::string &name) {
  static const std::regex seven(R"((.*\.7z)\.([0-9]{3,6}))", std::regex::icase);
  static const std::regex rar(R"((.*)\.part([0-9]{1,6})\.rar)",
                              std::regex::icase);
  static const std::regex old(R"((.*)\.([r-z])([0-9]{2}))", std::regex::icase);
  std::smatch m;
  Name n;
  if (std::regex_match(name, m, seven)) {
    n = {Lower(m[1]), 1, std::stoi(m[2])};
  } else if (std::regex_match(name, m, rar)) {
    n = {Lower(m[1]), 2, std::stoi(m[2])};
  } else if (std::regex_match(name, m, old)) {
    n = {Lower(m[1]), 3,
         1 + (std::tolower(m[2].str()[0]) - 'r') * 100 + std::stoi(m[3])};
  } else if (name.size() > 4 && Lower(name.substr(name.size() - 4)) == ".rar")
    n = {Lower(name.substr(0, name.size() - 4)), 3, 0};
  return n;
}
ArchiveVolume Capture(const std::string &path) {
  struct stat st;
  if (lstat(path.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_size < 0)
    throw std::runtime_error(
        "Archive part is missing or is not a regular file: " + path);
  return {path, ArchivePasswordIdentity(st), st.st_size, st.st_dev, st.st_ino};
}
} // namespace
ArchiveVolumes FindArchiveVolumes(const std::string &path) {
  ArchiveVolumes out;
  auto slash = path.find_last_of('/');
  auto folder = slash == std::string::npos ? "." : path.substr(0, slash);
  if (folder.empty())
    folder = "/";
  auto leaf = path.substr(slash == std::string::npos ? 0 : slash + 1);
  auto selected = Parse(leaf);
  if (!selected.kind) {
    out.files.push_back(Capture(path));
    return out;
  }
  const auto requested = Capture(path);
  std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(folder.c_str()),
                                                closedir);
  if (!dir)
    throw std::runtime_error("Cannot inspect archive parts in this folder.");
  std::map<int, std::string> found;
  for (;;) {
    errno = 0;
    auto entry = readdir(dir.get());
    if (!entry) {
      if (errno)
        throw std::runtime_error("Cannot finish inspecting archive parts.");
      break;
    }
    auto name = Parse(entry->d_name);
    if (name.kind != selected.kind || name.family != selected.family)
      continue;
    if (!found.emplace(name.index, entry->d_name).second)
      throw std::runtime_error("Duplicate archive part number.");
    if (found.size() > 256)
      throw std::runtime_error("Archive has more than 256 parts; this set "
                               "exceeds the current reader limit.");
  }
  const int first = selected.kind == 3 ? 0 : 1;
  if (found.empty() || found.begin()->first != first)
    throw std::runtime_error("The first archive part is missing. Keep all "
                             "parts together in one folder.");
  int expected = first;
  for (auto &item : found) {
    if (item.first != expected++)
      throw std::runtime_error(
          "Archive part " + std::to_string(expected - 1) +
          " is missing. Keep all parts together in one folder.");
    out.files.push_back(
        Capture(folder + (folder == "/" ? "" : "/") + item.second));
  }
  if (!found.count(selected.index) ||
      out.files[selected.index - first].identity != requested.identity)
    throw std::runtime_error("Selected archive part disappeared.");
  out.kind = selected.kind == 1 ? ArchiveVolumes::Joined7z
             : (selected.kind == 2 || out.files.size() > 1)
                 ? ArchiveVolumes::Rar
                 : ArchiveVolumes::Single;
  return out;
}
std::string ArchiveVolumes::Fingerprint() const {
  std::string out = std::to_string(kind) + ":";
  for (const auto &file : files)
    out += std::to_string(file.path.size()) + ":" + file.path + ":" +
           file.identity + ";";
  return out;
}
void ArchiveVolumes::Verify() const {
  for (const auto &file : files) {
    struct stat st;
    if (lstat(file.path.c_str(), &st) || !S_ISREG(st.st_mode) ||
        ArchivePasswordIdentity(st) != file.identity)
      throw std::runtime_error("Archive changed: part modified or missing: " +
                               file.path);
  }
}
} // namespace Listi
