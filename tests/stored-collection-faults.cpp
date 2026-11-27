// SPDX-License-Identifier: MIT
#include "SavedPlaces.h"
#include "SavedSearches.h"
#include "ShelfCollections.h"
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

using namespace Listi;
namespace {
const char *failField = nullptr;
int matching = 0;

void ReadCollections(const std::string &folder, int expected) {
  const auto places = SavedPlaces::Load(folder + "/places");
  const auto searches = SavedSearches::Load(folder + "/searches");
  const auto collections = ShelfCollections::Load(folder + "/shelf");
  const auto *shelf = collections.Find("Shelf");
  assert(places.Items().size() == size_t(expected));
  assert(searches.Items().size() == size_t(expected));
  assert(shelf && shelf->entries.size() == size_t(expected));
  for (int i = 0; i < expected; ++i) {
    const auto name = std::to_string(i);
    const auto path = "/" + name;
    assert(places.Find(path) && places.Find(path)->name == name);
    assert(searches.Find(name) && searches.Find(name)->request.root == path);
    assert(shelf->entries[i].path == path);
  }
}

void Reopen(const char *program, const char *folder, const char *expected) {
  const pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    execl(program, program, folder, expected, nullptr);
    _exit(127);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
}

extern "C" status_t __real__ZN8BMessage10AddMessageEPKcPKS_(BMessage *, const char *, const BMessage *);
extern "C" status_t __wrap__ZN8BMessage10AddMessageEPKcPKS_(BMessage *self, const char *name, const BMessage *item) {
  if (failField && !strcmp(failField, name) && ++matching == 2)
    return B_NO_MEMORY;
  return __real__ZN8BMessage10AddMessageEPKcPKS_(self, name, item);
}

int main(int argc, char **argv) {
  if (argc == 3) {
    ReadCollections(argv[1], atoi(argv[2]));
    return 0;
  }
  char path[] = "/tmp/listi-stored-collections-XXXXXX";
  assert(mkdtemp(path));
  const std::string folder(path);
  SavedPlaces places;
  SavedSearches searches;
  ShelfCollections collections;
  ShelfSnapshot shelf;
  auto append = [&](int i) {
    const auto name = std::to_string(i);
    const auto location = "/" + name;
    places.Add(name, location);
    SavedSearch search;
    search.name = name;
    search.request.root = location;
    search.request.expression = name;
    searches.Put(search, false);
    ShelfEntry entry;
    entry.path = location;
    shelf.entries.push_back(entry);
    collections.Put("Shelf", shelf, i != 0);
  };
  auto save = [&] {
    places.Save(folder + "/places");
    searches.Save(folder + "/searches");
    collections.Save(folder + "/shelf");
  };
  for (int i = 0; i < 3; ++i)
    append(i);
  save();
  Reopen(argv[0], path, "3");
  append(3);
  auto interrupt = [&](const char *field, auto update) {
    failField = field;
    matching = 0;
    bool refused = false;
    try { update(); } catch (const std::runtime_error &) { refused = true; }
    failField = nullptr;
    assert(matching == 2 && refused);
  };
  interrupt("place", [&] { places.Save(folder + "/places"); });
  interrupt("search", [&] { searches.Save(folder + "/searches"); });
  interrupt("entry", [&] { collections.Save(folder + "/shelf"); });
  Reopen(argv[0], path, "3");
  save();
  Reopen(argv[0], path, "4");
  std::filesystem::remove_all(folder);
  std::cout << "PASS interrupted collection writes preserve all old entries across restart and retry\n";
}
