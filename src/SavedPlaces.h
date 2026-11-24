// SPDX-License-Identifier: MIT
#pragma once
#include <Message.h>
#include <string>
#include <vector>
namespace Listi {
struct SavedPlace {
  std::string name, path;
};
class SavedPlaces {
public:
  static constexpr size_t Limit = 64;
  const std::vector<SavedPlace> &Items() const { return items; }
  const SavedPlace *Find(const std::string &path) const;
  std::string SuggestedName(const std::string &path) const;
  void Add(const std::string &name, const std::string &path);
  void Rename(const std::string &path, const std::string &name);
  void Remove(const std::string &path);
  void ApplyEdit(const BMessage &request);
  BMessage Message() const;
  static SavedPlaces FromMessage(const BMessage &);
  static SavedPlaces Load(const std::string &file);
  void Save(const std::string &file) const;

private:
  std::vector<SavedPlace> items;
  std::string Name(const std::string &, const std::string &except = "") const;
  bool NameInUse(const std::string &, const std::string &except = "") const;
};
} // namespace Listi
