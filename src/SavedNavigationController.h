// SPDX-License-Identifier: MIT
#pragma once
#include "SavedPlaces.h"
#include "SavedSearches.h"
#include <Messenger.h>
#include <array>
#include <optional>

namespace Listi {

// Window-looper state for the two navigation stores. Only the service writes to
// disk; readers see the last committed list until its matching reply arrives.
class SavedNavigationController {
public:
  SavedNavigationController(BMessenger service, const SavedPlaces &places,
                            bool placesWritable, const SavedSearches &searches,
                            bool searchesWritable, BMessenger owner = {});

  bool Saving() const { return pendingPlaces || pendingSearch; }
  bool CanEditPlaces() const { return placesWritable && !pendingPlaces; }
  const SavedPlaces &Places() const { return places; }

  struct PlacePrompt {
    std::string question;
    const char *button;
    BMessage accepted;
    std::optional<std::string> initial;
  };
  std::optional<PlacePrompt> PreparePlaceEdit(const BMessage &action) const;
  void EditPlace(const BMessage &edit);
  bool FinishPlaceSave(const BMessage &reply);

  struct SearchUpdate {
    BMessage message;
    BMessenger author;
  };
  BMessage SearchStore() const;
  std::optional<SearchUpdate> EditSearch(const BMessage &edit, BMessenger author);
  std::optional<SearchUpdate> FinishSearchSave(const BMessage &reply);
  void HandleSearchMessage(const BMessage &message, BMessenger standalone,
                           const std::array<BMessenger, 2> &embedded);

private:
  BMessenger service, owner;
  SavedPlaces places;
  bool placesWritable;
  int64 placesSerial = 0;
  std::optional<SavedPlaces> pendingPlaces;

  SavedSearches searches;
  bool searchesWritable;
  int64 searchesRevision = 0;
  // Failed saves keep the revision, but every attempt needs a distinct reply.
  int64 searchesSerial = 0;
  struct PendingSearch {
    int64 revision;
    SavedSearches next;
    std::string chosen;
    BMessenger author;
  };
  std::optional<PendingSearch> pendingSearch;
};

} // namespace Listi
