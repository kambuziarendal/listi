// SPDX-License-Identifier: MIT
#include "SavedNavigationController.h"
#include "Common.h"
#include <stdexcept>
#include <utility>

namespace Listi {
namespace {
constexpr char kPlacesUnavailable[] =
    "Saved places are unavailable or still being saved.";
}

SavedNavigationController::SavedNavigationController(
    BMessenger service, const SavedPlaces &places, bool placesWritable,
    const SavedSearches &searches, bool searchesWritable, BMessenger owner)
    : service(service), owner(owner), places(places), placesWritable(placesWritable),
      searches(searches), searchesWritable(searchesWritable) {}

std::optional<SavedNavigationController::PlacePrompt>
SavedNavigationController::PreparePlaceEdit(const BMessage &message) const {
  if (!CanEditPlaces())
    throw std::runtime_error(kPlacesUnavailable);

  const char *path = message.GetString("path", "");
  const int32 action = message.GetInt32("action", -1);
  const auto *place = places.Find(path);
  if (action < 0 || action > 2 || (action != 0 && !place))
    return std::nullopt;

  static constexpr struct {
    const char *question;
    const char *button;
  } prompts[] = {{"Save this work location: ", "Save place"},
                 {"Rename saved place: ", "Rename place"},
                 {"Forget saved place: ", "Forget place"}};
  const auto &label = prompts[action];
  PlacePrompt prompt{std::string(label.question) + path, label.button, message,
                     std::nullopt};
  if (action != 0)
    prompt.accepted.AddString("promptName", place->name.c_str());
  prompt.accepted.what = kPlaceAccepted;
  prompt.accepted.AddBool("placeLabel", true);
  if (action != 2)
    prompt.initial = place ? place->name : places.SuggestedName(path);
  return prompt;
}

void SavedNavigationController::EditPlace(const BMessage &edit) {
  if (!CanEditPlaces())
    throw std::runtime_error(kPlacesUnavailable);

  auto next = places;
  const char *path = edit.GetString("path", "");
  const char *name = edit.GetString("name", "");
  if (edit.HasString("promptName")) {
    const auto *current = places.Find(path);
    if (!current || current->name != edit.GetString("promptName", ""))
      throw std::runtime_error("This saved place changed while the prompt was open.");
  }
  switch (edit.GetInt32("action", -1)) {
  case 0:
    next.Add(name, path);
    break;
  case 1:
    next.Rename(path, name);
    break;
  case 2:
    next.Remove(path);
    break;
  }
  auto save = next.Message();
  save.what = kSavePlaces;
  save.AddMessenger("origin", owner);
  save.AddMessage("edit", &edit);
  if (auto existing = places.Find(path))
    save.AddString("previousName", existing->name.c_str());
  save.AddInt64("serial", ++placesSerial);
  pendingPlaces = std::move(next);
  if (service.SendMessage(&save) != B_OK) {
    pendingPlaces.reset();
    throw std::runtime_error("Could not queue saved places.");
  }
}

bool SavedNavigationController::FinishPlaceSave(const BMessage &reply) {
  BMessenger origin;
  BMessage snapshot;
  bool refreshed = reply.FindMessage("snapshot", &snapshot) == B_OK;
  if (reply.FindMessenger("origin", &origin) == B_OK && origin != owner) {
    if (refreshed)
      places = SavedPlaces::FromMessage(snapshot);
    return refreshed;
  }
  if (!pendingPlaces || reply.GetInt64("serial", -1) != placesSerial)
    return false;
  if (!reply.HasString("error"))
    places = refreshed ? SavedPlaces::FromMessage(snapshot) : std::move(*pendingPlaces);
  pendingPlaces.reset();
  return true;
}

BMessage SavedNavigationController::SearchStore() const {
  auto message = searches.Message();
  message.what = kSearchStore;
  message.AddInt64("revision", searchesRevision);
  message.AddBool("writable", searchesWritable);
  message.AddBool("saving", pendingSearch.has_value());
  return message;
}

std::optional<SavedNavigationController::SearchUpdate>
SavedNavigationController::EditSearch(const BMessage &edit, BMessenger author) {
  try {
    if (!searchesWritable)
      throw std::runtime_error(
          "Saved searches could not be loaded. Their file is preserved; changes are disabled.");
    const int64 revision = edit.GetInt64("revision", -1);
    const bool modifies = edit.GetBool("replace", false) || edit.GetBool("remove", false);
    if (pendingSearch || revision < 0 || revision > searchesRevision)
      throw std::runtime_error(
          "The saved-search list changed or is being written. Please try again.");

    // A different search can commit between the editor posting and our
    // receiving its message. Validate the affected entry, not the whole list.
    if (modifies && revision != searchesRevision) {
      BMessage expected, actual;
      const auto *current = searches.Find(edit.GetString("name", ""));
      if (current)
        current->WriteTo(actual);
      if (!current || edit.FindMessage("expectedSearch", &expected) != B_OK ||
          !actual.HasSameData(expected, true, true))
        throw std::runtime_error("This saved search changed in another window.");
    }
    auto next = searches;
    auto item = SavedSearch::FromMessage(edit);
    std::string chosen;
    if (edit.GetBool("remove", false))
      next.Remove(item.name);
    else
      chosen = next.Put(std::move(item), edit.GetBool("replace", false));

    auto request = next.Message();
    request.what = kSaveSearches;
    request.AddMessenger("origin", owner);
    request.AddMessage("edit", &edit);
    auto key = edit.GetBool("remove", false) ? edit.GetString("name", "") : chosen;
    if (auto existing = searches.Find(key)) {
      BMessage previous;
      existing->WriteTo(previous);
      request.AddMessage("previous", &previous);
    }
    request.AddInt64("revision", searchesRevision);
    request.AddInt64("serial", ++searchesSerial);
    pendingSearch = PendingSearch{searchesRevision, std::move(next), std::move(chosen), author};
    if (service.SendMessage(&request) != B_OK) {
      pendingSearch.reset();
      throw std::runtime_error("Could not queue saved search.");
    }
    return std::nullopt;
  } catch (const std::exception &e) {
    auto reply = SearchStore();
    reply.AddString("error", e.what());
    return SearchUpdate{reply, author};
  }
}

std::optional<SavedNavigationController::SearchUpdate>
SavedNavigationController::FinishSearchSave(const BMessage &result) {
  BMessenger origin;
  BMessage snapshot;
  bool refreshed = result.FindMessage("snapshot", &snapshot) == B_OK;
  if (result.FindMessenger("origin", &origin) == B_OK && origin != owner) {
    if (!refreshed)
      return std::nullopt;
    searches = SavedSearches::FromMessage(snapshot);
    ++searchesRevision;
    return SearchUpdate{SearchStore(), {}};
  }
  if (!pendingSearch || result.GetInt64("revision", -1) != pendingSearch->revision ||
      result.GetInt64("serial", -1) != searchesSerial)
    return std::nullopt;

  auto saved = std::move(*pendingSearch);
  pendingSearch.reset();
  if (!result.HasString("error")) {
    searches = refreshed ? SavedSearches::FromMessage(snapshot) : std::move(saved.next);
    ++searchesRevision;
  }
  auto reply = SearchStore();
  if (result.HasString("error"))
    reply.AddString("error", result.GetString("error", "Save failed."));
  else {
    reply.AddBool("committed", true);
    reply.AddString("chosen", saved.chosen.c_str());
  }
  return SearchUpdate{reply, saved.author};
}

void SavedNavigationController::HandleSearchMessage(
    const BMessage &message, BMessenger standalone,
    const std::array<BMessenger, 2> &embedded) {
  std::optional<SearchUpdate> reply;
  switch (message.what) {
  case kSearchEdit: {
    auto author = standalone;
    message.FindMessenger("searchEditor", &author);
    reply = EditSearch(message, author);
    break;
  }
  case kSearchesSaved:
    reply = FinishSearchSave(message);
    break;
  }
  if (!reply)
    return;

  auto shared = SearchStore();
  auto notify = [&](const BMessenger &editor) {
    if (editor.IsValid())
      editor.SendMessage(editor == reply->author ? &reply->message : &shared,
                         static_cast<BHandler *>(nullptr), 0);
  };
  notify(standalone);
  for (const auto &editor : embedded)
    notify(editor);
}

} // namespace Listi
