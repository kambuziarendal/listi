// SPDX-License-Identifier: MIT
#include "StoredFields.h"
#include "ShelfCollections.h"
#include "MessageStore.h"
#include <StorageDefs.h>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>

namespace Listi {
namespace {
BMessage EncodeStamp(const ShelfNodeStamp &stamp) {
  BMessage message;
  CheckStoredField(message.AddInt64("inode", stamp.inode));
  CheckStoredField(message.AddInt64("seconds", stamp.seconds));
  CheckStoredField(message.AddInt32("nanoseconds", stamp.nanoseconds));
  return message;
}

ShelfNodeStamp DecodeStamp(const BMessage &message) {
  ShelfNodeStamp stamp{message.GetInt64("inode", -1),
                       message.GetInt64("seconds", -1),
                       message.GetInt32("nanoseconds", -1)};
  if (stamp.inode <= 0 || stamp.seconds <= 0 || stamp.nanoseconds < 0 ||
      stamp.nanoseconds >= 1000000000)
    throw std::runtime_error("Invalid saved Shelf identity.");
  return stamp;
}

BMessage EncodeEntry(const ShelfEntry &entry) {
  BMessage message;
  CheckStoredField(message.AddString("path", entry.path.c_str()));
  if (entry.proof) {
    const auto &proof = *entry.proof;
    BMessage identity, root = EncodeStamp(proof.volumeRoot),
                       parent = EncodeStamp(proof.parent),
                       node = EncodeStamp(proof.entry);
    CheckStoredField(identity.AddString("device", proof.device.c_str()));
    CheckStoredField(identity.AddMessage("root", &root));
    CheckStoredField(identity.AddMessage("parent", &parent));
    CheckStoredField(identity.AddMessage("node", &node));
    CheckStoredField(identity.AddUInt32("kind", proof.kind));
    CheckStoredField(message.AddMessage("identity", &identity));
  }
  return message;
}

ShelfEntry DecodeEntry(const BMessage &message) {
  ShelfEntry entry;
  entry.path = message.GetString("path", "");
  entry.restored = entry.unresolved = true;
  if (StoredCount(message, "identity", 1)) {
    const auto identity = StoredItem(message, "identity", 0);
    const std::string device = identity.GetString("device", "");
    const auto kind = identity.GetUInt32("kind", 0);
    if (device.empty() || device.size() >= B_DEV_NAME_LENGTH ||
        (kind != S_IFREG && kind != S_IFDIR && kind != S_IFLNK))
      throw std::runtime_error("Invalid saved Shelf identity.");
    entry.directory = kind == S_IFDIR;
    entry.proof = ShelfProof{device,
                             DecodeStamp(StoredItem(identity, "root", 0)),
                             DecodeStamp(StoredItem(identity, "parent", 0)),
                             DecodeStamp(StoredItem(identity, "node", 0)),
                             kind};
  }
  return entry;
}

bool SameSavedEntry(const ShelfEntry &a, const ShelfEntry &b) {
  if (a.path != b.path || bool(a.proof) != bool(b.proof))
    return false;
  if (!a.proof)
    return true;
  const auto &first = *a.proof, &second = *b.proof;
  return first.device == second.device && first.volumeRoot == second.volumeRoot &&
         first.parent == second.parent && first.entry == second.entry &&
         first.kind == second.kind;
}

void CheckUnchanged(const ShelfCollection *current,
                    const ShelfCollection *expected) {
  if ((!current && !expected) ||
      (current && expected &&
       std::equal(current->entries.begin(), current->entries.end(),
                  expected->entries.begin(), expected->entries.end(),
                  SameSavedEntry)))
    return;
  throw std::runtime_error("The saved collection changed. Open Collections and try again.");
}

// Listi uses B_SINGLE_LAUNCH, but each Shelf window has its own looper.
std::mutex collectionEdits;
}

const ShelfCollection *ShelfCollections::Find(const std::string &name) const {
  auto item = std::find_if(items.begin(), items.end(),
                           [&](const auto &i) { return i.name == name; });
  return item == items.end() ? nullptr : &*item;
}

std::string ShelfCollections::Name(const std::string &value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  const auto last = value.find_last_not_of(" \t\r\n");
  const auto name = first == std::string::npos
                        ? std::string()
                        : value.substr(first, last - first + 1);
  if (name.empty() || name.size() > 80 ||
      std::any_of(name.begin(), name.end(),
                  [](unsigned char c) { return c < 32 || c == 127; }))
    throw std::runtime_error(
        "Use a collection name of 1 to 80 bytes without control characters.");
  return name;
}

void ShelfCollections::Put(const std::string &input, const ShelfSnapshot &shelf,
                           bool replace) {
  const auto name = Name(input);
  if (shelf.entries.empty() || shelf.entries.size() > Shelf::kMaximumEntries)
    throw std::runtime_error("Save between 1 and 1024 Shelf references.");
  std::set<std::string> paths;
  for (const auto &entry : shelf.entries) {
    const auto &path = entry.path;
    if (path.size() < 2 || path[0] != '/' || path.back() == '/' ||
        path.size() >= B_PATH_NAME_LENGTH || path.find('\0') != std::string::npos ||
        !paths.insert(path).second)
      throw std::runtime_error("Invalid or duplicate saved Shelf path.");
  }
  for (auto &item : items) {
    if (item.name != name)
      continue;
    if (!replace)
      throw std::runtime_error("That collection name is already in use.");
    item.entries = shelf.entries;
    return;
  }
  if (replace)
    throw std::runtime_error("That saved collection no longer exists.");
  if (items.size() >= kMaximumCollections)
    throw std::runtime_error("Saved collection limit reached (64).");
  items.push_back({name, shelf.entries});
}

void ShelfCollections::Remove(const std::string &name) {
  auto item = std::find_if(items.begin(), items.end(),
                           [&](const auto &i) { return i.name == name; });
  if (item == items.end())
    throw std::runtime_error("That saved collection no longer exists.");
  items.erase(item);
}

ShelfCollections ShelfCollections::Load(const std::string &file) {
  if (file.empty())
    throw std::runtime_error("The user settings folder is unavailable.");
  const auto root = LoadMessageStore(file);
  ShelfCollections result;
  if (root.what == 'none')
    return result;
  if (root.GetInt32("version", 0) != 1)
    throw std::runtime_error("Unsupported saved Shelf collection format.");
  const auto count = StoredCount(root, "collection", kMaximumCollections);
  for (int32 i = 0; i < count; ++i) {
    const auto collection = StoredItem(root, "collection", i);
    const auto entries = StoredCount(collection, "entry", Shelf::kMaximumEntries);
    ShelfSnapshot shelf;
    for (int32 j = 0; j < entries; ++j)
      shelf.entries.push_back(DecodeEntry(StoredItem(collection, "entry", j)));
    result.Put(collection.GetString("name", ""), shelf, false);
  }
  return result;
}

void ShelfCollections::Save(const std::string &file) const {
  if (file.empty())
    throw std::runtime_error("The user settings folder is unavailable.");
  BMessage root;
  CheckStoredField(root.AddInt32("version", 1));
  for (const auto &item : items) {
    BMessage collection;
    CheckStoredField(collection.AddString("name", item.name.c_str()));
    for (const auto &entry : item.entries) {
      auto encoded = EncodeEntry(entry);
      CheckStoredField(collection.AddMessage("entry", &encoded));
    }
    CheckStoredField(root.AddMessage("collection", &collection));
  }
  SaveMessageStore(file, root);
}

ShelfCollections ShelfCollections::SaveCurrent(const std::string &file,
                                                const std::string &name,
                                                const ShelfSnapshot &shelf,
                                                const ShelfCollection *expected) {
  const auto canonical = Name(name);
  std::lock_guard lock(collectionEdits);
  auto latest = Load(file);
  CheckUnchanged(latest.Find(canonical), expected);
  latest.Put(canonical, shelf, expected != nullptr);
  latest.Save(file);
  return latest;
}

ShelfCollections ShelfCollections::DeleteSaved(const std::string &file,
                                                const ShelfCollection &expected) {
  std::lock_guard lock(collectionEdits);
  auto latest = Load(file);
  CheckUnchanged(latest.Find(expected.name), &expected);
  latest.Remove(expected.name);
  latest.Save(file);
  return latest;
}
}
