// SPDX-License-Identifier: MIT
#include "HistoryPaneHint.h"
#include "FolderHistory.h"
#include "HistorySettings.h"
#include "ReadWorker.h"
#include <algorithm>
#include <stdexcept>

namespace Listi {
namespace {
class HintReader : public ReadWorker {
public:
  HintReader(BMessenger reply, const std::array<std::string, 2> &paths)
      : ReadWorker("folder history status"), target(reply), visible(paths) {}
  void MessageReceived(BMessage *message) override {
    if (message->what != kReadHistoryConfig) {
      ReadWorker::MessageReceived(message);
      return;
    }
    BMessage reply(kReadHistoryConfig);
    try {
      auto config = ReadHistorySettings();
      reply.AddBool("enabled", config.GetBool("enabled", false));
      for (auto pair : HistoryPairs(config)) {
        const char *source = pair.GetString("path", 0, "");
        if (std::find(visible.begin(), visible.end(), source) != visible.end()) {
          int32 count = kUnavailableVersions;
          try {
            HistoryStore store(source, pair.GetString("path", 1, ""));
            if (store.FolderIdentity().HasSameData(pair)) {
              auto points = store.Points();
              type_code type;
              int32 found = 0;
              count = points.GetInfo("point", &type, &found) == B_OK ? found : 0;
            }
          } catch (const std::exception &) {
            // An unavailable device is not an empty history.
          }
          pair.AddInt32("versions", count);
        }
        reply.AddMessage("pair", &pair);
      }
    } catch (const std::exception &) {
      // Explicitly opening the browser gives the full settings error.
    }
    target.SendMessage(&reply, (BHandler *)nullptr, 500000);
    Quit();
  }
private:
  BMessenger target;
  const std::array<std::string, 2> visible;
};
}

bool RequestHistoryConfig(BMessenger target, const std::array<std::string, 2> &visible) {
  ReadWorker *reader = new HintReader(target, visible);
  if (!StartReadWorker(reader)) return false;
  if (BMessenger(reader).SendMessage(kReadHistoryConfig) == B_OK) return true;
  RetireReadWorker(reader);
  return false;
}
}
