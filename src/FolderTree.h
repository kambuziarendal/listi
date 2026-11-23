// SPDX-License-Identifier: MIT
#pragma once
#include <Messenger.h>
#include <View.h>
#include <atomic>
#include <map>
#include <memory>
#include <string>
namespace Listi { class ReadWorker; }
class TreeList;
class TreeItem;
class FolderTree : public BView {
public:
  explicit FolderTree(BHandler *navigationTarget);
  ~FolderTree();
  void AttachedToWindow() override;
  void MessageReceived(BMessage *) override;
  void ShowPath(const std::string &);
  std::string PathAt(BPoint screen) const;
  void NavigateSelection(const std::string &);
  void ExpandPath(const std::string &);
  void Reset();

private:
  BHandler *navigationTarget;
  TreeList *list;
  Listi::ReadWorker *reader = nullptr;
  std::shared_ptr<std::atomic<int64>> generation;
  std::map<std::string, TreeItem *> items;
  std::string root, desired;
  void Clear();
  void Follow();
};
