// SPDX-License-Identifier: MIT
#include "FileSearch.h"
#include "UnavailableVolume.h"
#include "VirtualFile.h"
#include <Application.h>
#include <FindDirectory.h>
#include <fs_index.h>
#include <Node.h>
#include <Path.h>
#include <Volume.h>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
using namespace Listi;
int main() {
  BApplication app("application/x-vnd.Kambuzi-Listi-SearchTest");
  int checks = 0, failed = 0;
  auto check = [&](bool ok, const char *label) {
    ++checks; failed += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << label << std::endl;
  };
  char pattern[] = "/boot/home/listi-search-test-XXXXXX";
  char *created = mkdtemp(pattern);
  if (!created) return 2;
  std::string root = created, sibling = root + "-outside";
  mkdir((root + "/nested").c_str(), 0700); mkdir(sibling.c_str(), 0700);
  auto file = [](const std::string &path) { std::ofstream(path) << "synthetic search content\n"; };
  file(root + "/same.txt"); file(root + "/nested/same.txt");
  file(root + "/literal[1].txt"); file(root + "/Å space.txt"); file(sibling + "/same.txt");
  symlink(sibling.c_str(), (root + "/linked directory").c_str());
  symlink("missing", (root + "/broken link").c_str());
  std::vector<SearchHit> hits;
  SearchRequest request; request.root = root; request.expression = "same";
  auto run = [&] { hits.clear(); return SearchFiles(request, [] {return true;}, [&](const SearchHit &h) {hits.push_back(h); return true;}); };
  auto result = run();
  check(result.error.empty() && result.matched == 2 && hits.size() == 2, "recursive names find duplicate basenames in separate folders");
  check(hits[0].path != hits[1].path && SameSearchEntry(hits[0]), "results retain full original path and node identity");
  bool outside = false; for (const auto &h : hits) outside |= h.path.find(sibling) == 0;
  check(!outside, "directory symlink never traversed");
  request.expression = "SAME"; check(run().matched == 2, "case-insensitive literal name search");
  request.expression = "[1]"; check(run().matched == 1, "name search does not interpret wildcards");
  request.expression = "Å space"; check(run().matched == 1, "UTF-8 and spaces survive result identity");
  request.expression = "å space"; check(run().matched == 1, "native case folding matches UTF-8 name");
  request.expression = "broken link"; check(run().matched == 1 && SameSearchEntry(hits[0]), "broken symlink is a result, never dereferenced");
  request.expression = "same"; request.resultLimit = 1;
  result = run(); check(result.limited && result.matched == 1, "result limit explicitly incomplete, no overflow");
  request.resultLimit = 5000; request.visitLimit = 1;
  result = run(); check(result.limited && result.visited == 1, "visit limit bounds search work");
  request.visitLimit = 250000;
  result = SearchFiles(request, [] {return false;}, [&](const SearchHit &) {return false;});
  check(result.cancelled && !result.visited && !result.matched, "pre-cancelled request does not traverse");
  bool proceed = true; hits.clear();
  result = SearchFiles(request, [&] {return proceed;}, [&](const SearchHit &h) {hits.push_back(h); proceed = false; return true;});
  check(result.cancelled && hits.size() == 1, "mid-search cancellation stops before subsequent result");
  result = SearchFiles(request, [] {return true;}, [&](const SearchHit &) {return false;});
  check(result.cancelled && !result.matched, "failed delivery cannot claim complete results");
  request.root = root + "/missing"; check(!run().error.empty(), "missing root is a failure, not empty success");
  request.root = root + "/same.txt"; check(!run().error.empty(), "file root refused");
  request.root = "smb://host/share"; check(!run().error.empty(), "unsupported provider refused explicitly");
  request.root = root; request.expression = ""; check(!run().error.empty(), "empty expression refused");
  request.expression = std::string(4097, 'x'); check(!run().error.empty(), "oversized expression refused");
  request.expression = "same"; run(); auto old = hits[0];
  rename(old.path.c_str(), (old.path + ".old").c_str()); file(old.path);
  check(!SameSearchEntry(old), "replacement at same path is not original result");
  unlink(old.path.c_str()); rename((old.path + ".old").c_str(), old.path.c_str());
  std::string stage = root + "/.Listi-archive-stage-search-test";
  mkdir(stage.c_str(), 0700);
  file(stage + "/same.txt");
  struct stat stageIdentity;
  if (lstat(stage.c_str(), &stageIdentity))
    return 2;
  RegisterArchiveStage(stage, stageIdentity.st_dev, stageIdentity.st_ino);
  check(run().matched == 2, "name search excludes archive staging contents");
  request.predicate = true; request.expression = "name==\"same.txt\"";
  result = run(); check(result.error.empty() && result.matched == 2, "native BFS name predicate restricted to selected folder");
  outside = false; for (const auto &h : hits) outside |= h.path.find(sibling) == 0;
  check(!outside, "BFS folder boundary excludes sibling sharing root prefix");
  bool privateHit = false;
  for (const auto &h : hits) privateHit |= h.path.find(stage) == 0;
  check(!privateHit, "BFS results exclude staged files matching the query");
  request.expression = "name==\".Listi-archive-stage-search-test\"";
  result = run();
  check(result.error.empty() && !result.matched,
        "BFS results exclude the staging directory itself");
  ForgetArchiveStage(stage);
  check(run().matched == 1, "unregistered staging directory is visible again");
  unlink((stage + "/same.txt").c_str());
  rmdir(stage.c_str());
  request.expression = "(name==\"same.txt\")&&(size>0)";
  result = run(); check(result.error.empty() && result.matched == 2, "native BFS typed size and AND predicate");
  request.expression = "(name==\"same.txt\")||(name==\"literal[1].txt\")";
  result = run(); check(result.error.empty() && result.matched >= 2, "native BFS OR syntax accepted");
  struct stat fixtureStat; stat(root.c_str(), &fixtureStat);
  std::string index = "Listi:search-test-" + std::to_string(getpid());
  bool ownsIndex = fs_create_index(fixtureStat.st_dev, index.c_str(), B_STRING_TYPE, 0) == 0;
  check(ownsIndex, "create isolated owned BFS test index");
  BNode typed((root + "/same.txt").c_str()); BString attribute("native-search-attribute");
  check(typed.WriteAttrString(index.c_str(), &attribute) == B_OK, "synthetic indexed attribute written");
  request.expression = index + "==\"native-search-attribute\"";
  result = run(); check(result.error.empty() && result.matched == 1, "native indexed attribute query returns original");
  typed.RemoveAttr(index.c_str());
  check(ownsIndex && fs_remove_index(fixtureStat.st_dev, index.c_str()) == 0, "only owned test index removed");
  request.expression = "((invalid";
  check(!run().error.empty(), "invalid BFS syntax reported, not empty success");
  request.expression = "name==\"listi-no-match-18972\"";
  result = run(); check(result.error.empty() && result.matched == 0, "empty BFS result completes normally");
  struct stat st; stat(root.c_str(), &st); BVolume volume(st.st_dev); BPath trash;
  if (find_directory(B_TRASH_DIRECTORY, &trash, true, &volume) != B_OK) return 3;
  std::string trashed = std::string(trash.Path()) + "/listi-search-owned-" + std::to_string(getpid());
  file(trashed); request.root = trash.Path(); request.expression = "listi-search-owned-"; request.predicate = false;
  check(!run().error.empty(), "search inside Trash requires explicit inclusion");
  request.includeTrash = true; result = run();
  check(result.error.empty() && result.matched == 1, "explicit Trash scope finds owned fixture");
  request.predicate = true; request.expression = "name==\"listi-search-owned-" + std::to_string(getpid()) + "\"";
  check(run().matched == 1, "BFS query includes Trash when requested");
  request.root = "/boot"; request.includeTrash = false;
  check(run().matched == 0, "BFS query excludes Trash by default");
  unlink(trashed.c_str());
  InterruptedVolumes().Block(root + "/nested");
  request.root = root; request.predicate = false; request.expression = "same";
  result = run();
  check(result.error.empty() && result.matched == 1 && result.skipped == 1 &&
            hits.front().path == root + "/same.txt",
        "recursive search reports an unavailable branch without losing healthy results");
  request.predicate = true; request.expression = "name==\"same.txt\"";
  result = run();
  check(result.error.empty() && result.matched == 1 && result.skipped == 1 &&
            hits.front().path == root + "/same.txt",
        "BFS search reports in-scope matches omitted on an unavailable branch");
  for (const char *path : {"same.txt", "nested/same.txt", "literal[1].txt", "Å space.txt", "linked directory", "broken link"}) unlink((root + "/" + path).c_str());
  rmdir((root + "/nested").c_str()); rmdir(root.c_str());
  unlink((sibling + "/same.txt").c_str()); rmdir(sibling.c_str());
  std::cout << checks << " checks, " << failed << " failures\n";
  return failed ? 1 : 0;
}
