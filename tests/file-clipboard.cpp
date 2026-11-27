// SPDX-License-Identifier: MIT
#include "FileClipboard.h"
#include "Common.h"
#include <Application.h>
#include <Node.h>
#include <Path.h>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
using namespace Listi;
int checks = 0;
void Check(bool ok, const char *label) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", label); fflush(stdout);
  if (!ok) throw std::runtime_error(label);
  ++checks;
}
void Write(const std::string &p, const std::string &text) { std::ofstream(p) << text; }
std::string Read(const std::string &p) { std::ifstream in(p); return {std::istreambuf_iterator<char>(in), {}}; }
bool Exists(const std::string &p) { struct stat st; return lstat(p.c_str(), &st) == 0; }
BMessage Data(BClipboard &c) { c.Lock(); BMessage m(*c.Data()); c.Unlock(); return m; }
void Set(BClipboard &c, const BMessage &m) { c.Lock(); *c.Data() = m; c.Commit(); c.Unlock(); }
template<class F> bool Rejected(F f) { try { f(); return false; } catch (const std::exception &) { return true; } }
std::vector<std::string> Moved(const Result &r) { std::vector<std::string> out; for (auto &p : r.completedRoots) out.push_back(p.first); return out; }
int main(int argc, char **argv) {
  BApplication app("application/x-vnd.Kambuzi-ListiClipboardTest");
  if (argc > 1) {
    // Probe the actual system clipboard for UI interoperability checks.
    try {
      if (std::string(argv[1]) == "dump") {
        for (auto &item : ReadFileClipboard(Data(*be_clipboard)))
          printf("%s %s\n", item.cut ? "CUT" : "COPY", item.path.c_str());
      } else if (std::string(argv[1]) == "mixed" && argc == 4) {
        // The first Tracker reference is Cut; the second is Copy.
        auto selection = MakeFileSelection({argv[2], argv[3]}, true);
        for (const auto &item : ReadFileClipboard(selection)) {
          if (item.path == argv[3]) {
            auto key = item.key;
            key[0] = 'm';
            Check(selection.ReplaceInt32(key.c_str(), 'Tcsl') == B_OK, "set Tracker Copy mode");
          }
        }
        Set(*be_clipboard, selection);
      } else if ((std::string(argv[1]) == "copy" || std::string(argv[1]) == "cut") && argc > 2) {
        std::vector<std::string> paths; for (int i=2;i<argc;++i) paths.emplace_back(argv[i]);
        PutFileClipboard(paths, std::string(argv[1]) == "cut");
      }
      return 0;
    } catch (const std::exception &e) { fprintf(stderr, "%s\n", e.what()); return 1; }
  }
  char tmp[] = "/boot/home/Listi-clipboard-test-XXXXXX";
  if (!mkdtemp(tmp)) return 2;
  std::string root(tmp), a=root+"/a", b=root+"/b", c=root+"/c";
  mkdir(a.c_str(),0755);mkdir(b.c_str(),0755);mkdir(c.c_str(),0755);
  BClipboard clip("listi-clipboard-tests", true);
  try {
    Write(a+"/one", "original"); Write(a+"/two", "second");
    PutFileClipboard({a+"/one",a+"/two"},false,&clip);
    auto data=Data(clip); auto items=ReadFileClipboard(data);
    Check(items.size()==2 && !items[0].cut,"copy publishes native references and modes");
    const std::string archivePath = "archive:///boot/home/two%20words.zip!/three";
    PutPathsAsText({a + "/one", archivePath}, &clip);
    Check(!HasClipboardFiles(Data(clip)) &&
              ReadClipboardText(&clip) == a + "/one\n" + archivePath,
          "Copy paths replaces file references with newline-separated plain text");
    Check(Rejected([&] { PutPathsAsText({}, &clip); }) &&
              ReadClipboardText(&clip) == a + "/one\n" + archivePath,
          "Copy paths without a selection preserves clipboard contents");
    PutFileClipboard({a+"/one",a+"/two"},false,&clip);
    const std::vector<std::string> pictures{a + "/blå logo.png", b + "/blå logo.png"};
    CopySelectionText(kCopyNames, pictures, root, &clip);
    Check(ReadClipboardText(&clip) == "blå logo.png\nblå logo.png", "Names preserve Unicode and extensions");
    CopySelectionText(kCopyRelative, pictures, root, &clip);
    Check(ReadClipboardText(&clip) == "a/blå logo.png\nb/blå logo.png", "Relative paths distinguish duplicate names");
    Check(Rejected([&] { CopySelectionText(kCopyRelative, pictures, a, &clip); }),
          "Relative paths require a shared base");
    PutFileClipboard({a+"/one",a+"/two"},false,&clip);
    auto paste=PrepareFilePaste(b,&clip);
    auto r=Run(paste.jobs[0],{}); FinishFilePaste(paste,{},&clip);
    Check(r.completed==2 && Read(b+"/one")=="original" && Read(a+"/one")=="original","copy preserves both originals");
    Check(ReadFileClipboard(Data(clip)).size()==2,"copy clipboard remains reusable");
    Check(Run(PrepareFilePaste(c,&clip).jobs[0],{}).completed==2,"copy can paste to another folder");
    Check(Rejected([&]{PrepareFilePaste(a,&clip);}),"same-folder paste rejected before mutation");
    mkdir((a+"/child").c_str(), 0755);
    symlink((a+"/child").c_str(), (root+"/child-alias").c_str());
    PutFileClipboard({a},true,&clip);
    Check(Rejected([&]{PrepareFilePaste(root+"/child-alias",&clip);}),
          "Cut rejects a descendant through an alias before reserving the clipboard");
    paste=PrepareFilePaste(b,&clip);
    Check(paste.jobs.size()==1 && !paste.lease.empty(),
          "rejected target leaves the same Cut selection available for a valid destination");
    FinishFilePaste(paste,{},&clip);
    PutFileClipboard({a+"/one"},true,&clip);
    Check(Exists(a+"/one") && ReadFileClipboard(Data(clip))[0].cut,"cut only marks source");
    paste=PrepareFilePaste(b,&clip);
    Check(Rejected([&]{PrepareFilePaste(c,&clip);}),"reserved cut cannot be pasted twice");
    Hooks skip;skip.conflict=[](const auto &){return Decision::Skip;};
    r=Run(paste.jobs[0],skip);FinishFilePaste(paste,Moved(r),&clip);
    Check(r.skipped==1 && Exists(a+"/one") && ReadFileClipboard(Data(clip))[0].cut,"skipped cut retains original and cut clipboard");
    paste=PrepareFilePaste(b,&clip); Hooks abort;abort.conflict=[](const auto &){return Decision::Abort;};
    r=Run(paste.jobs[0],abort);FinishFilePaste(paste,Moved(r),&clip);
    Check(r.cancelled && Exists(a+"/one") && ReadFileClipboard(Data(clip))[0].cut,"cancelled conflict retains cut clipboard");
    paste=PrepareFilePaste(b,&clip);Hooks replace;replace.conflict=[](const auto &){return Decision::Replace;};
    r=Run(paste.jobs[0],replace);FinishFilePaste(paste,Moved(r),&clip);
    Check(r.completed==1 && !Exists(a+"/one") && Read(b+"/one")=="original","successful cut replaces only approved target");
    Check(Rejected([&]{ReadFileClipboard(Data(clip));}),"successful move consumes cut entry");
    Write(a+"/one","new source");PutFileClipboard({a+"/one"},true,&clip);
    paste=PrepareFilePaste(c,&clip);
    PutFileClipboard({a+"/two"},false,&clip);
    r=Run(paste.jobs[0],replace);FinishFilePaste(paste,Moved(r),&clip);
    Check(!Exists(a+"/one") && ReadFileClipboard(Data(clip))[0].path==a+"/two","new clipboard survives completion of older move");
    Write(a+"/one","stale");PutFileClipboard({a+"/one"},true,&clip);
    rename((a+"/one").c_str(),(a+"/old").c_str());Write(a+"/one","replacement");
    Check(Rejected([&]{PrepareFilePaste(b,&clip);}) && Read(a+"/one")=="replacement","stale clipboard rejects replacement before queueing");
    PutFileClipboard({a+"/one"},true,&clip);paste=PrepareFilePaste(b,&clip);
    rename((a+"/one").c_str(),(a+"/old2").c_str());Write(a+"/one","queued replacement");
    r=Run(paste.jobs[0],replace);FinishFilePaste(paste,Moved(r),&clip);
    Check(r.completed==0 && r.errors.size()==1 && Read(a+"/one")=="queued replacement","queued move rejects replaced source");
    PutFileClipboard({a+"/one"},true,&clip);paste=PrepareFilePaste(b,&clip);
    Hooks change;change.conflict=[&](const auto &){rename((a+"/one").c_str(),(a+"/old3").c_str());Write(a+"/one","conflict replacement");return Decision::Replace;};
    r=Run(paste.jobs[0],change);FinishFilePaste(paste,Moved(r),&clip);
    Check(r.completed==0 && !r.errors.empty() && Read(a+"/one")=="conflict replacement" && Read(b+"/one")=="original","source replacement during conflict leaves both entries untouched");
    PutFileClipboard({a+"/one"},true,&clip);paste=PrepareFilePaste(b,&clip);
    rename(b.c_str(),(root+"/old-b").c_str());mkdir(b.c_str(),0755);
    r=Run(paste.jobs[0],replace);FinishFilePaste(paste,Moved(r),&clip);
    Check(r.completed==0 && Exists(a+"/one") && !Exists(b+"/one"),"replaced destination folder is rejected");
    PutFileClipboard({a+"/one"},true,&clip);paste=PrepareFilePaste(c,&clip);
    change.conflict=[&](const auto &){rename(c.c_str(),(root+"/old-c").c_str());mkdir(c.c_str(),0755);return Decision::Replace;};
    r=Run(paste.jobs[0],change);FinishFilePaste(paste,Moved(r),&clip);
    Check(r.completed==0 && Exists(a+"/one") && !Exists(c+"/one"),"destination replacement during conflict is rejected");
    Write(a+"/shared","first source");Write(c+"/shared","second source");
    PutFileClipboard({a+"/shared",c+"/shared"},true,&clip);
    auto collisions=Data(clip);
    Check(Rejected([&]{PrepareFilePaste(b,&clip);}) &&
              !Data(clip).HasString("listi:paste-lease") && !Exists(b+"/shared") &&
              Read(a+"/shared")=="first source" && Read(c+"/shared")=="second source",
          "same-name cuts from different folders are refused before reservation");
    auto conflictingItems=ReadFileClipboard(collisions);
    auto copyMode=conflictingItems.front().key; copyMode[0]='m';
    collisions.ReplaceInt32(copyMode.c_str(),'Tcsl');Set(clip,collisions);
    Check(Rejected([&]{PrepareFilePaste(b,&clip);}) &&
              !Data(clip).HasString("listi:paste-lease") &&
              ReadFileClipboard(Data(clip)).size()==2,
          "mixed copy and cut cannot queue two jobs for the same output");

    const auto incoming = a + "/set", selected = b + "/set/doc";
    mkdir(incoming.c_str(), 0755);
    mkdir((incoming + "/doc").c_str(), 0755);
    mkdir((b + "/set").c_str(), 0755);
    mkdir(selected.c_str(), 0755);
    Write(incoming + "/doc/content", "incoming");
    Write(selected + "/content", "keep");
    PutFileClipboard({incoming, selected}, true, &clip);
    auto mixed = Data(clip);
    for (const auto &item : ReadFileClipboard(mixed)) {
      if (item.path == incoming) {
        auto key = item.key;
        key[0] = 'm';
        Check(mixed.ReplaceInt32(key.c_str(), 'Tcsl') == B_OK,
              "set incoming folder to Tracker Copy mode");
      }
    }
    Set(clip, mixed);
    paste = PrepareFilePaste(b, &clip);
    Check(paste.jobs.size() == 2 && !paste.lease.empty(), "mixed paste reserved for queued checks");
    const auto overlappingCopy = Run(paste.jobs.front(), replace);
    const auto overlappingMove = Run(paste.jobs.back(), replace);
    FinishFilePaste(paste, Moved(overlappingMove), &clip);
    Check(!overlappingCopy.completed && !overlappingCopy.errors.empty() &&
              !overlappingMove.completed && !overlappingMove.errors.empty() &&
              !Data(clip).HasString("listi:paste-lease") &&
              Read(incoming + "/doc/content") == "incoming" &&
              Read(selected + "/content") == "keep" && !Exists(b + "/doc"),
          "nested output overlapping a Cut root rejects the whole mixed paste");
    // Same two roots, but the incoming merge now touches only a sibling of
    // the selected directory. A shared ancestor alone is not a collision.
    Check(rename((incoming + "/doc").c_str(), (incoming + "/other").c_str()) == 0,
          "prepare a disjoint sibling under the same output tree");
    paste = PrepareFilePaste(b, &clip);
    Check(paste.jobs.size() == 2, "disjoint nested merge is accepted");
    Check(rename((incoming + "/other").c_str(), (incoming + "/doc").c_str()) == 0,
          "introduce overlap after both clipboard jobs were queued");
    const auto stale = Run(paste.jobs.front(), replace);
    FinishFilePaste(paste, {}, &clip);
    Check(!stale.completed && !stale.errors.empty() && !Exists(b + "/doc") &&
              Read(selected + "/content") == "keep" && Read(incoming + "/doc/content") == "incoming",
          "queued Copy rechecks its Cut sibling even after paste preparation");
    Check(rename((incoming + "/doc").c_str(), (incoming + "/other").c_str()) == 0,
          "restore disjoint source before retrying the mixed selection");
    paste = PrepareFilePaste(b, &clip);
    Check(paste.jobs.size() == 2, "rejected mixed selection can retry the disjoint merge");
    auto copyResult = Run(paste.jobs.front(), {});
    auto moveResult = Run(paste.jobs.back(), {});
    FinishFilePaste(paste, Moved(moveResult), &clip);
    Check(copyResult.completed == 1 && moveResult.completed == 1 &&
              !Exists(selected) && Read(incoming + "/other/content") == "incoming" &&
              Read(b + "/set/other/content") == "incoming" && Read(b + "/doc/content") == "keep",
          "nonoverlapping mixed paste delivers both versions and consumes only the cut root");

    // Mixed Tracker clipboard, success and failure must be independent.
    PutFileClipboard({a+"/one",a+"/two"},true,&clip);data=Data(clip);items=ReadFileClipboard(data);
    std::string mode=items[1].key;mode[0]='m';data.ReplaceInt32(mode.c_str(),'Tcsl');Set(clip,data);
    paste=PrepareFilePaste(b,&clip);
    Check(paste.jobs.size()==2 && paste.jobs[0].action==Action::Copy && paste.jobs[1].action==Action::Move,"mixed native clipboard splits copy and move safely");
    Run(paste.jobs[0],{});r=Run(paste.jobs[1],{});FinishFilePaste(paste,Moved(r),&clip);
    Check(!Exists(a+"/one") && Exists(a+"/two") && ReadFileClipboard(Data(clip)).size()==1,"mixed clipboard consumes only moved entry");
    Write(a+"/one","partial");PutFileClipboard({a+"/one",a+"/two"},true,&clip);paste=PrepareFilePaste(c,&clip);
    Write(c+"/two","existing");r=Run(paste.jobs[0],skip);FinishFilePaste(paste,Moved(r),&clip);
    Check(r.completed==1 && r.skipped==1 && !Exists(a+"/one") && Exists(a+"/two") && ReadFileClipboard(Data(clip)).size()==1,"partial move keeps only skipped cut entry");
    PutFileClipboard({a+"/two"},false,&clip);auto saved=Data(clip);
    Check(Rejected([&]{PutFileClipboard({"/boot"},true,&clip);}) && ReadFileClipboard(Data(clip))[0].path==a+"/two","volume root rejection preserves previous clipboard");
    Check(Rejected([&]{PutFileClipboard({"smb://host/share/file"},false,&clip);}),"virtual paths cannot leak into native clipboard");
    Check(Rejected([&]{PrepareFilePaste("sftp://host/folder",&clip);}),"virtual destination rejected without changing clipboard");
    Check(Rejected([&]{PutFileClipboard({},false,&clip);}),"empty selection leaves clipboard untouched");
    Check(Rejected([&]{PutFileClipboard({a+"/two",a+"/two"},false,&clip);}),"duplicate nodes rejected");
    mkdir((a+"/folder").c_str(),0755);Write(a+"/folder/child","child");
    Check(Rejected([&]{PutFileClipboard({a+"/folder",a+"/folder/child"},true,&clip);}),"overlapping source roots rejected");
    PutFileClipboard({a+"/folder"},true,&clip);paste=PrepareFilePaste(b,&clip);r=Run(paste.jobs[0],{});FinishFilePaste(paste,Moved(r),&clip);
    Check(r.completed==1 && Read(b+"/folder/child")=="child" && !Exists(a+"/folder"),"folder cut moves entire root");
    symlink("two",(a+"/link").c_str());PutFileClipboard({a+"/link"},false,&clip);paste=PrepareFilePaste(b,&clip);r=Run(paste.jobs[0],{});
    char target[100]{};auto length=readlink((b+"/link").c_str(),target,99);
    Check(r.completed==1 && length==3 && std::string(target)=="two" && Exists(a+"/two"),"symlink clipboard copies link, not target");
    BNode node((a+"/two").c_str());const char attribute[]="clipboard metadata";node.WriteAttr("Listi:test",B_STRING_TYPE,0,attribute,sizeof(attribute));
    PutFileClipboard({a+"/two"},false,&clip);paste=PrepareFilePaste(c,&clip);r=Run(paste.jobs[0],replace);BNode copied((c+"/two").c_str());char attr[100]{};
    Check(r.completed==1 && copied.ReadAttr("Listi:test",B_STRING_TYPE,0,attr,100)==sizeof(attribute) && !strcmp(attr,attribute),"clipboard copy preserves BFS attributes");
    BMessage text;text.AddData("text/plain",B_MIME_TYPE,"text",4);Set(clip,text);
    Check(Rejected([&]{PrepareFilePaste(b,&clip);}) && Data(clip).HasData("text/plain",B_MIME_TYPE),"text clipboard is not interpreted as file paths");
    data=saved;items=ReadFileClipboard(data);mode=items[0].key;mode[0]='m';data.ReplaceInt32(mode.c_str(),123);Set(clip,data);
    Check(Rejected([&]{PrepareFilePaste(b,&clip);}),"unknown native clipboard mode rejected");
    data=saved;data.RemoveName(mode.c_str());Set(clip,data);
    Check(Rejected([&]{PrepareFilePaste(b,&clip);}),"missing mode rejected");
    data=saved;data.RemoveName(items[0].key.c_str());data.AddRef("refs",&items[0].ref);Set(clip,data);
    Check(Rejected([&]{PrepareFilePaste(b,&clip);}),"unidentified generic refs rejected");
    PutFileClipboard({a+"/two"},true,&clip);paste=PrepareFilePaste(c,&clip);Hooks stop;stop.proceed=[] {return false;};r=Run(paste.jobs[0],stop);FinishFilePaste(paste,Moved(r),&clip);
    Check(r.cancelled && Exists(a+"/two") && ReadFileClipboard(Data(clip))[0].cut,"cancel before transfer preserves source and cut mode");
    printf("%d native clipboard checks passed. Fixture: %s\n",checks,root.c_str());return 0;
  } catch (const std::exception &e) { fprintf(stderr,"FAIL %s (fixture %s)\n",e.what(),root.c_str());return 1; }
}
