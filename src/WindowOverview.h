// SPDX-License-Identifier: MIT
#pragma once
#include <Messenger.h>
#include <Window.h>
#include <string>
#include <vector>

namespace Listi {
inline constexpr uint32 kNameWindow = 'wnam', kUseFolderName = 'waut',
                        kOpenWindowOverview = 'wovw', kShowLayoutGuide = 'wgde';

// Only independent working surfaces participate. Prompts, popup menus and
// background loopers are not destinations; their existing ownership is unchanged.
class WorkingWindow : public BWindow {
public:
  WorkingWindow(BRect, const char *title, const char *kind, uint32 flags);
  ~WorkingWindow() override;
  int64 WindowId() const { return id; }
  void SetWorkingSetName(const std::string &value) { setName = value; Publish(); }
  const std::string &Alias() const { return alias; }
  void SetAlias(const std::string &value) { alias = value; Publish(); }
  void Describe(const std::string &automaticTitle, const std::string &location,
                bool modified = false);
  void MessageReceived(BMessage *) override;
  bool HandleWindowAction(BMessage *);

private:
  std::string automatic, location, alias, setName;
  const std::string kind;
  int64 id;
  bool modified = false;
  BMessenger nameDialog;
  void Publish();
};

struct BrowserChoice { std::string label; BMessenger target; int64 id; };
std::vector<BrowserChoice> WorkingSetChoices();
BMessenger OpenWindowOverview(WorkingWindow &, BMessenger previous);
}
