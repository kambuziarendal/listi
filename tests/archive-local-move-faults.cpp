// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "VirtualFile.h"
#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <Path.h>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using namespace Listi;
namespace {
enum Fault { None, DestinationRename, SourceRename, CorruptDestination, LocalDelete, ClaimDestination } fault=None;
std::string sourceArchive,destinationArchive,deletePath;
bool injected=false;
struct stat claimParent{};
const std::string payload="Listi move fault payload";
std::string readBytes(const std::string &path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};}
void clean(const std::string &p){BDirectory d(p.c_str());BEntry e;while(d.GetNextEntry(&e,false)==B_OK){struct stat s;e.GetStat(&s);if(S_ISDIR(s.st_mode)){BPath path;e.GetPath(&path);clean(path.Path());}e.Remove();}rmdir(p.c_str());}
}
extern "C" int __real_openat(int, const char *, int, ...);
extern "C" int __wrap_openat(int parent, const char *leaf, int flags, ...) {
  if (!(flags & O_CREAT))
    return __real_openat(parent, leaf, flags);
  va_list args;
  va_start(args, flags);
  int mode = va_arg(args, int);
  va_end(args);
  struct stat actualParent;
  if (fault == ClaimDestination && !injected && !strcmp(leaf, "item") &&
      (flags & O_EXCL) && !fstat(parent, &actualParent) &&
      actualParent.st_dev == claimParent.st_dev && actualParent.st_ino == claimParent.st_ino) {
    const int other = __real_openat(parent, leaf, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (other >= 0) {
      injected = write(other, "other", 5) == 5;
      close(other);
    }
  }
  return __real_openat(parent, leaf, flags, mode);
}
extern "C" int __real_renameat(int,const char *,int,const char *);
extern "C" int __wrap_renameat(int from,const char *oldLeaf,int target,const char *newLeaf){
 const bool replacement=std::string(oldLeaf)=="updated";
 const std::string to=replacement && Leaf(destinationArchive)==newLeaf?destinationArchive:
     replacement && Leaf(sourceArchive)==newLeaf?sourceArchive:"";
 if (replacement && !injected && ((fault==DestinationRename&&to==destinationArchive)||(fault==SourceRename&&to==sourceArchive))) {
  injected=true;errno=fault==DestinationRename?ENOSPC:EIO;return -1;
 }
 int status=__real_renameat(from,oldLeaf,target,newLeaf);
 if(replacement&&!status&&!injected&&fault==CorruptDestination&&to==destinationArchive){
  auto bytes=readBytes(to);auto at=bytes.find(payload);
  if(at!=std::string::npos){int fd=open(to.c_str(),O_WRONLY);if(fd>=0){injected=pwrite(fd,"X",1,at)==1;fsync(fd);close(fd);}}
 }
 return status;
}
extern "C" int __real_unlink(const char *);
extern "C" int __wrap_unlink(const char *path){
 if(fault==LocalDelete&&!injected&&path==deletePath){injected=true;errno=EIO;return -1;}
 return __real_unlink(path);
}
int main(){
 BApplication app("application/x-vnd.Listi-archive-local-fault-test");char pattern[]="/tmp/Listi-local-fault-XXXXXX";auto made=mkdtemp(pattern);if(!made)return 2;std::string root=made;
 mkdir((root+"/in").c_str(),0700);std::ofstream(root+"/in/item")<<payload;std::ofstream(root+"/in/keep")<<"keep";
 int checks=0,failures=0;auto check=[&](bool ok,const std::string&label){++checks;failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<label<<std::endl;};
 for(auto mode:{DestinationRename,CorruptDestination,LocalDelete}){
  fault=None;injected=false;std::ofstream(root+"/in/item")<<payload;
  auto d=PrepareArchiveJob(false,{root+"/in/keep"});d.archiveFormat="tar";SetArchiveDestination(d,root,"dest"+std::to_string(mode));if(Run(d,{}).completed!=1)return 2;
  destinationArchive=d.destination;auto target=OpenArchive(destinationArchive);auto before=readBytes(destinationArchive);
  char *resolved=realpath((root+"/in/item").c_str(),nullptr);deletePath=resolved;free(resolved);
  auto job=PrepareArchiveMove(Job{Action::Move,{root+"/in/item"},target});fault=mode;auto r=Run(job,{});fault=None;
  check(injected&&!r.errors.empty()&&!r.completed&&r.completedRoots.empty(),"local move injected fault "+std::to_string(mode)+" cannot report success");
  check(readBytes(root+"/in/item")==payload,"local move injected fault "+std::to_string(mode)+" retains full source bytes");
  if(mode==DestinationRename)check(readBytes(destinationArchive)==before,"archive ENOSPC preserves previous target");
  if (mode == CorruptDestination) {
    bool detected = false;
    for (const auto &error : r.errors) {
      // Publication can now reject changed output before the move's byte comparison.
      if (error.find("verified contents could not be confirmed") != std::string::npos ||
          error.find("Destination bytes differ") != std::string::npos)
        detected = true;
    }
    check(detected && readBytes(destinationArchive).find("X" + payload.substr(1)) != std::string::npos,
          "damaged published destination detected before source cleanup");
  }
  if(mode==LocalDelete)check(readBytes(destinationArchive)!=before,"local unlink failure leaves verified destination copy");
 }
 fault=None;injected=false;
 auto s=PrepareArchiveJob(false,{root+"/in/item",root+"/in/keep"});s.archiveFormat="zip";SetArchiveDestination(s,root,"source");if(Run(s,{}).completed!=1)return 2;
 sourceArchive=s.destination;auto source=OpenArchive(sourceArchive),member=ChildLocation(source,"item");auto original=readBytes(sourceArchive);
 auto out=root+"/out";mkdir(out.c_str(),0700);auto job=PrepareArchiveMove(Job{Action::Move,{member},out});fault=SourceRename;auto r=Run(job,{});fault=None;
 check(injected&&!r.completed&&!r.errors.empty(),"archive cleanup rename failure cannot report move");
 check(readBytes(sourceArchive)==original&&readBytes(out+"/item")==payload,"archive cleanup failure retains exact source and local copy");
 unlink((out+"/item").c_str());
 injected = false;
 if (stat(out.c_str(), &claimParent)) return 2;
 fault = ClaimDestination;
 r = Run(job, {});
 fault = None;
 check(injected && !r.completed && !r.errors.empty() &&
       readBytes(out + "/item") == "other" && readBytes(sourceArchive) == original,
       "archive move never replaces a newly claimed local name or removes the member");
 unlink((out+"/item").c_str());bool damaged=false;Hooks corrupt;corrupt.proceed=[&]{struct stat st;if(!damaged&&!lstat((out+"/item").c_str(),&st)&&readBytes(out+"/item")==payload){damaged=true;std::ofstream(out+"/item")<<std::string(payload.size(),'X');}return true;};
 r=Run(job,corrupt);check(damaged&&!r.completed&&!r.errors.empty()&&readBytes(sourceArchive)==original,"same-size local destination corruption blocks source cleanup");
 // Cleanup can fail after an earlier local root completed. Report only that root.
 std::ofstream(root+"/in/first")<<"first";std::ofstream(root+"/in/item")<<payload;
 auto d=PrepareArchiveJob(false,{root+"/in/keep"});d.archiveFormat="zip";SetArchiveDestination(d,root,"partial");if(Run(d,{}).completed!=1)return 2;
 char *resolved=realpath((root+"/in/item").c_str(),nullptr);deletePath=resolved;free(resolved);injected=false;
 job=PrepareArchiveMove(Job{Action::Move,{root+"/in/first",root+"/in/item"},OpenArchive(d.destination)});fault=LocalDelete;r=Run(job,{});fault=None;
 check(injected&&r.completed==1&&r.completedRoots.size()==1&&r.completedRoots[0].first==root+"/in/first"&&!r.errors.empty(),"partial local cleanup reports only completed source root");
 check(readBytes(root+"/in/item")==payload&&access((root+"/in/first").c_str(),F_OK)!=0,"partial cleanup retains remaining source without restoring stale data");
 clean(root);std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;
}
