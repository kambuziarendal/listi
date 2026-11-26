// SPDX-License-Identifier: MIT
#include "PaneModel.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
static void check(bool ok, const char* label) {
  if (!ok) throw std::runtime_error(label);
  printf("PASS %s\n", label);
}
int main() {
  PaneModel scan;
  for (const auto *name : {"match-b", "other", "match-a"}) {
    auto row = std::make_unique<PaneEntry>();
    row->path = row->name = name;
    scan.AddScanned(std::move(row), "match", true);
  }
  check(scan.Visible().size() == 2 && scan.Visible()[0]->name == "match-b" &&
        scan.HiddenCount() == 1, "incoming rows filter without sorting the previous batch");
  scan.Sort(scan.Sorting());
  check(scan.Selected() == std::vector<std::string>{"match-a", "match-b"},
        "scan completion sorts visible rows and keeps restored selections");
  PaneModel model;
  std::vector<std::string> paths;
  for (int i = 0; i < 10000; ++i) {
    auto e = std::make_unique<PaneEntry>();
    char name[24]; snprintf(name, sizeof(name), "item-%05d", i);
    e->name = name; e->path = std::string("/fixture/") + name;
    e->device = 1;
    e->node = i + 1;
    paths.push_back(e->path); model.Add(std::move(e));
  }
  auto parent = std::make_unique<PaneEntry>();
  parent->name = ".."; parent->path = "/fixture"; parent->parent = true;
  model.Add(std::move(parent)); model.Filter("");
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) model.SetSelection(paths, paths.back());
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
  printf("20 full selections of 10000 entries: %lld us\n", (long long)us);
  check(model.Selected() == paths, "large selection preserves visible order");
  bool tooMany = false;
  try {
    model.SelectedLocalIdentities(1024);
  } catch (const std::runtime_error &) {
    tooMany = true;
  }
  check(tooMany, "a bounded action refuses an oversized selection before building its node map");
  check(!model.OnlySelected(), "multiple rows are not a single inspection target");
  check(model.Focused()->path == paths.back(), "focus preserved");
  model.SetSelection({paths[0], paths[0], "/fixture", "/missing"}, paths[0]);
  check(model.Selected() == std::vector<std::string>{paths[0]}, "duplicate, parent and absent entries excluded");
  check(model.OnlySelected() && model.OnlySelected()->path == paths[0], "one visible target");
  const auto nodes = model.SelectedLocalIdentities();
  check(nodes.size() == 1 && nodes.at(paths[0]).device == 1 &&
            nodes.at(paths[0]).inode == 1,
        "file operations inherit the original identity from the selected visible row");
  model.RestoreSelection({paths[1], paths[1]});
  check(model.Selected().size() == 2, "restore extends existing selection");
  check(model.MatchPattern("*", false) == paths.size(), "wildcard excludes the parent row");
  const auto beforePreview = model.Selected();
  check(model.MatchPattern("item-0000?", false) == 10 &&
            model.MatchPattern("ITEM-*", false) == 0 &&
            model.MatchPattern("fixture/*", false) == 0,
        "wildcard uses case-sensitive whole names, not paths");
  check(model.Selected() == beforePreview, "preview leaves selection untouched");
  check(model.MatchPattern("item-0000?", true) == 10 &&
            model.Selected() == std::vector<std::string>(paths.begin(), paths.begin() + 10) &&
            model.Focused()->path == paths.front(),
        "confirmed pattern replaces selection and focuses the first match");
  const auto matched = model.Selected();
  check(model.MatchPattern("absent*", true) == 0 && model.Selected() == matched,
        "zero matches preserve the current selection");
  model.Filter("item-00000");
  check(model.MatchPattern("*", true) == 1 &&
            model.Selected() == std::vector<std::string>{paths[0]},
        "pattern selection respects the current literal filter");
  model.SetSelection(paths, paths[0]);
  check(model.Selected() == std::vector<std::string>{paths[0]}, "hidden entries excluded");
  auto duplicate = std::make_unique<PaneEntry>();
  duplicate->name = "item-00000";
  duplicate->path = paths[0];
  duplicate->device = 1;
  duplicate->node = 1;
  model.Add(std::move(duplicate));
  model.Filter("item-00000");
  check(model.Selected().size() == 2 && !model.OnlySelected(), "same-path rows are ambiguous");
  bool ambiguous = false;
  try {
    model.SelectedLocalIdentities();
  } catch (const std::runtime_error &) {
    ambiguous = true;
  }
  check(ambiguous, "ambiguous local selection cannot authorize a file operation");
  model.SetSelection({}, "");
  check(model.Selected().empty() && model.Focused() == nullptr, "empty selection clears focus");

  PaneModel remote;
  auto member = std::make_unique<PaneEntry>();
  member->name = "child";
  member->path = "sftp://example.invalid/child";
  member->kind = 2;
  member->modified = 128;
  member->modifiedKnown = true;
  member->modifiedFraction = "123";
  member->sizeKnown = false;
  member->providerIdentity = "node-5";
  remote.Add(std::move(member));
  remote.Filter("");
  remote.Select(1);
  check(remote.Selected().size() == 1 && remote.SelectedLocalIdentities().empty(),
        "virtual selections need no local node identity");
  const auto selected = remote.SelectedRemoteEntries().at("sftp://example.invalid/child");
  check(selected.link && !selected.directory && !selected.sizeKnown && selected.modifiedKnown &&
            selected.modified == 128 && selected.modifiedFraction == "123" &&
            selected.providerIdentity == "node-5",
        "remote deletion binds the type and available revision from the displayed row");
}
