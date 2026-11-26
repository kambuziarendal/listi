// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "LocalMoveTree.h"
#include "VirtualFile.h"
#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <Path.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
using namespace Listi;
static uint64 calls=0;static bool counting=false;
extern "C" int __real_lstat(const char*,struct stat*);
extern "C" int __wrap_lstat(const char* p,struct stat* st){if(counting)++calls;return __real_lstat(p,st);}
static size_t resolutions = 0;
extern "C" char *__real_realpath(const char *, char *);
extern "C" char *__wrap_realpath(const char *path, char *buffer) {
  if (counting)
    ++resolutions;
  return __real_realpath(path, buffer);
}

// Measure the destructive phase alone: unrelated selected roots must not be
// scanned for each unlink. A single directory still checks its entire tree.
bool checkLocalCleanup(const std::string &parent, int count, bool selectDirectory) {
  const auto tree = parent + (selectDirectory ? "/one-root" : "/loose-roots");
  if (mkdir(tree.c_str(), 0700))
    return false;
  std::vector<std::string> roots;
  for (int i = 0; i < count; ++i) {
    const auto path = tree + "/entry" + std::to_string(i);
    if (!(std::ofstream(path) << "test"))
      return false;
    roots.push_back(path);
  }
  if (selectDirectory)
    roots = {tree};

  LocalMoveTree captured(roots, {}, false);
  calls = resolutions = 0;
  counting = true;
  for (const auto &root : roots)
    captured.Remove(root, {});
  counting = false;
  std::cout << "local_cleanup selected=" << roots.size()
            << " files=" << count << " realpath_calls=" << resolutions
            << " lstat_calls=" << calls << std::endl;
  const auto entries = size_t(count) + (selectDirectory ? 1 : 0);
  if (resolutions > entries || calls > 20 * entries)
    return false;
  return selectDirectory ? access(tree.c_str(), F_OK) != 0 : rmdir(tree.c_str()) == 0;
}
void clean(const std::string &p){BDirectory d(p.c_str());BEntry e;while(d.GetNextEntry(&e,false)==B_OK){struct stat s;e.GetStat(&s);if(S_ISDIR(s.st_mode)){BPath path;e.GetPath(&path);clean(path.Path());}e.Remove();}rmdir(p.c_str());}
int main(){BApplication app("application/x-vnd.Listi-AuditScaleProbe");char base[]="/boot/home/Listi-scale-probe-XXXXXX";if(!mkdtemp(base))return 2;
 std::vector<int> sizes{64,128};
 if(const char *extra=getenv("LISTI_SCALE_FILES")) { sizes.clear();std::istringstream in(extra);int n;while(in>>n){if(n<1||n>5000)return 2;sizes.push_back(n);}if(sizes.empty())return 2; }
 size_t byteCount=1024;
 if(const char *n=getenv("LISTI_SCALE_BYTES")){byteCount=strtoull(n,nullptr,10);if(!byteCount||byteCount>67108864)return 2;}
 std::string payload(byteCount,'x');
 std::string format=getenv("LISTI_SCALE_FORMAT")?getenv("LISTI_SCALE_FORMAT"):"zip";
 if(format!="zip"&&format!="tar")return 2;
 try{for(int count:sizes){
 std::string root=std::string(base)+"/case"+std::to_string(count),tree=root+"/tree",out=root+"/out";
 mkdir(root.c_str(),0700);mkdir(tree.c_str(),0700);mkdir(out.c_str(),0700);
 if (!checkLocalCleanup(root, count, false) || !checkLocalCleanup(root, count, true))
   return 8;
 for(int i=0;i<count;++i)std::ofstream(tree+"/item"+std::to_string(i))<<payload;
 auto create=PrepareArchiveJob(false,{tree});create.archiveFormat=format;SetArchiveDestination(create,root,"fixture");auto made=Run(create,{});if(made.completed!=1||!made.errors.empty())return 3;
 auto archive=OpenArchive(create.destination);auto job=PrepareArchiveMove(Job{Action::Move,{ChildLocation(archive,"tree")},out});
 calls=0;counting=true;bigtime_t start=system_time();auto result=Run(job,{});auto elapsed=system_time()-start;counting=false;
 if(result.completed!=1||!result.errors.empty()){for(auto&e:result.errors)std::cerr<<e<<std::endl;return 4;}
 int verified=0;for(int i=0;i<count;++i){std::string content;ReadFile(out+"/tree/item"+std::to_string(i),[&](const char*p,size_t n){content.append(p,n);});if(content==payload)++verified;}
 FileEntry removed;bool sourceGone=!StatFile(ChildLocation(archive,"tree"),removed);
 std::cout<<"files="<<count<<" bytes_each="<<byteCount<<" format="<<format<<" lstat_calls="<<calls<<" elapsed_us="<<elapsed<<" verified="<<verified<<" source_removed="<<sourceGone<<std::endl;
 if(verified!=count||!sourceGone)return 5;
 // A generous structural work budget, not a wall-clock performance promise.
 if(calls > uint64_t(count)*500 + uint64_t(count)*((byteCount+65535)/65536)*32 + 10000) { std::cerr<<"FAIL repeated metadata work budget"<<std::endl; return 7; }
 }clean(base);std::cout<<sizes.size()*3<<" checks, 0 failures"<<std::endl;return 0;}catch(const std::exception&e){std::cerr<<e.what()<<std::endl;return 6;}}
