// SPDX-License-Identifier: MIT
#include "AutomaticHistory.h"

#include <Message.h>
#include <Roster.h>
#include <View.h>

class _EXPORT ListiHistoryTile : public BView {
public:
  explicit ListiHistoryTile(float size)
      : BView(BRect(0, 0, size, size), Listi::kHistoryDeskbarItem,
              B_FOLLOW_NONE, B_WILL_DRAW) {}
  explicit ListiHistoryTile(BMessage *archive) : BView(archive) {}

  static ListiHistoryTile *Instantiate(BMessage *archive);

  status_t Archive(BMessage *archive, bool deep) const override {
    auto status = BView::Archive(archive, deep);
    if (status == B_OK)
      status = archive->AddString("add_on", Listi::kHistoryHelperSignature);
    if (status == B_OK)
      status = archive->AddString("class", "ListiHistoryTile");
    return status;
  }

  void AttachedToWindow() override {
    SetViewColor(B_TRANSPARENT_COLOR);
    AdoptParentColors();
    SetToolTip("Listi Folder History: click for status and settings");
    if (!be_roster->IsRunning(Listi::kHistoryHelperSignature))
      Listi::LaunchHistoryHelper(false);
  }

  void Draw(BRect) override {
    SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
    BRect face = Bounds().InsetByCopy(2, 2);
    StrokeEllipse(face);
    const BPoint center((face.left + face.right) / 2, (face.top + face.bottom) / 2);
    StrokeLine(center, BPoint(center.x, face.top + 2));
    StrokeLine(center, BPoint(face.right - 2, center.y));
  }

  void MouseDown(BPoint) override { Listi::LaunchHistoryHelper(true); }
};

ListiHistoryTile *ListiHistoryTile::Instantiate(BMessage *archive) {
  if (!validate_instantiation(archive, "ListiHistoryTile"))
    return nullptr;
  return new ListiHistoryTile(archive);
}

extern "C" _EXPORT BView *instantiate_deskbar_item(float width, float height) {
  return new ListiHistoryTile((width < height ? width : height) - 1);
}
