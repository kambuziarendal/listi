// SPDX-License-Identifier: MIT
#include "StoredFields.h"
#include "SavedPlaces.h"
#include "MessageStore.h"
#include "VirtualFile.h"
#include <algorithm>
#include <stdexcept>
namespace Listi {
namespace {
constexpr size_t kNameLimit = 80;

std::string TrimName(const std::string &name) {
  const auto first = name.find_first_not_of(" \t\r\n");
  return first == std::string::npos
             ? ""
             : name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
}

bool ValidName(const std::string &name) {
  return !name.empty() && name.size() <= kNameLimit &&
         std::none_of(name.begin(), name.end(),
                      [](unsigned char c) { return c < 32 || c == 127; });
}

std::string NamePrefix(const std::string &name, size_t limit) {
  size_t end = std::min(name.size(), limit);
  if (end < name.size())
    while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80)
      --end;
  return TrimName(name.substr(0, end));
}

std::string Address(const std::string &path) {
  if (path.empty() || path.size() > 8192 ||
      path.find('\0') != std::string::npos)
    throw std::runtime_error("Invalid saved address.");
  if (IsVirtual(path))
    return ParseLocation(path).URL(); // Reject embedded authentication.
  if (path[0] != '/' && path != "@disks")
    throw std::runtime_error("Saved places require an absolute address.");
  return path;
}
}
const SavedPlace *SavedPlaces::Find(const std::string &path) const {
  for (auto &item : items)
    if (item.path == path)
      return &item;
  return nullptr;
}
std::string SavedPlaces::Name(const std::string &raw,
                              const std::string &except) const {
  auto name = TrimName(raw);
  if (!ValidName(name))
    throw std::runtime_error(
        "Use a name of 1 to 80 bytes without control characters.");
  if (NameInUse(name, except))
    throw std::runtime_error("That place name is already in use.");
  return name;
}
bool SavedPlaces::NameInUse(const std::string &name,
                            const std::string &except) const {
  for (auto &item : items)
    if (item.path != except && item.name == name)
      return true;
  return false;
}
std::string SavedPlaces::SuggestedName(const std::string &path) const {
  auto leaf = LocationLeaf(path);
  auto base = NamePrefix(leaf.empty() ? path : leaf, kNameLimit);
  if (!ValidName(base))
    base = "Place";
  auto name = base;
  for (size_t i = 2; NameInUse(name); ++i) {
    const auto suffix = " (" + std::to_string(i) + ")";
    name = NamePrefix(base, kNameLimit - suffix.size()) + suffix;
  }
  return name;
}
void SavedPlaces::Add(const std::string &raw, const std::string &location) {
  auto path = Address(location);
  auto name = Name(raw);
  if (Find(path))
    throw std::runtime_error("This place is already saved.");
  if (items.size() >= Limit)
    throw std::runtime_error("Saved-place limit reached (64).");
  items.push_back({name, path});
}
void SavedPlaces::Rename(const std::string &path, const std::string &raw) {
  auto name = Name(raw, path);
  for (auto &item : items)
    if (item.path == path) {
      item.name = name;
      return;
    }
  throw std::runtime_error("That saved place no longer exists.");
}
void SavedPlaces::Remove(const std::string &path) {
  auto found = std::find_if(items.begin(), items.end(),
                            [&](auto &p) { return p.path == path; });
  if (found == items.end())
    throw std::runtime_error("That saved place no longer exists.");
  items.erase(found);
}
void SavedPlaces::ApplyEdit(const BMessage &request) {
  BMessage edit;
  if (request.FindMessage("edit", &edit) != B_OK)
    throw std::runtime_error("Missing saved-place edit.");
  const char *path = edit.GetString("path", "");
  const char *name = edit.GetString("name", "");
  const int32 action = edit.GetInt32("action", -1);
  const auto *old = Find(path);
  if (action != 0 && (!old || old->name != request.GetString("previousName", "")))
    throw std::runtime_error("This saved place changed in another window.");
  switch (action) {
  case 0: Add(name, path); break;
  case 1: Rename(path, name); break;
  case 2: Remove(path); break;
  default: throw std::runtime_error("Unknown saved-place edit.");
  }
}
BMessage SavedPlaces::Message() const {
  BMessage result;
  CheckStoredField(result.AddInt32("version", 1));
  for (auto &item : items) {
    BMessage place;
    CheckStoredField(place.AddString("name", item.name.c_str()));
    CheckStoredField(place.AddString("path", item.path.c_str()));
    CheckStoredField(result.AddMessage("place", &place));
  }
  return result;
}
SavedPlaces SavedPlaces::FromMessage(const BMessage &m) {
  if (m.GetInt32("version", 0) != 1)
    throw std::runtime_error("Unsupported saved-places format.");
  SavedPlaces result;
  BMessage place;
  const auto count = StoredCount(m, "place", Limit);
  for (int32 i = 0; i < count; ++i) {
    if (m.FindMessage("place", i, &place) != B_OK)
      throw std::runtime_error("Invalid saved place.");
    result.Add(place.GetString("name", ""), place.GetString("path", ""));
  }
  return result;
}
SavedPlaces SavedPlaces::Load(const std::string &file) {
  auto message = LoadMessageStore(file);
  if (message.what == 'none')
    return {};
  return FromMessage(message);
}
void SavedPlaces::Save(const std::string &file) const {
  auto message = Message();
  FromMessage(message);
  SaveMessageStore(file, message);
}
} // namespace Listi
