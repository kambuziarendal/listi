// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "VirtualFile.h"
#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <Path.h>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using namespace Listi;
namespace {
enum Fault { None, DestinationRename, SourceRename, CorruptDestination } fault=None;
std::string sourceArchive,destinationArchive;
bool injected=false;
const std::string payload="Listi move fault payload";
std::string readBytes(const std::string &path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};}
void clean(const std::string &p){BDirectory d(p.c_str());BEntry e;while(d.GetNextEntry(&e,false)==B_OK){struct stat s;e.GetStat(&s);if(S_ISDIR(s.st_mode)){BPath path;e.GetPath(&path);clean(path.Path());}e.Remove();}rmdir(p.c_str());}
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
int main(){
 BApplication app("application/x-vnd.Listi-archive-move-fault-test");char pattern[]="/tmp/Listi-move-fault-XXXXXX";auto made=mkdtemp(pattern);if(!made)return 2;std::string root=made;
 mkdir((root+"/in").c_str(),0700);std::ofstream(root+"/in/item")<<payload;std::ofstream(root+"/in/keep")<<"keep";
 int checks=0,failures=0;auto check=[&](bool ok,const std::string&label){++checks;failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<label<<std::endl;};
 for(auto mode:{DestinationRename,SourceRename,CorruptDestination}){
  fault=None;injected=false;
  auto s=PrepareArchiveJob(false,{root+"/in/item",root+"/in/keep"});s.archiveFormat="zip";SetArchiveDestination(s,root,"source"+std::to_string(mode));
  auto d=PrepareArchiveJob(false,{root+"/in/keep"});d.archiveFormat="tar";SetArchiveDestination(d,root,"dest"+std::to_string(mode));
  if(Run(s,{}).completed!=1||Run(d,{}).completed!=1)return 2;
  sourceArchive=s.destination;destinationArchive=d.destination;
  auto source=OpenArchive(sourceArchive),target=OpenArchive(destinationArchive),item=ChildLocation(source,"item");
  auto original=readBytes(sourceArchive),targetBefore=readBytes(destinationArchive);
  auto job=PrepareArchiveMove(Job{Action::Move,{item},target});fault=mode;int completions=0;
  Hooks hooks;hooks.fileDone=[&](const std::string&,const std::string&){++completions;};auto result=Run(job,hooks);fault=None;
  check(injected&&!result.errors.empty()&&!result.completed&&result.completedRoots.empty()&&!completions,"fault "+std::to_string(mode)+" never reports successful move");
  check(readBytes(sourceArchive)==original,"fault "+std::to_string(mode)+" retains source archive bytes exactly");
  if(mode==DestinationRename)check(readBytes(destinationArchive)==targetBefore,"destination publication ENOSPC keeps prior archive");
  if(mode==SourceRename)check(readBytes(destinationArchive)!=targetBefore,"source publication failure retains committed destination copy");
  if (mode == CorruptDestination) {
    bool detected = false;
    for (const auto &error : result.errors) {
      // Publication can now reject changed output before the move's byte comparison.
      if (error.find("verified contents could not be confirmed") != std::string::npos ||
          error.find("Destination bytes differ") != std::string::npos)
        detected = true;
    }
    check(detected && readBytes(destinationArchive).find("X" + payload.substr(1)) != std::string::npos,
          "damaged published destination detected before source cleanup");
  }
 }
 clean(root);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
