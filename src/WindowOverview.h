// SPDX-License-Identifier: MIT
#pragma once
#include <Messenger.h>
#include <Window.h>
#include <memory>
#include <string>
#include <vector>

namespace Listi {
inline constexpr uint32 kNameWindow = 'wnam', kUseFolderName = 'waut',
                        kOpenWindowOverview = 'wovw', kShowLayoutGuide = 'wgde';

// Only independent working surfaces participate. Prompts, popup menus and
// background loopers are not destinations; their existing ownership is unchanged.
struct Job;
struct NodeIdentity;
class ReadWorker;
class WorkingWindow : public BWindow {
public:
  WorkingWindow(BRect, const char *title, const char *kind, uint32 flags);
  ~WorkingWindow() override;
  int64 WindowId() const { return id; }
  void SetWorkingSetName(const std::string &value) { setName = value; Publish(); }
  const std::string &Alias() const { return alias; }
  void SetAlias(const std::string &value) { alias = value; Publish(); }
  void SetCopyFolder(const std::string &path, const NodeIdentity &);
  void PickCopyFolder(Job job);
  bool HandleCopyFolder(BMessage &, Job &);
  void Describe(const std::string &automaticTitle, const std::string &location,
                bool modified = false);
  void MessageReceived(BMessage *) override;
  bool HandleWindowAction(BMessage *);

private:
  std::string automatic, location, alias, setName, copyFolder;
  const std::string kind;
  int64 id;
  bool modified = false;
  BMessenger nameDialog;
  std::unique_ptr<Job> pendingCopy;
  ReadWorker *copyProbe = nullptr;
  int64 copyRequest = 0, copyFolderRevision = 0;
  int32 copyDevice = -1;
  int64 copyInode = -1;
  void Publish();
};

struct BrowserChoice { std::string label; BMessenger target; int64 id; };
std::vector<BrowserChoice> WorkingSetChoices();
BMessenger OpenWindowOverview(WorkingWindow &, BMessenger previous);
}
