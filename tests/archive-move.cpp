// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "ArchiveVolumes.h"
#include "ArchivePasswords.h"
#include "FileClipboard.h"
#include "JobSnapshot.h"
#include "VirtualFile.h"
#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <Path.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
using namespace Listi;
namespace {
int checks=0, failures=0;
void check(bool ok,const std::string &label){++checks;failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<label<<std::endl;}
std::string content(const std::string &p){std::string s;ReadFile(p,[&](const char*b,size_t n){s.append(b,n);});return s;}
bool exists(const std::string &p){FileEntry e;return StatFile(p,e);}
void clean(const std::string &p){BDirectory d(p.c_str());BEntry e;while(d.GetNextEntry(&e,false)==B_OK){struct stat s;e.GetStat(&s);if(S_ISDIR(s.st_mode)){BPath path;e.GetPath(&path);clean(path.Path());}e.Remove();}rmdir(p.c_str());}
std::string root;int serial=0;
std::string archive(const std::string &format,const std::vector<std::string> &sources, bool encrypted=false){
 auto create=PrepareArchiveJob(false,sources);create.archiveFormat=format;SetArchiveDestination(create,root,"a"+std::to_string(++serial));
 if(encrypted){create.archiveEncryptNames=format=="7z";create.archivePasswordToken=HoldArchiveCreationPassword(std::make_shared<ArchiveSecret>("Listi-move-test"));}
 auto result=Run(create,{});if(result.completed!=1||!result.errors.empty())throw std::runtime_error("Create fixture: "+(result.errors.empty()?"no completion":result.errors.front()));
 return OpenArchive(create.destination);
}
Job move(const std::vector<std::string>&sources,const std::string&dest){return PrepareArchiveMove(Job{Action::Move,sources,dest});}
Result workerRun(const Job &job) {
 struct Work {const Job &job;Result result;} work{job,{}};
 pthread_attr_t attributes;pthread_attr_init(&attributes);pthread_attr_setstacksize(&attributes,262144);
 pthread_t thread;int status=pthread_create(&thread,&attributes,[](void *p)->void*{auto &w=*static_cast<Work*>(p);w.result=Run(w.job,{});return nullptr;},&work);
 pthread_attr_destroy(&attributes);if(status)throw std::runtime_error("Cannot start native worker test");pthread_join(thread,nullptr);return work.result;
}
std::string fingerprint(const std::string &url){return FindArchiveVolumes(ParseLocation(url).archive).Fingerprint();}
template<class F>bool rejects(F f){try{f();return false;}catch(const std::exception&){return true;}}
}
int main(){
 BApplication app("application/x-vnd.Listi-archive-move-test");char pattern[]="/tmp/Listi-move-XXXXXX";auto made=mkdtemp(pattern);if(!made)return 2;root=made;
 mkdir((root+"/in").c_str(),0700);mkdir((root+"/in/folder").c_str(),0700);mkdir((root+"/in/folder/empty").c_str(),0700);
 std::ofstream(root+"/in/keep")<<"retained";std::ofstream(root+"/in/item")<<"payload";std::ofstream(root+"/in/folder/nested")<<std::string(160000,'x');
 const auto input=root+"/in/";
 try {
  mkdir((input+"packed").c_str(),0700);
  const std::string nestedPayload(160000, 'x');
  std::ofstream(input+"packed/nested") << nestedPayload;
  // The positive TAR→ZIP fixtures use whole-second times representable by both.
  const timespec stamp[2]={{1700000000,0},{1700000000,0}};
  for(const auto &path:{input+"item",input+"folder/nested",input+"folder/empty",input+"folder"})
    if (utimensat(AT_FDCWD,path.c_str(),stamp,0)) return 2;
  for(const auto &format:WritableArchiveFormats()){
   const bool flat = format.id == "ar";
   const bool hasEmpty = !flat && format.id != "cab";
   // ar only stores regular files. CAB stores directories through file paths.
   const auto secondInput = input + (flat ? "folder/nested" : hasEmpty ? "folder" : "packed");
   const auto name = Leaf(secondInput);
   auto source = archive(format.id, {input+"keep", input+"item", secondInput});
   auto target = archive("zip", {input+"keep"});
   auto item = ChildLocation(source, "item"), second = ChildLocation(source, name);
   auto targetItem = ChildLocation(target, "item"), targetSecond = ChildLocation(target, name);
   auto job = move({item, second}, target);
   auto result = workerRun(DecodeJob(EncodeJob(job,0)));
   if(!result.errors.empty())std::cerr<<result.errors.front()<<std::endl;
   check(result.completed==2 && result.errors.empty() && !result.cancelled,format.id+" cross-container move completes selected roots");
   check(!exists(item) && !exists(second) && content(ChildLocation(source,"keep"))=="retained",format.id+" source removes only selected entries");
   check(content(targetItem)=="payload" && content(flat ? targetSecond : ChildLocation(targetSecond,"nested"))==nestedPayload,format.id+" destination has both payloads");
   if (hasEmpty) check(exists(ChildLocation(targetSecond,"empty")),format.id+" empty directory preserved");
   check(result.completedRoots.size()==2&&result.completedRoots[0].first==item,format.id+" completion names original virtual roots");
   check(PrepareArchiveEditTarget(ParseLocation(source).archive).archiveFormat==format.id,format.id+" source retains container format");
   auto back = Run(move({targetItem, targetSecond},source),{});
   check(back.completed==2 && back.errors.empty() && content(item)=="payload" &&
         content(flat ? second : ChildLocation(second,"nested"))==nestedPayload &&
         !exists(targetItem) && !exists(targetSecond),format.id+" reverse move into writer");
  }
  for(const auto &format:WritableArchiveFormats())if(format.encryption){
   SetArchivePasswordPrompt([](const std::string&,bool,const TransferControl&){return std::make_shared<ArchiveSecret>("Listi-move-test");});
   auto es=archive(format.id,{input+"keep",input+"item"},true),ed=archive(format.id,{input+"keep"},true);
   auto ei=ChildLocation(es,"item");auto ej=move({ei},ed);auto beforeSource=fingerprint(es),beforeTarget=fingerprint(ed);
   ForgetArchivePasswords();SetArchivePasswordPrompt([](const std::string&,bool,const TransferControl&){return ArchiveSecretPtr{};});
   auto cancelled=Run(ej,{});check(cancelled.cancelled&&!cancelled.completed&&fingerprint(es)==beforeSource&&fingerprint(ed)==beforeTarget,format.id+" cancelled password prompt preserves both encrypted archives");
   SetArchivePasswordPrompt([](const std::string&,bool,const TransferControl&){return std::make_shared<ArchiveSecret>("Listi-move-test");});
   auto result=workerRun(move({ei},ed));check(result.completed==1&&result.errors.empty()&&!exists(ei),format.id+" move between encrypted archives");
   ForgetArchivePasswords();int prompts=0;
   SetArchivePasswordPrompt([&](const std::string&,bool,const TransferControl&){++prompts;return std::make_shared<ArchiveSecret>("Listi-move-test");});
   check(content(ChildLocation(es,"keep"))=="retained"&&content(ChildLocation(ed,"item"))=="payload"&&prompts==2,format.id+" source and destination retain encryption after move");
   ForgetArchivePasswords();SetArchivePasswordPrompt({});
  }
  auto emptySource=archive("zip",{input+"item"}),emptyTarget=archive("tgz",{input+"keep"});
  auto emptied=Run(move({ChildLocation(emptySource,"item")},emptyTarget),{});
  check(emptied.completed==1&&ListFiles(emptySource).empty(),"moving last member leaves a readable empty source archive");
  auto s=archive("zip",{input+"keep",input+"item",input+"folder"});auto d=archive("tgz",{input+"keep"});auto item=ChildLocation(s,"item");
  auto j=move({item},d);auto sb=fingerprint(s),db=fingerprint(d);Hooks cancel;cancel.proceed=[] {return false;};
  auto r=Run(j,cancel);check(r.cancelled&&!r.completed&&fingerprint(s)==sb&&fingerprint(d)==db,"cancel before copying preserves both archives");
  auto stale=j;auto change=PrepareArchiveCopy(Job{Action::Copy,{input+"folder"},d});Run(change,{});
  r=Run(stale,{});check(!r.errors.empty()&&!r.completed&&exists(item),"queued destination replacement rejects stale move");
  j=move({item},d);db=fingerprint(d);Hooks afterCopy;
  afterCopy.proceed=[&]{return fingerprint(d)==db;};
  r=Run(j,afterCopy);check(r.cancelled&&!r.completed&&exists(item)&&content(ChildLocation(d,"item"))=="payload","cancel after destination commit retains both copies and reports no move");
  j=move({item},d);sb=fingerprint(s);db=fingerprint(d);Hooks skip;skip.conflict=[](const std::string&){return Decision::Skip;};
  r=Run(j,skip);check(!r.errors.empty()&&!r.completed&&fingerprint(s)==sb&&fingerprint(d)==db,"skip conflict does not remove source or commit partial destination");
  Hooks abort;abort.conflict=[](const std::string&){return Decision::Abort;};r=Run(move({item},d),abort);
  check(r.cancelled&&!r.completed&&fingerprint(s)==sb&&fingerprint(d)==db,"abort conflict preserves both archives");
  Hooks replace;replace.conflict=[](const std::string&){return Decision::Replace;};r=Run(move({item},d),replace);
  check(r.completed==1&&r.errors.empty()&&!exists(item)&&content(ChildLocation(d,"item"))=="payload","explicit replace completes verified move");
  // Source changes after copy: never remove entries from the newer source.
  s=archive("zip",{input+"keep",input+"item"});d=archive("tgz",{input+"keep"});item=ChildLocation(s,"item");j=move({item},d);db=fingerprint(d);bool injected=false;
  Hooks sourceChange;sourceChange.proceed=[&]{if(!injected&&fingerprint(d)!=db){injected=true;Run(PrepareArchiveCopy(Job{Action::Copy,{input+"folder"},s}),{});}return true;};
  r=Run(j,sourceChange);check(injected&&!r.errors.empty()&&!r.completed&&exists(item)&&exists(ChildLocation(s,"folder"))&&content(ChildLocation(d,"item"))=="payload","source changed after copy is retained with destination copy");
  // Destination replacement after commit invalidates cleanup guards.
  s=archive("zip",{input+"keep",input+"item"});d=archive("tgz",{input+"keep"});item=ChildLocation(s,"item");j=move({item},d);db=fingerprint(d);injected=false;
  Hooks targetChange;targetChange.proceed=[&]{if(!injected&&fingerprint(d)!=db){injected=true;Run(PrepareArchiveCopy(Job{Action::Copy,{input+"folder"},d}),{});}return true;};
  r=Run(j,targetChange);check(injected&&!r.errors.empty()&&!r.completed&&exists(item)&&exists(ChildLocation(d,"folder")),"destination changed after copy blocks source cleanup");
  // Cancellation during source rewrite also leaves original source intact.
  s=archive("zip",{input+"keep",input+"item"});d=archive("tgz",{input+"keep"});item=ChildLocation(s,"item");bool stop=false;Hooks cleanupCancel;
  cleanupCancel.progress=[&](const std::string&p,uint64_t){if(p==ParseLocation(s).archive)stop=true;};cleanupCancel.proceed=[&]{return !stop;};
  r=Run(move({item},d),cleanupCancel);check(stop&&r.cancelled&&!r.completed&&exists(item)&&content(ChildLocation(d,"item"))=="payload","cancel during source rebuild retains both copies");
  // Preflight rejects destructive ambiguities and unsupported endpoints.
  auto folder=ChildLocation(s,"folder");Run(PrepareArchiveCopy(Job{Action::Copy,{input+"folder"},s}),{});
  check(rejects([&]{move({folder,ChildLocation(folder,"nested")},d);}),"overlapping selection rejected");
  check(rejects([&]{move({s},d);}),"archive root cannot be moved");
  check(move({item},root).action==Action::MoveArchive,"archive cut to local folder prepares verified move");
  check(move({input+"item"},d).action==Action::MoveArchive,"local source prepares verified archive move");
  check(rejects([&]{move({folder},ChildLocation(folder,"empty"));}),"same-archive self descendant rejected");
  const auto alias=root+"/alias.zip";link(ParseLocation(s).archive.c_str(),alias.c_str());
  check(rejects([&]{move({item},OpenArchive(alias));}),"hardlinked archive aliases rejected");unlink(alias.c_str());
  // Clipboard lifecycle: pending, cancel, retry, success, and newer selection.
  BClipboard clipboard("Listi-archive-move-test",true);
  auto csrc=archive("zip",{input+"keep",input+"item",input+"folder"});auto cdst=archive("tgz",{input+"keep"});auto ci=ChildLocation(csrc,"item");
  PutPathsAsText({"previous contents"}, &clipboard);
  std::string deep = csrc;
  for (int level = 0; level < 9; ++level)
    deep = ChildLocation(deep, std::string(240, 'd'));
  std::vector<std::string> largeSelection;
  for (size_t i = 0; i < kFileSelectionLimit; ++i)
    largeSelection.push_back(ChildLocation(deep, std::to_string(i)));
  check(rejects([&] { PutFileClipboard(largeSelection, false, &clipboard); }) &&
        ReadClipboardText(&clipboard) == "previous contents",
        "oversized archive Copy leaves the previous clipboard intact");
  PutFileClipboard({ci},true,&clipboard);auto paste=PrepareFilePaste(cdst,&clipboard);
  check(paste.archiveCut&&!paste.lease.empty()&&paste.jobs.size()==1&&paste.jobs[0].action==Action::MoveArchive,"archive Cut prepares leased move job");
  check(rejects([&]{PrepareFilePaste(cdst,&clipboard);}),"duplicate pending paste refused");
  FinishFilePaste(paste,{},&clipboard);paste=PrepareFilePaste(cdst,&clipboard);
  check(!paste.lease.empty(),"cancel confirmation releases clipboard lease for retry");
  r=Run(paste.jobs[0],{});std::vector<std::string> moved;for(auto &entry:r.completedRoots)moved.push_back(entry.first);FinishFilePaste(paste,moved,&clipboard);
  check(r.completed==1&&rejects([&]{PrepareFilePaste(cdst,&clipboard);}),"successful move consumes archive Cut selection");
  auto cf=ChildLocation(csrc,"folder");PutFileClipboard({cf},true,&clipboard);paste=PrepareFilePaste(cdst,&clipboard);
  PutFileClipboard({input+"keep"},false,&clipboard);FinishFilePaste(paste,{cf},&clipboard);
  auto newer=PrepareFilePaste(root,&clipboard);check(newer.jobs.size()==1&&!newer.archiveCut&&newer.lease.empty()&&content(newer.jobs[0].sources[0])=="retained","late completion preserves newer clipboard selection");
  PutFileClipboard({cf},true,&clipboard);
  check(rejects([&]{PrepareFilePaste(ChildLocation(ChildLocation(csrc,"folder"),"empty"),&clipboard);}),"clipboard same-archive descendant refused before lease");
  auto ck=ChildLocation(csrc,"keep");PutFileClipboard({ck},true,&clipboard);
  paste=PrepareFilePaste(cf,&clipboard);r=Run(paste.jobs[0],{});moved.clear();for(auto &entry:r.completedRoots)moved.push_back(entry.first);FinishFilePaste(paste,moved,&clipboard);
  check(r.completed==1&&!exists(ck)&&content(ChildLocation(cf,"keep"))=="retained"&&rejects([&]{PrepareFilePaste(cdst,&clipboard);}),"same-archive clipboard Cut moves and consumes selection");
  PutFileClipboard({cf},true,&clipboard);Run(PrepareArchiveCopy(Job{Action::Copy,{input+"item"},csrc}),{});
  check(rejects([&]{PrepareFilePaste(cdst,&clipboard);}),"archive changed since Cut is rejected");
  PutFileClipboard({cf},false,&clipboard);paste=PrepareFilePaste(cdst,&clipboard);r=Run(paste.jobs[0],{});
  check(r.completed==1&&exists(cf)&&exists(ChildLocation(cdst,"folder")),"ordinary archive Copy still retains source and requires no cut lease");
 }catch(const std::exception &e){check(false,std::string("unexpected exception: ")+e.what());}

 clean(root);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
