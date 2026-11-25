// SPDX-License-Identifier: MIT
#include "FileSearch.h"
#include "VirtualFile.h"
#include <UnicodeChar.h>
#include <Entry.h>
#include <Message.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Query.h>
#include <Volume.h>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
namespace Listi {
void SearchRequest::WriteCriteria(BMessage &message) const {
  message.AddString("root", root.c_str());
  message.AddString("expression", expression.c_str());
  message.AddBool("predicate", predicate);
  message.AddBool("trash", includeTrash);
  WriteSearchFilters(message, filters);
}

SearchRequest SearchRequest::ReadCriteria(const BMessage &message) {
  SearchRequest request;
  request.root = message.GetString("root", "");
  request.expression = message.GetString("expression", "");
  request.predicate = message.GetBool("predicate", false);
  request.includeTrash = message.GetBool("trash", false);
  request.filters = ReadSearchFilters(message);
  return request;
}

namespace {
bool Within(const std::string &path, const std::string &root) {
  return path == root || (root == "/" && path[0] == '/') ||
         path.compare(0, root.size() + 1, root + "/") == 0;
}
std::u32string Fold(const char *text) {
  std::u32string result;
  while (*text)
    result += BUnicodeChar::ToLower(BUnicodeChar::FromUTF8(&text));
  return result;
}
struct Pending {
  std::string path;
  struct stat identity;
  unsigned depth;
};
}
bool SameSearchEntry(const SearchHit &hit) {
  struct stat st;
  return !lstat(hit.path.c_str(), &st) && st.st_dev == hit.device &&
         st.st_ino == hit.node;
}
SearchSummary SearchFiles(const SearchRequest &request,
                          const std::function<bool()> &proceed,
                          const std::function<bool(const SearchHit &)> &deliver,
                          BQuery *liveQuery) {
  SearchSummary out;
  CompiledSearchFilters filters;
  try {
    filters = CompileSearchFilters(request.filters);
  } catch (const std::exception &e) {
    out.error = e.what();
    return out;
  }
  auto alive = [&] {
    if (!proceed())
      out.cancelled = true;
    return !out.cancelled;
  };
  if (!alive())
    return out;
  if (request.root.empty() || request.root[0] != '/' ||
      (request.flat && request.predicate) ||
      (request.expression.empty() &&
       (request.predicate || (!request.flat && !filters.active))) ||
      request.expression.size() > 4096 ||
      request.expression.find('\0') != std::string::npos ||
      request.root.find('\0') != std::string::npos || !request.resultLimit ||
      !request.visitLimit) {
    out.error =
        "Choose a local folder and a name, file filter or BFS formula (up to 4096 bytes).";
    return out;
  }
  char resolved[PATH_MAX];
  struct stat rootStat;
  if (!realpath(request.root.c_str(), resolved) || stat(resolved, &rootStat)) {
    out.error = strerror(errno);
    return out;
  }
  if (!S_ISDIR(rootStat.st_mode)) {
    out.error = "Search scope is not a folder.";
    return out;
  }
  std::string root = resolved;
  BVolume volume(rootStat.st_dev);
  if (volume.InitCheck() != B_OK || !volume.IsPersistent() ||
      volume.IsShared()) {
    out.error = "Choose a folder on a persistent local volume.";
    return out;
  }
  BPath trashPath;
  std::string trash;
  if (find_directory(B_TRASH_DIRECTORY, &trashPath, false, &volume) == B_OK)
    trash = trashPath.Path();
  auto excluded = [&](const std::string &path) {
    return !request.includeTrash && !trash.empty() && Within(path, trash);
  };
  if (excluded(root)) {
    out.error = "This scope is in Trash. Enable Include Trash to search it.";
    return out;
  }
  auto accept = [&](const std::string &path, const struct stat &st,
                    const std::string &relativeFolder) {
    if (!alive())
      return false;
    if (!filters.Matches(path, st))
      return true;
    if (out.matched >= request.resultLimit) {
      out.limited = true;
      return false;
    }
    if (!deliver({path, st.st_dev, st.st_ino, relativeFolder, st.st_size, st.st_mtime})) {
      out.cancelled = true;
      return false;
    }
    ++out.matched;
    return true;
  };
  if (request.predicate) {
    if (!volume.KnowsQuery()) {
      out.error = "This volume does not support BFS queries.";
      return out;
    }
    BQuery snapshot;
    BQuery &query = liveQuery ? *liveQuery : snapshot;
    status_t status = query.SetVolume(&volume);
    if (status == B_OK)
      status = query.SetPredicate(request.expression.c_str());
    if (status == B_OK && alive())
      status = query.Fetch();
    if (!alive())
      return out;
    if (status != B_OK) {
      out.error = std::string("BFS query: ") + strerror(status);
      return out;
    }
    BEntry entry;
    while (alive()) {
      status = query.GetNextEntry(&entry, false);
      if (status == B_ENTRY_NOT_FOUND)
        break;
      if (status != B_OK) {
        out.error = std::string("BFS results: ") + strerror(status);
        break;
      }
      if (out.visited >= request.visitLimit) {
        out.limited = true;
        break;
      }
      ++out.visited;
      BPath path;
      struct stat st;
      if (entry.GetPath(&path) != B_OK || lstat(path.Path(), &st) != 0) {
        ++out.skipped;
        continue;
      }
      if (st.st_dev != rootStat.st_dev || !Within(path.Path(), root) ||
          path.Path() == root || excluded(path.Path()) ||
          IsListiPrivateEntry(path.Path()))
        continue;
      if (!accept(path.Path(), st, {}))
        break;
    }
    return out;
  }
  const auto needle = Fold(request.expression.c_str());
  std::vector<Pending> pending{{root, rootStat, 0}};
  std::set<ino_t> seen;
  while (!pending.empty() && alive() && !out.limited) {
    Pending current = pending.back();
    pending.pop_back();
    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(current.path.c_str()),
                                                  closedir);
    if (!dir) {
      if (current.depth == 0)
        out.error = strerror(errno);
      else
        ++out.skipped;
      continue;
    }
    struct stat opened;
    if (fstat(dirfd(dir.get()), &opened) || opened.st_dev != rootStat.st_dev ||
        opened.st_ino != current.identity.st_ino) {
      ++out.skipped;
      continue;
    }
    if (!seen.insert(opened.st_ino).second)
      continue;
    std::string folder;
    if (request.flat)
      folder = current.depth == 0
                   ? "."
                   : current.path.substr(root == "/" ? 1 : root.size() + 1);
    while (alive()) {
      errno = 0;
      auto item = readdir(dir.get());
      if (!item) {
        if (errno)
          ++out.skipped;
        break;
      }
      if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
        continue;
      if (out.visited >= request.visitLimit) {
        out.limited = true;
        break;
      }
      ++out.visited;
      std::string path =
          current.path + (current.path == "/" ? "" : "/") + item->d_name;
      if (excluded(path) || IsListiPrivateEntry(path))
        continue;
      struct stat st;
      if (lstat(path.c_str(), &st)) {
        ++out.skipped;
        continue;
      }
      if (st.st_dev != rootStat.st_dev) {
        ++out.skipped;
        continue;
      }
      if ((!request.flat || S_ISREG(st.st_mode)) &&
          (needle.empty() ||
           Fold(item->d_name).find(needle) != std::u32string::npos) &&
          !accept(path, st, folder))
        break;
      // Never recurse through symlinks or other mounted volumes. Bound depth,
      // visited entries and result storage, reporting all truncation explicitly.
      if (S_ISDIR(st.st_mode)) {
        if (current.depth >= 256 || pending.size() >= 4096)
          ++out.skipped;
        else
          pending.push_back({path, st, current.depth + 1});
      }
    }
  }
  return out;
}
} // namespace Listi
