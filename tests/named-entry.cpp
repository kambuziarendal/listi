// SPDX-License-Identifier: MIT
#include "NamedEntry.h"
#include "JobSnapshot.h"
#include <Application.h>
#include <Clipboard.h>
#include <Node.h>
#include <StorageDefs.h>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
using namespace Listi;
namespace {
int checks = 0;
void Check(bool ok, const char *label) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", label);
  if (!ok) throw std::runtime_error(label);
  ++checks;
}
void Write(const std::string &path, const std::string &text) { std::ofstream(path) << text; }
std::string Read(const std::string &path) {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), {}};
}
bool Exists(const std::string &path) { struct stat st; return !lstat(path.c_str(), &st); }
std::string Link(const std::string &path) {
  char data[4096]; ssize_t size = readlink(path.c_str(), data, sizeof(data));
  return size < 0 ? "" : std::string(data, size);
}
template<class F> bool Reject(F action) {
  try { action(); return false; } catch (const std::exception &) { return true; }
}
}
int main() {
  BApplication app("application/x-vnd.Kambuzi-NamedEntryTests");
  char temp[] = "/boot/home/listi-named-tests-XXXXXX";
  if (!mkdtemp(temp)) return 2;
  std::string root(temp), a = root + "/a", b = root + "/b";
  mkdir(a.c_str(),0755); mkdir(b.c_str(),0755);
  auto clipboard = be_clipboard->SystemCount();
  try {
    Job create{Action::MakeFile, {}, b + "/new note"};
    FreezeLocalJob(create, b);
    Check(Run(create, {}).completed == 1 && Exists(create.destination) &&
          Read(create.destination).empty(), "new file is empty and in the named folder");
    Write(create.destination, "existing");
    Check(!Run(create, {}).errors.empty() && Read(create.destination) == "existing",
          "new file never overwrites another entry");
    unlink(create.destination.c_str());
    const auto file = a + "/æ $' report.txt";
    Write(file,"original"); chmod(file.c_str(),0640);
    BNode node(file.c_str()); node.WriteAttr("user:note",B_STRING_TYPE,0,"retained",9);
    auto duplicate = PrepareNamedEntry(Action::Duplicate,{file});
    Check(duplicate.entryName == "æ $' report copy.txt", "duplicate suggestion preserves extension and Unicode");
    Check(duplicate.destination == a && !Exists(Join(a,duplicate.entryName)), "duplicate freezes source parent without writing");
    auto result = Run(duplicate,{});
    auto copy = Join(a,duplicate.entryName);
    Check(result.completed == 1 && Read(copy) == "original" && Read(file) == "original", "named duplicate preserves original and payload");
    struct stat st; lstat(copy.c_str(),&st);
    char note[32] = {}; BNode copied(copy.c_str()); copied.ReadAttr("user:note",B_STRING_TYPE,0,note,sizeof(note));
    Check((st.st_mode & 0777) == 0640 && std::string(note) == "retained", "duplicate preserves permissions and BFS attributes");
    Check(PrepareNamedEntry(Action::Duplicate,{file}).entryName == "æ $' report copy 2.txt", "next duplicate suggestion avoids existing copy");
    bool asked = false; Hooks replace;
    replace.conflict = [&](const auto &) { asked = true; return Decision::Replace; };
    result = Run(duplicate,replace);
    Check(result.completed == 0 && !result.errors.empty() && !asked && Read(copy) == "original", "duplicate never offers replacement of existing target");
    for (const auto &name : {std::string(""),std::string("."),std::string(".."),std::string("../escape"),std::string(256,'a'),std::string("x\0y",3)})
      Check(Reject([&] { ValidateNewEntryName(a,name); }), "invalid new entry name rejected");
    Check(Reject([&] { PrepareNamedEntry(Action::Duplicate,{}); }), "empty selection refused");
    Check(Reject([&] { PrepareNamedEntry(Action::Link,{file,copy},b); }), "multi-target link refused");
    Check(Reject([&] { PrepareNamedEntry(Action::Duplicate,{"sftp://example.invalid/file"}); }), "remote duplicate refused without connecting");
    Check(Reject([&] { PrepareNamedEntry(Action::Link,{file},"@disks"); }), "volume listing is not a link destination");
    Check(Reject([&] { PrepareNamedEntry(Action::Duplicate,{"/boot"}); }), "volume-root duplicate refused");
    auto relative = PrepareNamedEntry(Action::Link,{file},b);
    relative.entryName = "relative"; relative.relativeLink = true;
    result = Run(relative,{});
    Check(result.completed == 1 && Link(b+"/relative") == "../a/æ $' report.txt" && Read(b+"/relative") == "original", "relative symbolic link resolves target with shell-like filename");
    auto absolute = PrepareNamedEntry(Action::Link,{file},b);
    absolute.entryName = "absolute"; absolute.relativeLink = false;
    result = Run(absolute,{});
    Check(result.completed == 1 && Link(b+"/absolute") == file && Read(b+"/absolute") == "original", "absolute symbolic link stores full original path");
    result = Run(absolute,replace);
    Check(result.completed == 0 && !result.errors.empty() && Link(b+"/absolute") == file, "existing link is never replaced");
    rename(root.c_str(),(root+"-moved").c_str());
    Check(Read(root+"-moved/b/relative") == "original" && Read(root+"-moved/b/absolute").empty(), "relative link survives moving the shared tree; absolute remains absolute");
    rename((root+"-moved").c_str(),root.c_str());
    mkdir((a+"/folder.ext").c_str(),0755); mkdir((a+"/folder.ext/nested").c_str(),0755);
    Write(a+"/folder.ext/nested/data","nested"); symlink("missing",(a+"/folder.ext/broken").c_str());
    auto folder = PrepareNamedEntry(Action::Duplicate,{a+"/folder.ext"});
    Check(folder.entryName == "folder.ext copy", "folder dots are not treated as filename extensions");
    result = Run(folder,{});
    Check(result.completed == 1 && Read(a+"/folder.ext copy/nested/data") == "nested" && Link(a+"/folder.ext copy/broken") == "missing", "directory duplicate copies hierarchy and does not follow broken links");
    auto dirLink = PrepareNamedEntry(Action::Link,{a+"/folder.ext"},b);
    result = Run(dirLink,{});
    Check(result.completed == 1 && Read(Join(b,dirLink.entryName)+"/nested/data") == "nested", "directory link is usable without copying its contents");
    auto broken = PrepareNamedEntry(Action::Duplicate,{a+"/folder.ext/broken"});
    result = Run(broken,{});
    Check(result.completed == 1 && Link(Join(broken.destination,broken.entryName)) == "missing", "duplicating a symlink preserves the link itself");
    Check(Reject([&] { ValidateNewEntryName(a+"/folder.ext","broken"); }), "broken existing symlink counts as a name collision");
    auto stale = PrepareNamedEntry(Action::Duplicate,{file});
    rename(file.c_str(),(a+"/old-original").c_str()); Write(file,"replacement");
    result = Run(stale,{});
    Check(result.completed == 0 && !result.errors.empty() && !Exists(Join(a,stale.entryName)), "source replaced while naming cannot be duplicated");
    auto wrongDestination = PrepareNamedEntry(Action::Duplicate,{file});
    wrongDestination.destinationFolder = wrongDestination.destination;
    wrongDestination.destination = b;
    result = Run(wrongDestination,{});
    Check(result.completed == 0 && !result.errors.empty() &&
              !Exists(Join(b,wrongDestination.entryName)),
          "an unrelated guarded folder cannot authorize a duplicate destination");
    stale = PrepareNamedEntry(Action::Link,{file},b);
    rename(b.c_str(),(root+"/old-b").c_str()); mkdir(b.c_str(),0755);
    result = Run(stale,{});
    Check(result.completed == 0 && !result.errors.empty() && !Exists(Join(b,stale.entryName)), "replaced destination cannot receive a queued link");
    auto cancelled = PrepareNamedEntry(Action::Link,{file},b);
    Hooks stop; stop.proceed = [] { return false; };
    result = Run(cancelled,stop);
    Check(result.cancelled && !Exists(Join(b,cancelled.entryName)), "cancelled link creates no entry");
    auto raced = PrepareNamedEntry(Action::Duplicate,{file});
    Hooks race; race.progress = [&](const auto &, uint64_t) { Write(Join(a,raced.entryName),"competitor"); };
    result = Run(raced,race);
    Check(result.completed == 0 && !result.errors.empty() && Read(Join(a,raced.entryName)) == "competitor" && Read(file) == "replacement", "destination created during duplicate is preserved");
    auto longFile = a + "/" + std::string(250,'a') + ".txt";
    Write(longFile,"long"); auto longCopy = PrepareNamedEntry(Action::Duplicate,{longFile});
    Check(longCopy.entryName.size() < B_FILE_NAME_LENGTH && Run(longCopy,{}).completed == 1, "long original name gets a valid bounded suggestion");
    auto hidden = a+"/.settings"; Write(hidden,"hidden");
    Check(PrepareNamedEntry(Action::Duplicate,{hidden}).entryName == ".settings copy", "dotfile suggestion retains the full hidden name");
    auto interrupted = PrepareNamedEntry(Action::Duplicate,{a+"/folder.ext"});
    interrupted.entryName = "interrupted folder";
    bool proceed = true; Hooks partial;
    partial.proceed = [&] { return proceed; };
    partial.progress = [&](const auto &,uint64_t) { proceed = false; };
    result = Run(interrupted,partial);
    Check(result.cancelled && Read(a+"/folder.ext/nested/data") == "nested", "cancelled folder duplicate retains originals and reports cancellation");
    Check(be_clipboard->SystemCount() == clipboard, "named entry actions leave system clipboard untouched");
    printf("%d named entry checks passed\n",checks);
    return 0;
  } catch (const std::exception &e) { fprintf(stderr,"%s\n",e.what()); return 1; }
}
