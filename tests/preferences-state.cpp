// SPDX-License-Identifier: MIT
#include "Common.h"
#include <Application.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
int main() {
  BApplication app("application/x-vnd.Kambuzi-Listi-PreferencesTest");
  int checks=0, failures=0;
  auto check=[&](bool ok,const char *label) {
    ++checks;failures+=!ok;
    std::cout<<(ok?"PASS ":"FAIL ")<<label<<std::endl;
  };
  Preferences live;
  Preferences other;
  other.Load(BMessage());
  check(other.separateWindows, "older archives default to separate windows");
  Preferences edited = other;
  edited.separateWindows = false;
  BMessage delta = edited.SharedChanges(&other);
  other.workspace = Workspace::einn;
  other.icons = false;
  other.ApplyShared(delta);
  check(!other.separateWindows && !other.icons && other.IsEinn(),
        "shared edit preserves unrelated appearance and window layout");
  Preferences reopened;
  reopened.Load(other.Save());
  check(!reopened.separateWindows, "separate windows preference survives restart");
  auto stale=live.Save();
  stale.ReplaceBool("icons",false);stale.ReplaceFloat("fontSize",17);
  live.SetPaneMode(0,3);live.SetPaneMode(1,2);
  live.paneTree[0]=true;live.paneTree[1]=true;
  live.workspace=Workspace::einn;live.singlePane=1;
  live.preview=true;live.folderModes["/synthetic"]=1;
  live.ApplyDialog(stale);
  check(live.IsEinn() && live.singlePane==1,"stale preferences dialog preserves active workspace");
  Preferences restored;restored.Load(live.Save());
  check(restored.IsEinn() && restored.singlePane==1,"workspace and chosen panel survive restart");
  check(restored.buttons && restored.paneTree[1] && restored.preview,"hidden Tviskipt options remain stored");
  Preferences buttons;
  check(buttons.ActionButtonsVisible(), "older preferences inherit visible function buttons");
  buttons.tviskiptButtons = 0;
  Preferences resumed;
  resumed.Load(buttons.Save());
  check(!resumed.ActionButtonsVisible() && resumed.buttons,
        "Tviskipt-only hide survives restart without changing the layout default");
  resumed.workspace = Workspace::smidja;
  check(resumed.ActionButtonsVisible(), "Smidja still uses the layout default");
  resumed.smidjaButtons = 0;
  resumed.tviskiptButtons = 1;
  resumed.Load(resumed.Save());
  resumed.ApplyDialog(stale);
  check(!resumed.ActionButtonsVisible(), "Smidja hide survives restart and stale settings");
  resumed.workspace = Workspace::tviskipt;
  check(resumed.ActionButtonsVisible(), "Tviskipt visibility is independent of Smidja");
  resumed.tviskiptButtons = 0;
  resumed.ApplyDialog(stale);
  check(!resumed.ActionButtonsVisible(), "stale appearance dialog preserves the quick toggle");
  resumed.ApplyLayout(WorkspaceLayout{});
  check(!resumed.ActionButtonsVisible(), "loading a profile does not adopt another quick toggle");
  resumed.buttons = false;
  resumed.tviskiptButtons = 1;
  check(resumed.ActionButtonsVisible(), "quick toggle can show the bank against a hidden default");
  auto invalidBank = resumed.Save();
  invalidBank.ReplaceInt32("tviskiptButtons", 99);
  resumed.Load(invalidBank);
  check(!resumed.ActionButtonsVisible() && resumed.tviskiptButtons == -1,
        "invalid quick toggle falls back to the layout default");
  BMessage legacy;restored.Load(legacy);
  check(!restored.IsEinn() && restored.singlePane==0,"older settings default to Tviskipt");
  legacy.AddBool("simpleWorkspace",true);legacy.AddInt32("singlePane",42);restored.Load(legacy);
  check(restored.IsEinn() && restored.singlePane==0,"invalid stored pane cannot select outside workspace");
  live.workspace=Workspace::askr;
  live.ApplyDialog(stale);
  check(live.workspace==Workspace::askr,"stale dialog preserves Askr");
  restored.Load(live.Save());
  check(restored.workspace==Workspace::askr && restored.singlePane==1,"Askr and active pane survive restart");
  check(live.Save().GetBool("simpleWorkspace",false),"Askr fallback is Einn on older versions");
  BMessage invalid;invalid.AddInt32("workspace",99);restored.Load(invalid);
  check(!restored.SinglePane(),"unknown workspace safely defaults to Tviskipt");
  check(!live.icons && live.font.Size()==17,"dialog appearance fields apply");
  restored.Load(live.Save()); restored.workspace=Workspace::tviskipt;
  check(restored.PaneMode(0)==3 && restored.PaneMode(1)==2,"stale dialog preserves current pane modes");
  check(live.paneTree[0] && live.paneTree[1],"stale dialog preserves both trees");
  check(live.preview && live.folderModes["/synthetic"]==1,"stale dialog preserves preview and folder overrides");
  live.workspace=Workspace::smidja;live.smidjaSidebar[0]=SidebarMode::Hidden;
  live.smidjaSidebar[1]=SidebarMode::Favorites;live.smidjaPreview=false;
  live.ApplyDialog(stale);
  check(live.IsSmidja() && !live.SinglePane(),"stale dialog preserves Smidja");
  restored.Load(live.Save());
  check(restored.IsSmidja() && restored.smidjaSidebar[0] == SidebarMode::Hidden &&
        restored.smidjaSidebar[1] == SidebarMode::Favorites &&
        !restored.smidjaPreview,"Smidja customization survives restart");
  check(!live.Save().GetBool("simpleWorkspace",true),"Smidja rolls back safely to Tviskipt");
  BMessage oldSmidja;oldSmidja.AddInt32("workspace",3);restored.Load(oldSmidja);
  check(restored.IsSmidja() && restored.smidjaSidebar[0] == SidebarMode::FolderTree &&
        restored.smidjaSidebar[1] == SidebarMode::FolderTree &&
        restored.smidjaPreview,"Smidja defaults expose both trees and preview");
  // Numeric identities from existing installations must not change.
  for (int32 id = 0; id < 4; ++id) {
    const auto expected = static_cast<Workspace>(id);
    BMessage old; old.AddInt32("workspace", id);
    old.AddBool("workbenchLeftTree", false);
    old.AddBool("workbenchRightTree", true);
    old.AddBool("workbenchPreview", false);
    restored.Load(old);
    check(restored.workspace == expected, "legacy workspace number preserves layout");
    check(restored.smidjaSidebar[0] == SidebarMode::Hidden &&
        restored.smidjaSidebar[1] == SidebarMode::FolderTree && !restored.smidjaPreview,
          "legacy Smidja layout survives upgrade");
    auto saved = restored.Save();
    check(saved.GetInt32("workspace", -1) == id &&
          !strcmp(saved.GetString("workspaceId", ""), WorkspaceId(expected)),
          "new save includes stable numeric alias and canonical ASCII ID");
    check(!saved.GetBool("workbenchLeftTree", true) && saved.GetBool("workbenchRightTree", false) &&
          !saved.GetBool("workbenchPreview", true), "older app can read saved Smidja fields");
    BMessage canonical; canonical.AddString("workspaceId", WorkspaceId(expected));
    canonical.AddBool("smidjaLeftTree", false); canonical.AddBool("smidjaRightTree", true);
    canonical.AddBool("smidjaPreview", false); restored.Load(canonical);
    check(restored.workspace == expected && restored.smidjaSidebar[0] == SidebarMode::Hidden &&
          restored.smidjaSidebar[1] == SidebarMode::FolderTree && !restored.smidjaPreview, "canonical-only settings load");
    // Simulate an older version modifying fields while newer aliases remain.
    saved.ReplaceInt32("workspace", (id + 1) % 4);
    saved.ReplaceBool("workbenchLeftTree", true);
    saved.ReplaceBool("workbenchPreview", true);
    restored.Load(saved);
    check(static_cast<int32>(restored.workspace) == (id + 1) % 4 &&
          restored.smidjaSidebar[0] == SidebarMode::FolderTree && restored.smidjaPreview,
          "rollback edits override stale canonical aliases");
  }
  BMessage unknown; unknown.AddString("workspaceId", "unknown"); restored.Load(unknown);
  check(restored.workspace == Workspace::tviskipt, "unknown ID defaults to Tviskipt");
  check(!strcmp(WorkspaceName(Workspace::smidja), "Smiðja") &&
        !strcmp(WorkspaceName(Workspace::tviskipt), "Tvískipt"), "display names preserve Unicode");
  Preferences views;
  views.workspace=Workspace::einn;
  check(views.PaneMode(0)==0 && views.PaneMode(1)==0,
        "Einn starts both panels in Details");
  views.workspace=Workspace::smidja;
  check(views.PaneMode(0)==0, "other workspaces still start in Details");
  views.SetPaneMode(0, MillerMode);
  views.folderModes["/remembered"]=MillerMode;
  auto dialog=views;
  dialog.SetDefaultMode(Workspace::einn,1);
  views.ApplyDialog(dialog.Save());
  restored.Load(views.Save());
  check(restored.PaneMode(0)==MillerMode && restored.PaneMode(1)==0 &&
        restored.folderModes["/remembered"]==MillerMode,
        "column pane and folder overrides persist independently of defaults");
  restored.workspace=Workspace::einn;
  check(restored.PaneMode(0)==1, "custom Einn default survives restart");
  restored.SetPaneMode(0,2);
  dialog=restored;
  dialog.SetDefaultMode(Workspace::einn,3);
  restored.ApplyDialog(dialog.Save());
  check(!restored.HasPaneMode(0) && restored.PaneMode(0)==3 &&
        restored.folderModes["/remembered"]==MillerMode,
        "changing a default updates its panes without removing folder choices");
  BMessage oldView;
  oldView.AddInt32("leftMode", 2);
  restored.Load(oldView);
  restored.workspace=Workspace::einn;
  check(restored.PaneMode(0)==2 && restored.PaneMode(1)==0,
        "upgrade preserves a legacy explicit icon choice");
  for(float size:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-1.f,7.f,41.f}) {
    BMessage m;m.AddFloat("fontSize",size);live.Load(m);
    check(live.font.Size()==12,"invalid persisted font safely defaults to 12");
  }
  for(float size:{8.f,12.f,40.f}) {
    BMessage m;m.AddFloat("fontSize",size);live.Load(m);
    check(live.font.Size()==size,"valid font boundaries preserved");
  }
  std::cout<<checks<<" checks, "<<failures<<" failures\n";
  return failures?1:0;
}
