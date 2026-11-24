// SPDX-License-Identifier: MIT
#pragma once

#include <Message.h>
#include <atomic>
#include <memory>
#include <string>

namespace Listi {
inline constexpr uint32 kFolderHistory = 'fhst';
void OpenFolderHistory(const char *source);
bool FolderHistoryBusy();

class HistoryStore {
public:
  HistoryStore(const std::string &source, const std::string &folder);
  ~HistoryStore();
  BMessage Points() const;
  BMessage Browse(const std::string &point) const;
  void Save(const std::atomic_bool &cancel) const;
  void Restore(const std::string &point, const std::string &destination,
               const std::atomic_bool &cancel) const;
private:
  struct State;
  std::unique_ptr<State> state;
};
}
